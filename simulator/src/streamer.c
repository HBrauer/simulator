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
#include <stdatomic.h>
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
    iq_ci16_t samples[];
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

static void *stream_render_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->render_cpu);
    stream_block_record_t *record = calloc(1, sizeof(*record) + worker->packet_bytes);
    if (record == NULL) {
        record_worker_error(worker, "render buffer allocation failed");
        return NULL;
    }

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

        if (ringbuffer_available(&worker->ringbuffer) == 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = RENDER_BACKPRESSURE_NS};
            nanosleep(&ts, NULL);
            continue;
        }

        const uint64_t render_time_ns = streamer_block_start_ns(block_index, block_samples, sample_rate_hz);
        render_stats_t stats;
        renderer_render_channel_block(worker->scenario, worker->asset_cache, &receiver_snapshot, &channel_snapshot, render_time_ns, record->samples, block_samples, &stats);
        record->sample_count = block_samples;

        if (!ringbuffer_try_push(&worker->ringbuffer, render_time_ns, record)) {
            record_overrun(worker, block_samples);
        }

        block_index = streamer_next_block_index(block_index, block_samples, sample_rate_hz);
    }

    free(record);
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
    };
    uint8_t packet[64];
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

    udp_output_t udp = {.fd = -1};
    if (!udp_output_open(&udp, host, port, receiver_snapshot.udp_multicast_interface)) {
        record_worker_error(worker, "UDP socket open failed");
        return NULL;
    }

    const size_t send_capacity = vita49_if_data_packet_size(worker->block_samples, worker->class_id_present);
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

        size_t batch_count = 0;
        while (batch_count < STREAM_SEND_BATCH_SIZE) {
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
    worker->packet_bytes = worker->block_samples * sizeof(iq_ci16_t);
    if (!ringbuffer_init(&worker->ringbuffer, sizeof(stream_block_record_t) + worker->packet_bytes, RINGBUFFER_PACKET_CAPACITY)) {
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
