#include "streamer.h"
#include "receiver.h"
#include "renderer.h"
#include "ringbuffer.h"
#include "udp_output.h"
#include "vita49_packet.h"

#ifdef __linux__
#include <sched.h>
#endif
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RENDER_BACKPRESSURE_NS 50000L
#define RINGBUFFER_PACKET_CAPACITY 8U
#define STREAM_SEND_BATCH_SIZE 16U
#define CONTEXT_PACKET_INTERVAL_NS 1000000000ULL
/* Low-rate channels shrink their block below the configured size so block latency, pacing,
 * and the context heartbeat stay bounded (~>= 4 blocks/s); never below this floor. */
#define STREAM_MIN_BLOCK_SAMPLES 64U

/* Ringbuffer record: rendered payload prefixed with its actual sample count, which can be
 * below the configured block size for low-rate channels (and can change on a REST rate
 * switch while records are still in flight to the UDP thread). */
typedef struct {
    uint64_t sample_count;
    /* Rendered samples in the channel's native format (iq_ci16_t / iq_ci24_t / iq_cf32_t); the
     * slot is sized by sim_internal_bytes_per_sample() at worker start. The leading uint64_t
     * keeps this region 8-byte aligned for the int32/float element types. */
    unsigned char samples[];
} stream_block_record_t;

typedef struct {
    atomic_bool *running;
    const scenario_t *scenario;
    const asset_cache_t *asset_cache;
    const timebase_t *timebase;
    receiver_config_t *receiver;
    receiver_metrics_t *metrics;
    stream_metrics_t *stream_metrics;
    size_t channel_index;
    pthread_mutex_t *receiver_lock;
    size_t block_samples;
    uint64_t max_batch_latency_ns;
    uint32_t render_threads;
    size_t packet_bytes;
    ringbuffer_t ringbuffer;
    pthread_t render_thread;
    pthread_t udp_thread;
    bool render_started;
    bool udp_started;
    bool ringbuffer_initialized;
    int render_cpu;
    int udp_cpu;
    uint8_t vita_sequence;
    uint8_t context_sequence;
    bool class_id_present;
    vita49_class_id_t class_id;
} stream_worker_t;

struct streamer_manager {
    atomic_bool running;
    size_t worker_count;
    stream_worker_t workers[SIM_MAX_RECEIVERS * SIM_MAX_CHANNELS];
};

size_t streamer_block_samples_for_rate(size_t configured_block_samples, uint32_t sample_rate_hz)
{
    if (configured_block_samples == 0 || sample_rate_hz == 0) {
        return configured_block_samples;
    }
    /* Largest power of two giving at least ~4 blocks per second (<= rate/4 samples),
     * floored at STREAM_MIN_BLOCK_SAMPLES. Only rates below 4 * configured shrink. */
    size_t cap = STREAM_MIN_BLOCK_SAMPLES;
    while (cap * 2U <= (size_t)(sample_rate_hz / 4U)) {
        cap *= 2U;
    }
    return configured_block_samples < cap ? configured_block_samples : cap;
}

size_t streamer_batch_blocks_for_latency(size_t block_samples, uint32_t sample_rate_hz, uint64_t max_batch_latency_ns)
{
    if (max_batch_latency_ns == 0U) {
        return SIZE_MAX;
    }
    const uint64_t block_duration_ns = streamer_block_duration_ns(block_samples, sample_rate_hz);
    if (block_duration_ns == 0U) {
        return SIZE_MAX;
    }
    const uint64_t blocks = max_batch_latency_ns / block_duration_ns;
    if (blocks <= 1ULL) {
        return 1U;
    }
    return blocks > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)blocks;
}

uint64_t streamer_block_duration_ns(size_t block_samples, uint32_t sample_rate_hz)
{
    if (block_samples == 0 || sample_rate_hz == 0) {
        return 0;
    }
    if (block_samples > UINT64_MAX / 1000000000ULL) {
        return UINT64_MAX;
    }
    const uint64_t sample_ns = (uint64_t)block_samples * 1000000000ULL;
    return (sample_ns + (uint64_t)sample_rate_hz - 1ULL) / (uint64_t)sample_rate_hz;
}

#define STREAM_DAY_NS (86400ULL * 1000000000ULL)

uint64_t streamer_block_index_from_time_ns(uint64_t scenario_time_ns, size_t block_samples, uint32_t sample_rate_hz)
{
    if (block_samples == 0 || sample_rate_hz == 0) {
        return 0;
    }
    const uint64_t sample_index =
        (uint64_t)(((__uint128_t)scenario_time_ns * (uint64_t)sample_rate_hz) / 1000000000ULL);
    return sample_index / (uint64_t)block_samples;
}

uint64_t streamer_block_start_ns(uint64_t block_index, size_t block_samples, uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return 0;
    }
    const __uint128_t samples = (__uint128_t)block_index * (uint64_t)block_samples;
    return (uint64_t)((samples * 1000000000ULL) / (uint64_t)sample_rate_hz);
}

/* Advance to the next grid block, wrapping back to the first block of the day at midnight
 * so the rendered scenario time stays a valid time-of-day in [0, 86400 s). */
static uint64_t streamer_next_block_index(uint64_t block_index, size_t block_samples, uint32_t sample_rate_hz)
{
    const uint64_t next = block_index + 1ULL;
    if (streamer_block_start_ns(next, block_samples, sample_rate_hz) >= STREAM_DAY_NS) {
        return 0;
    }
    return next;
}

static void sleep_for_block(size_t block_samples, uint32_t sample_rate_hz)
{
    const uint64_t duration_ns = streamer_block_duration_ns(block_samples, sample_rate_hz);
    if (duration_ns == 0) {
        return;
    }
    struct timespec ts = {
        .tv_sec = (time_t)(duration_ns / 1000000000ULL),
        .tv_nsec = (long)(duration_ns % 1000000000ULL),
    };
    nanosleep(&ts, NULL);
}

static uint64_t monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until_monotonic_ns(uint64_t deadline_ns)
{
    const uint64_t now_ns = monotonic_now_ns();
    if (deadline_ns <= now_ns) {
        return;
    }
    const uint64_t sleep_ns = deadline_ns - now_ns;
    struct timespec ts = {
        .tv_sec = (time_t)(sleep_ns / 1000000000ULL),
        .tv_nsec = (long)(sleep_ns % 1000000000ULL),
    };
    nanosleep(&ts, NULL);
}

static void apply_stream_affinity(int stream_cpu)
{
#ifdef __linux__
    if (stream_cpu < 0) {
        return;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET((unsigned int)stream_cpu, &cpuset);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)stream_cpu;
#endif
}

static int next_stream_cpu(const streamer_config_t *config, size_t *index)
{
    if (config->stream_cpus == NULL || config->stream_cpu_count == 0) {
        return -1;
    }
    return config->stream_cpus[(*index)++ % config->stream_cpu_count];
}

static void stream_worker_set_active(stream_worker_t *worker, bool active)
{
    const bool was_active = atomic_exchange(&worker->stream_metrics->active, active);
    if (active && !was_active) {
        atomic_fetch_add(&worker->metrics->active_streams, 1);
    } else if (!active && was_active) {
        atomic_fetch_sub(&worker->metrics->active_streams, 1);
    }
}

static void record_worker_error(stream_worker_t *worker, const char *reason)
{
    atomic_fetch_add(&worker->metrics->worker_errors, 1);
    atomic_fetch_add(&worker->stream_metrics->worker_errors, 1);
    fprintf(stderr, "error: stream worker (receiver %u, channel %zu) failed: %s\n", worker->receiver->id, worker->channel_index, reason);
}

static void record_overrun(stream_worker_t *worker, uint64_t block_samples)
{
    atomic_fetch_add(&worker->metrics->ringbuffer_overruns, 1);
    atomic_fetch_add(&worker->metrics->samples_dropped, block_samples);
    atomic_fetch_add(&worker->stream_metrics->ringbuffer_overruns, 1);
    atomic_fetch_add(&worker->stream_metrics->samples_dropped, block_samples);
}

static void record_underrun(stream_worker_t *worker, uint64_t block_samples)
{
    atomic_fetch_add(&worker->metrics->ringbuffer_underruns, 1);
    atomic_fetch_add(&worker->metrics->samples_missed, block_samples);
    atomic_fetch_add(&worker->stream_metrics->ringbuffer_underruns, 1);
    atomic_fetch_add(&worker->stream_metrics->samples_missed, block_samples);
}

static void record_send_drop(stream_worker_t *worker, int error_code, uint64_t block_samples)
{
    atomic_fetch_add(&worker->metrics->udp_send_errors, 1);
    atomic_fetch_add(&worker->metrics->samples_dropped, block_samples);
    atomic_fetch_add(&worker->metrics->samples_send_dropped, block_samples);
    atomic_fetch_add(&worker->stream_metrics->udp_send_errors, 1);
    atomic_fetch_add(&worker->stream_metrics->samples_dropped, block_samples);
    atomic_fetch_add(&worker->stream_metrics->samples_send_dropped, block_samples);
    if (error_code == EAGAIN || error_code == EWOULDBLOCK) {
        atomic_fetch_add(&worker->metrics->udp_send_would_block, 1);
        atomic_fetch_add(&worker->stream_metrics->udp_send_would_block, 1);
    } else if (error_code == ENOBUFS) {
        atomic_fetch_add(&worker->metrics->udp_send_no_buffer, 1);
        atomic_fetch_add(&worker->stream_metrics->udp_send_no_buffer, 1);
    } else {
        atomic_fetch_add(&worker->metrics->udp_send_other_errors, 1);
        atomic_fetch_add(&worker->stream_metrics->udp_send_other_errors, 1);
    }
}

static void record_late_sample_count(stream_worker_t *worker, uint64_t samples)
{
    if (samples == 0U) {
        return;
    }
    atomic_fetch_add(&worker->metrics->samples_late, samples);
    atomic_fetch_add(&worker->metrics->samples_missed, samples);
    atomic_fetch_add(&worker->stream_metrics->samples_late, samples);
    atomic_fetch_add(&worker->stream_metrics->samples_missed, samples);
}

static void pace_or_record_late(stream_worker_t *worker, uint64_t *next_send_ns, uint64_t period_duration_ns, uint64_t period_samples)
{
    if (period_duration_ns == 0U || period_samples == 0U || *next_send_ns > UINT64_MAX - period_duration_ns) {
        *next_send_ns = monotonic_now_ns();
        return;
    }
    *next_send_ns += period_duration_ns;
    const uint64_t now_ns = monotonic_now_ns();
    if (*next_send_ns + period_duration_ns < now_ns) {
        const uint64_t late_ns = now_ns - *next_send_ns;
        record_late_sample_count(worker, (late_ns / period_duration_ns) * period_samples);
        *next_send_ns = now_ns;
    } else {
        sleep_until_monotonic_ns(*next_send_ns);
    }
}

static void snapshot_worker_config(stream_worker_t *worker, receiver_config_t *receiver, channel_config_t *channel)
{
    pthread_mutex_lock(worker->receiver_lock);
    *receiver = *worker->receiver;
    pthread_mutex_unlock(worker->receiver_lock);
    *channel = receiver->channels[worker->channel_index];
}

/* One cooperative renderer in a channel's render pool. The orchestrator (the channel's render
 * thread) renders one block of a fork-join round itself and hands the round's other blocks to
 * these helpers. Each helper renders a whole block independently -- a block is a pure function of
 * its index, so the renderer DSP is untouched and needs no per-thread state beyond its own output
 * buffer. The task pointers reference the orchestrator's per-round stack locals and are stable for
 * the duration of the go/done handshake (the orchestrator does not touch them until every helper
 * has posted done). */
typedef struct {
    stream_worker_t *worker;
    sem_t go;
    sem_t done;
    atomic_bool running;
    pthread_t thread;
    const receiver_config_t *receiver;
    const channel_config_t *channel;
    uint64_t render_time_ns;
    size_t block_samples;
    stream_block_record_t *record;
} render_pool_worker_t;

static void *render_pool_worker_main(void *arg)
{
    render_pool_worker_t *helper = arg;
    /* Do not pin: helpers float onto idle cores rather than share the orchestrator's core. */
    apply_stream_affinity(-1);
    render_stats_t stats;
    while (true) {
        sem_wait(&helper->go);
        if (!atomic_load(&helper->running)) {
            break;
        }
        renderer_render_channel_block(helper->worker->scenario, helper->worker->asset_cache,
                                      helper->receiver, helper->channel, helper->render_time_ns,
                                      helper->record->samples, helper->block_samples, &stats);
        helper->record->sample_count = helper->block_samples;
        sem_post(&helper->done);
    }
    return NULL;
}

static void *stream_render_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->render_cpu);

    uint32_t threads = worker->render_threads;
    if (threads < 1U) {
        threads = 1U;
    }
    if (threads > SIM_MAX_RENDER_THREADS) {
        threads = SIM_MAX_RENDER_THREADS;
    }

    /* One output buffer per cooperative renderer: records[0] for this thread, records[j] for
     * helper j-1. The ring copies each record on push, so the buffers are reused every round. */
    stream_block_record_t *records[SIM_MAX_RENDER_THREADS] = {0};
    for (uint32_t i = 0; i < threads; i++) {
        records[i] = calloc(1, sizeof(**records) + worker->packet_bytes);
        if (records[i] == NULL) {
            record_worker_error(worker, "render buffer allocation failed");
            for (uint32_t k = 0; k < i; k++) {
                free(records[k]);
            }
            return NULL;
        }
    }

    /* Spawn the helper pool (threads - 1 of them). A partial failure simply narrows the round
     * width to however many started; the channel still streams, just with less parallelism. */
    render_pool_worker_t *helpers = NULL;
    uint32_t started_helpers = 0;
    if (threads > 1U) {
        helpers = calloc(threads - 1U, sizeof(*helpers));
        if (helpers != NULL) {
            for (uint32_t j = 0; j < threads - 1U; j++) {
                helpers[j].worker = worker;
                helpers[j].record = records[j + 1U];
                atomic_init(&helpers[j].running, true);
                sem_init(&helpers[j].go, 0, 0);
                sem_init(&helpers[j].done, 0, 0);
                if (pthread_create(&helpers[j].thread, NULL, render_pool_worker_main, &helpers[j]) != 0) {
                    sem_destroy(&helpers[j].go);
                    sem_destroy(&helpers[j].done);
                    record_worker_error(worker, "render pool thread create failed");
                    break;
                }
                started_helpers++;
            }
        }
    }
    const size_t render_width = (size_t)started_helpers + 1U;

    uint32_t last_rate_hz = 0;
    uint64_t block_index = 0;
    bool grid_initialized = false;

    while (atomic_load(worker->running)) {
        receiver_config_t receiver_snapshot;
        channel_config_t channel_snapshot;
        snapshot_worker_config(worker, &receiver_snapshot, &channel_snapshot);
        const uint32_t sample_rate_hz = channel_snapshot.sample_rate_hz;
        /* Low rates render shorter blocks than configured (see
         * streamer_block_samples_for_rate); buffers stay sized for the configured maximum. */
        const size_t block_samples = streamer_block_samples_for_rate(worker->block_samples, sample_rate_hz);

        if (!channel_snapshot.stream_enabled) {
            stream_worker_set_active(worker, false);
            grid_initialized = false;
            sleep_for_block(block_samples, sample_rate_hz);
            continue;
        }
        stream_worker_set_active(worker, true);

        /* Re-anchor the grid to the current time-of-day whenever we (re)start streaming or
         * the sample rate changes (which also re-derives the block size); otherwise advance
         * block-by-block so the rendered content is a pure function of the block index (and
         * therefore identical across instances). */
        if (!grid_initialized || sample_rate_hz != last_rate_hz) {
            block_index = streamer_block_index_from_time_ns(timebase_now_ns(worker->timebase), block_samples, sample_rate_hz);
            last_rate_hz = sample_rate_hz;
            grid_initialized = true;
        }

        const size_t available = ringbuffer_available(&worker->ringbuffer);
        if (available == 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = RENDER_BACKPRESSURE_NS};
            nanosleep(&ts, NULL);
            continue;
        }
        size_t round = render_width < available ? render_width : available;

        /* Block indices for this round, walked through streamer_next_block_index so the UTC-day
         * wrap is handled exactly (never via block_index + j, which would not wrap). */
        uint64_t idxs[SIM_MAX_RENDER_THREADS];
        idxs[0] = block_index;
        for (size_t j = 1; j < round; j++) {
            idxs[j] = streamer_next_block_index(idxs[j - 1], block_samples, sample_rate_hz);
        }

        /* Dispatch blocks 1..round-1 to helpers, render block 0 on this thread, then join. */
        for (size_t j = 1; j < round; j++) {
            render_pool_worker_t *helper = &helpers[j - 1];
            helper->receiver = &receiver_snapshot;
            helper->channel = &channel_snapshot;
            helper->render_time_ns = streamer_block_start_ns(idxs[j], block_samples, sample_rate_hz);
            helper->block_samples = block_samples;
            sem_post(&helper->go);
        }
        {
            render_stats_t stats;
            const uint64_t render_time_ns = streamer_block_start_ns(idxs[0], block_samples, sample_rate_hz);
            renderer_render_channel_block(worker->scenario, worker->asset_cache, &receiver_snapshot, &channel_snapshot, render_time_ns, records[0]->samples, block_samples, &stats);
            records[0]->sample_count = block_samples;
        }
        for (size_t j = 1; j < round; j++) {
            sem_wait(&helpers[j - 1].done);
        }

        /* Push the round's blocks in index order; the ring had >= round free slots. */
        for (size_t j = 0; j < round; j++) {
            const uint64_t render_time_ns = streamer_block_start_ns(idxs[j], block_samples, sample_rate_hz);
            if (!ringbuffer_try_push(&worker->ringbuffer, render_time_ns, records[j])) {
                record_overrun(worker, block_samples);
            }
        }

        block_index = streamer_next_block_index(idxs[round - 1], block_samples, sample_rate_hz);
    }

    for (uint32_t j = 0; j < started_helpers; j++) {
        atomic_store(&helpers[j].running, false);
        sem_post(&helpers[j].go);
        pthread_join(helpers[j].thread, NULL);
        sem_destroy(&helpers[j].go);
        sem_destroy(&helpers[j].done);
    }
    free(helpers);
    for (uint32_t i = 0; i < threads; i++) {
        free(records[i]);
    }
    return NULL;
}

/* Context packets announce {rf reference frequency, bandwidth, sample rate} in-band: once
 * when a stream (re)starts, immediately when the channel configuration changes, and at a
 * ~1 s heartbeat otherwise. Change detection compares the configured fields, not the
 * instantaneous scan-swept center, so a scanning tuner does not spam a packet per block. */
typedef struct {
    bool sent;
    uint32_t bandwidth_hz;
    uint32_t sample_rate_hz;
    bool track_tuner;
    uint64_t center_frequency_hz;
    uint64_t sent_at_monotonic_ns;
} context_state_t;

static bool context_config_changed(const context_state_t *state, const channel_config_t *channel)
{
    return state->bandwidth_hz != channel->bandwidth_hz ||
        state->sample_rate_hz != channel->sample_rate_hz ||
        state->track_tuner != channel->track_tuner ||
        (!channel->track_tuner && state->center_frequency_hz != channel->center_frequency_hz);
}

static void maybe_send_context_packet(stream_worker_t *worker, udp_output_t *udp, const receiver_config_t *receiver, const channel_config_t *channel, context_state_t *state)
{
    const uint64_t now_ns = monotonic_now_ns();
    const bool changed = state->sent && context_config_changed(state, channel);
    const bool due = !state->sent || changed ||
        now_ns - state->sent_at_monotonic_ns >= CONTEXT_PACKET_INTERVAL_NS;
    if (!due) {
        return;
    }
    const uint64_t scenario_time_ns = timebase_now_ns(worker->timebase);
    const vita49_context_packet_t context = {
        .stream_id = channel->stream_id,
        .sequence = (uint8_t)(worker->context_sequence & 0x0fU),
        .timestamp_ns = scenario_time_ns,
        .changed = changed,
        .class_id_present = worker->class_id_present,
        .class_id = worker->class_id,
        .rf_reference_frequency_hz = receiver_channel_center_hz(receiver, channel, scenario_time_ns),
        .bandwidth_hz = channel->bandwidth_hz,
        .sample_rate_hz = channel->sample_rate_hz,
        .reference_level_dbm = channel->rf_reference_power_dbm,
        .format = channel->output_format,
    };
    uint8_t packet[80];
    size_t packet_bytes = 0;
    if (!vita49_write_context_packet(&context, packet, sizeof(packet), &packet_bytes)) {
        return;
    }
    size_t sent_bytes = 0;
    int error_code = 0;
    if (udp_output_send(udp, packet, packet_bytes, &sent_bytes, &error_code)) {
        worker->context_sequence = (uint8_t)((worker->context_sequence + 1U) & 0x0fU);
        atomic_fetch_add(&worker->metrics->udp_packets_sent, 1);
        atomic_fetch_add(&worker->metrics->udp_bytes_sent, sent_bytes);
        atomic_fetch_add(&worker->stream_metrics->udp_packets_sent, 1);
        atomic_fetch_add(&worker->stream_metrics->udp_bytes_sent, sent_bytes);
    }
    /* A dropped context packet is recovered by the heartbeat; do not skew sample metrics. */
    state->sent = true;
    state->bandwidth_hz = channel->bandwidth_hz;
    state->sample_rate_hz = channel->sample_rate_hz;
    state->track_tuner = channel->track_tuner;
    state->center_frequency_hz = channel->center_frequency_hz;
    state->sent_at_monotonic_ns = now_ns;
}

static void *stream_udp_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->udp_cpu);
    receiver_config_t receiver_snapshot;
    channel_config_t channel_snapshot;
    snapshot_worker_config(worker, &receiver_snapshot, &channel_snapshot);

    const char *host = receiver_snapshot.udp_output_host;
    const uint16_t port = channel_snapshot.udp_output.port;
    /* The on-wire sample format is fixed per channel (not a REST retune), so capture it once. */
    const sim_output_format_t output_format = channel_snapshot.output_format;

    udp_output_t udp = {.fd = -1};
    if (!udp_output_open(&udp, host, port, receiver_snapshot.udp_multicast_interface)) {
        record_worker_error(worker, "UDP socket open failed");
        return NULL;
    }

    const size_t send_capacity = vita49_if_data_packet_size(worker->block_samples, worker->class_id_present, output_format);
    uint8_t *packets = calloc(STREAM_SEND_BATCH_SIZE, send_capacity);
    if (packets == NULL) {
        record_worker_error(worker, "packet buffer allocation failed");
        udp_output_close(&udp);
        return NULL;
    }
    const size_t record_stride = sizeof(stream_block_record_t) + worker->packet_bytes;
    uint8_t *records = calloc(STREAM_SEND_BATCH_SIZE, record_stride);
    if (records == NULL) {
        record_worker_error(worker, "payload buffer allocation failed");
        free(packets);
        udp_output_close(&udp);
        return NULL;
    }
    uint64_t payload_timestamps[STREAM_SEND_BATCH_SIZE];
    uint64_t next_send_ns = monotonic_now_ns();
    context_state_t context_state = {0};

    while (atomic_load(worker->running)) {
        snapshot_worker_config(worker, &receiver_snapshot, &channel_snapshot);
        const size_t block_samples = streamer_block_samples_for_rate(worker->block_samples, channel_snapshot.sample_rate_hz);

        if (!channel_snapshot.stream_enabled) {
            ringbuffer_drain(&worker->ringbuffer);
            next_send_ns = monotonic_now_ns();
            context_state.sent = false; /* re-announce the configuration on re-enable */
            sleep_for_block(block_samples, channel_snapshot.sample_rate_hz);
            continue;
        }
        const uint64_t block_duration_ns = streamer_block_duration_ns(block_samples, channel_snapshot.sample_rate_hz);

        maybe_send_context_packet(worker, &udp, &receiver_snapshot, &channel_snapshot, &context_state);

        /* Bound the batch by both a fixed count (syscall amortisation) and a wall-clock span
         * (update latency), taking whichever is smaller. High rates hit the count cap; low rates
         * hit the time cap, so a 100 kS/s stream sends a block or two at a time instead of lumping
         * 16 blocks (~250 ms) into one paced burst that scrolls the waterfall in jerks. */
        size_t batch_cap = streamer_batch_blocks_for_latency(block_samples, channel_snapshot.sample_rate_hz, worker->max_batch_latency_ns);
        if (batch_cap > STREAM_SEND_BATCH_SIZE) {
            batch_cap = STREAM_SEND_BATCH_SIZE;
        }
        size_t batch_count = 0;
        while (batch_count < batch_cap) {
            uint8_t *record = records + batch_count * record_stride;
            if (!ringbuffer_try_pop(&worker->ringbuffer, &payload_timestamps[batch_count], record)) {
                break;
            }
            batch_count++;
        }

        if (batch_count == 0U) {
            record_underrun(worker, block_samples);
            pace_or_record_late(worker, &next_send_ns, block_duration_ns, block_samples);
            continue;
        }

        const void *send_data[STREAM_SEND_BATCH_SIZE];
        size_t send_lengths[STREAM_SEND_BATCH_SIZE];
        uint64_t send_samples[STREAM_SEND_BATCH_SIZE];
        size_t sent_messages = 0;
        size_t sent_bytes = 0;
        int send_error = 0;
        size_t prepared_count = 0;
        uint64_t batch_samples = 0;
        const uint32_t stream_id = channel_snapshot.stream_id;
        for (size_t i = 0; i < batch_count; i++) {
            uint8_t *packet = packets + i * send_capacity;
            /* Each record carries the sample count it was rendered with, which may differ
             * from the current snapshot across a rate change. */
            const stream_block_record_t *record = (const stream_block_record_t *)(records + i * record_stride);
            batch_samples += record->sample_count;
            const vita49_if_data_packet_t vita_packet = {
                .stream_id = stream_id,
                .sequence = (uint8_t)((worker->vita_sequence + (uint8_t)i) & 0x0fU),
                .timestamp_ns = payload_timestamps[i],
                .class_id_present = worker->class_id_present,
                .class_id = worker->class_id,
                .payload = record->samples,
                .payload_samples = (size_t)record->sample_count,
                .format = output_format,
            };
            size_t send_bytes = 0;
            if (!vita49_write_if_data_packet(&vita_packet, packet, send_capacity, &send_bytes)) {
                record_send_drop(worker, EINVAL, record->sample_count);
                continue;
            }
            send_data[prepared_count] = packet;
            send_lengths[prepared_count] = send_bytes;
            send_samples[prepared_count] = record->sample_count;
            prepared_count++;
        }

        bool batch_ok = prepared_count > 0U && udp_output_send_batch(&udp, send_data, send_lengths, prepared_count, &sent_messages, &sent_bytes, &send_error);
        if (sent_messages > 0U || batch_ok) {
            uint64_t sent_samples = 0;
            for (size_t i = 0; i < sent_messages; i++) {
                sent_samples += send_samples[i];
            }
            worker->vita_sequence = (uint8_t)((worker->vita_sequence + sent_messages) & 0x0fU);
            atomic_fetch_add(&worker->metrics->udp_packets_sent, sent_messages);
            atomic_fetch_add(&worker->metrics->udp_bytes_sent, sent_bytes);
            atomic_fetch_add(&worker->metrics->samples_rendered, sent_samples);
            atomic_fetch_add(&worker->metrics->samples_sent, sent_samples);
            atomic_fetch_add(&worker->stream_metrics->udp_packets_sent, sent_messages);
            atomic_fetch_add(&worker->stream_metrics->udp_bytes_sent, sent_bytes);
            atomic_fetch_add(&worker->stream_metrics->samples_rendered, sent_samples);
            atomic_fetch_add(&worker->stream_metrics->samples_sent, sent_samples);
        }
        if (!batch_ok) {
            for (size_t i = sent_messages; i < prepared_count; i++) {
                record_send_drop(worker, send_error, send_samples[i]);
            }
        }
        const uint64_t batch_duration_ns = streamer_block_duration_ns((size_t)batch_samples, channel_snapshot.sample_rate_hz);
        pace_or_record_late(worker, &next_send_ns, batch_duration_ns, batch_samples);
    }

    free(records);
    free(packets);
    udp_output_close(&udp);
    return NULL;
}

static bool stream_worker_start(stream_worker_t *worker)
{
    const sim_output_format_t format = worker->receiver->channels[worker->channel_index].output_format;
    worker->packet_bytes = worker->block_samples * sim_internal_bytes_per_sample(format);
    /* A parallel-render channel produces up to render_threads blocks per fork-join round; give
     * the ring enough slots to hold a few rounds so the renderers run ahead of the paced UDP
     * drain instead of stalling on a full ring. */
    size_t ring_capacity = RINGBUFFER_PACKET_CAPACITY;
    const size_t parallel_capacity = 4U * (size_t)worker->render_threads;
    if (parallel_capacity > ring_capacity) {
        ring_capacity = parallel_capacity;
    }
    if (!ringbuffer_init(&worker->ringbuffer, sizeof(stream_block_record_t) + worker->packet_bytes, ring_capacity)) {
        return false;
    }
    worker->ringbuffer_initialized = true;
    if (pthread_create(&worker->render_thread, NULL, stream_render_thread_main, worker) != 0) {
        return false;
    }
    worker->render_started = true;
    if (pthread_create(&worker->udp_thread, NULL, stream_udp_thread_main, worker) != 0) {
        return false;
    }
    worker->udp_started = true;
    return true;
}

static void stream_worker_join(stream_worker_t *worker)
{
    if (worker->render_started) {
        pthread_join(worker->render_thread, NULL);
        worker->render_started = false;
    }
    if (worker->udp_started) {
        pthread_join(worker->udp_thread, NULL);
        worker->udp_started = false;
    }
    stream_worker_set_active(worker, false);
    if (worker->ringbuffer_initialized) {
        ringbuffer_free(&worker->ringbuffer);
        worker->ringbuffer_initialized = false;
    }
}

bool streamer_manager_start(streamer_manager_t **manager, const streamer_config_t *config)
{
    streamer_manager_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return false;
    }
    atomic_init(&m->running, true);

    /* Spread the render and UDP threads across the configured CPU set (round-robin, render and
     * UDP threads of a worker on distinct CPUs) instead of pinning every thread to one core,
     * which serialised them. Empty set -> no pinning. Channel 0 (conventionally the widest) is
     * assigned first per receiver so it tends to land on its own core. */
    size_t next_cpu = 0;

    for (size_t i = 0; i < config->config->receiver_count; i++) {
        receiver_config_t *receiver = &config->config->receivers[i];
        for (size_t c = 0; c < receiver->channel_count; c++) {
            stream_worker_t *worker = &m->workers[m->worker_count++];
            const int render_cpu = next_stream_cpu(config, &next_cpu);
            const int udp_cpu = next_stream_cpu(config, &next_cpu);
            *worker = (stream_worker_t){
                .running = &m->running,
                .scenario = config->scenario,
                .asset_cache = config->asset_cache,
                .timebase = config->timebase,
                .receiver = receiver,
                .metrics = &config->metrics[i],
                .stream_metrics = &config->metrics[i].streams[c],
                .channel_index = c,
                .receiver_lock = config->receiver_lock,
                .block_samples = config->block_samples,
                .max_batch_latency_ns = config->max_batch_latency_ns,
                .render_threads = receiver->channels[c].render_threads > 0U ? receiver->channels[c].render_threads : 1U,
                .render_cpu = render_cpu,
                .udp_cpu = udp_cpu,
                .class_id_present = config->config->class_id_present,
                .class_id = {
                    .oui = config->config->class_id_oui,
                    .information_class_code = config->config->class_id_information_code,
                    .packet_class_code = config->config->class_id_packet_code,
                },
            };
            if (!stream_worker_start(worker)) {
                streamer_manager_stop(m);
                return false;
            }
        }
    }

    *manager = m;
    return true;
}

void streamer_manager_stop(streamer_manager_t *manager)
{
    if (manager == NULL) {
        return;
    }
    atomic_store(&manager->running, false);
    for (size_t i = 0; i < manager->worker_count; i++) {
        stream_worker_join(&manager->workers[i]);
    }
    free(manager);
}
