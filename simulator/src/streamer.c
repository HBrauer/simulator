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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RENDER_BACKPRESSURE_NS 50000L
#define RINGBUFFER_PACKET_CAPACITY 8U
#define STREAM_SEND_BATCH_SIZE 16U

typedef enum {
    STREAM_KIND_80MHZ,
    STREAM_KIND_DDC
} stream_kind_t;

typedef struct {
    atomic_bool *running;
    const scenario_t *scenario;
    const asset_cache_t *asset_cache;
    const timebase_t *timebase;
    receiver_config_t *receiver;
    receiver_metrics_t *metrics;
    stream_metrics_t *stream_metrics;
    size_t ddc_index;
    pthread_mutex_t *receiver_lock;
    stream_kind_t kind;
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
} stream_worker_t;

struct streamer_manager {
    atomic_bool running;
    size_t worker_count;
    stream_worker_t workers[SIM_MAX_RECEIVERS * (1 + SIM_DDC_COUNT)];
};

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

static uint32_t stream_sample_rate(const stream_worker_t *worker, const receiver_config_t *receiver, const ddc_config_t *ddc)
{
    return worker->kind == STREAM_KIND_80MHZ ? receiver->sample_rate_hz : ddc->sample_rate_hz;
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

static bool stream_enabled(const stream_worker_t *worker, const receiver_config_t *receiver, const ddc_config_t *ddc)
{
    return worker->kind == STREAM_KIND_80MHZ ? receiver->stream_enabled : ddc->stream_enabled;
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

static void render_one_block(const stream_worker_t *worker, iq_ci16_t *buffer, const receiver_config_t *receiver, const ddc_config_t *ddc, uint64_t scenario_time_ns)
{
    render_stats_t stats;
    if (worker->kind == STREAM_KIND_80MHZ) {
        renderer_render_80mhz_block(worker->scenario, worker->asset_cache, receiver, scenario_time_ns, buffer, worker->block_samples, &stats);
    } else {
        if (receiver_ddc_in_window(receiver, ddc, scenario_time_ns)) {
            renderer_render_ddc_block(worker->scenario, worker->asset_cache, ddc, scenario_time_ns, buffer, worker->block_samples, &stats);
        } else {
            memset(buffer, 0, worker->packet_bytes);
        }
    }
}

static void record_overrun(stream_worker_t *worker)
{
    atomic_fetch_add(&worker->metrics->ringbuffer_overruns, 1);
    atomic_fetch_add(&worker->metrics->samples_dropped, worker->block_samples);
    atomic_fetch_add(&worker->stream_metrics->ringbuffer_overruns, 1);
    atomic_fetch_add(&worker->stream_metrics->samples_dropped, worker->block_samples);
}

static void record_underrun(stream_worker_t *worker)
{
    atomic_fetch_add(&worker->metrics->ringbuffer_underruns, 1);
    atomic_fetch_add(&worker->metrics->samples_missed, worker->block_samples);
    atomic_fetch_add(&worker->stream_metrics->ringbuffer_underruns, 1);
    atomic_fetch_add(&worker->stream_metrics->samples_missed, worker->block_samples);
}

static void record_send_drop(stream_worker_t *worker, int error_code)
{
    atomic_fetch_add(&worker->metrics->udp_send_errors, 1);
    atomic_fetch_add(&worker->metrics->samples_dropped, worker->block_samples);
    atomic_fetch_add(&worker->metrics->samples_send_dropped, worker->block_samples);
    atomic_fetch_add(&worker->stream_metrics->udp_send_errors, 1);
    atomic_fetch_add(&worker->stream_metrics->samples_dropped, worker->block_samples);
    atomic_fetch_add(&worker->stream_metrics->samples_send_dropped, worker->block_samples);
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

static void *stream_render_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->render_cpu);
    iq_ci16_t *buffer = calloc(worker->block_samples, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    uint32_t last_rate_hz = 0;
    uint64_t block_index = 0;
    bool grid_initialized = false;

    while (atomic_load(worker->running)) {
        pthread_mutex_lock(worker->receiver_lock);
        receiver_config_t receiver_snapshot = *worker->receiver;
        ddc_config_t ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
        pthread_mutex_unlock(worker->receiver_lock);
        const uint32_t sample_rate_hz = stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot);

        if (!stream_enabled(worker, &receiver_snapshot, &ddc_snapshot)) {
            stream_worker_set_active(worker, false);
            grid_initialized = false;
            sleep_for_block(worker->block_samples, sample_rate_hz);
            continue;
        }
        stream_worker_set_active(worker, true);

        /* Re-anchor the grid to the current time-of-day whenever we (re)start streaming or
         * the sample rate changes; otherwise advance block-by-block so the rendered content
         * is a pure function of the block index (and therefore identical across instances). */
        if (!grid_initialized || sample_rate_hz != last_rate_hz) {
            block_index = streamer_block_index_from_time_ns(timebase_now_ns(worker->timebase), worker->block_samples, sample_rate_hz);
            last_rate_hz = sample_rate_hz;
            grid_initialized = true;
        }

        if (ringbuffer_available(&worker->ringbuffer) == 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = RENDER_BACKPRESSURE_NS};
            nanosleep(&ts, NULL);
            continue;
        }

        const uint64_t render_time_ns = streamer_block_start_ns(block_index, worker->block_samples, sample_rate_hz);
        render_one_block(worker, buffer, &receiver_snapshot, &ddc_snapshot, render_time_ns);

        if (!ringbuffer_try_push(&worker->ringbuffer, render_time_ns, buffer)) {
            record_overrun(worker);
        }

        block_index = streamer_next_block_index(block_index, worker->block_samples, sample_rate_hz);
    }

    free(buffer);
    return NULL;
}

static void *stream_udp_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->udp_cpu);
    pthread_mutex_lock(worker->receiver_lock);
    receiver_config_t receiver_snapshot = *worker->receiver;
    ddc_config_t ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
    pthread_mutex_unlock(worker->receiver_lock);

    const char *host = receiver_snapshot.udp_output_host;
    const uint16_t port = worker->kind == STREAM_KIND_80MHZ
        ? receiver_snapshot.udp_80mhz_output.port
        : ddc_snapshot.udp_output.port;

    udp_output_t udp = {.fd = -1};
    if (!udp_output_open(&udp, host, port, receiver_snapshot.udp_multicast_interface)) {
        return NULL;
    }

    const size_t send_capacity = vita49_if_data_packet_size(worker->block_samples);
    uint8_t *packets = calloc(STREAM_SEND_BATCH_SIZE, send_capacity);
    if (packets == NULL) {
        udp_output_close(&udp);
        return NULL;
    }
    uint8_t *payloads = calloc(STREAM_SEND_BATCH_SIZE, worker->packet_bytes);
    if (payloads == NULL) {
        free(packets);
        udp_output_close(&udp);
        return NULL;
    }
    uint64_t payload_timestamps[STREAM_SEND_BATCH_SIZE];
    uint64_t next_send_ns = monotonic_now_ns();

    while (atomic_load(worker->running)) {
        pthread_mutex_lock(worker->receiver_lock);
        receiver_snapshot = *worker->receiver;
        ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
        pthread_mutex_unlock(worker->receiver_lock);

        if (!stream_enabled(worker, &receiver_snapshot, &ddc_snapshot)) {
            ringbuffer_drain(&worker->ringbuffer);
            next_send_ns = monotonic_now_ns();
            sleep_for_block(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));
            continue;
        }
        const uint64_t block_duration_ns = streamer_block_duration_ns(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));

        size_t batch_count = 0;
        while (batch_count < STREAM_SEND_BATCH_SIZE) {
            uint8_t *payload = payloads + batch_count * worker->packet_bytes;
            if (!ringbuffer_try_pop(&worker->ringbuffer, &payload_timestamps[batch_count], payload)) {
                break;
            }
            batch_count++;
        }

        if (batch_count == 0U) {
            record_underrun(worker);
            pace_or_record_late(worker, &next_send_ns, block_duration_ns, worker->block_samples);
            continue;
        }

        const void *send_data[STREAM_SEND_BATCH_SIZE];
        size_t send_lengths[STREAM_SEND_BATCH_SIZE];
        size_t sent_messages = 0;
        size_t sent_bytes = 0;
        int send_error = 0;
        size_t prepared_count = 0;
        const uint32_t stream_id = vita49_stream_id(receiver_snapshot.id, worker->kind == STREAM_KIND_DDC, (uint32_t)worker->ddc_index);
        for (size_t i = 0; i < batch_count; i++) {
            uint8_t *packet = packets + i * send_capacity;
            const uint8_t *payload = payloads + i * worker->packet_bytes;
            const vita49_if_data_packet_t vita_packet = {
                .stream_id = stream_id,
                .sequence = (uint8_t)((worker->vita_sequence + (uint8_t)i) & 0x0fU),
                .timestamp_ns = payload_timestamps[i],
                .payload = (const iq_ci16_t *)payload,
                .payload_samples = worker->block_samples,
            };
            size_t send_bytes = 0;
            if (!vita49_write_if_data_packet(&vita_packet, packet, send_capacity, &send_bytes)) {
                record_send_drop(worker, EINVAL);
                continue;
            }
            send_data[prepared_count] = packet;
            send_lengths[prepared_count] = send_bytes;
            prepared_count++;
        }

        if (prepared_count > 0U && udp_output_send_batch(&udp, send_data, send_lengths, prepared_count, &sent_messages, &sent_bytes, &send_error)) {
            const uint64_t sent_samples = worker->block_samples * (uint64_t)sent_messages;
            worker->vita_sequence = (uint8_t)((worker->vita_sequence + sent_messages) & 0x0fU);
            atomic_fetch_add(&worker->metrics->udp_packets_sent, sent_messages);
            atomic_fetch_add(&worker->metrics->udp_bytes_sent, sent_bytes);
            atomic_fetch_add(&worker->metrics->samples_rendered, sent_samples);
            atomic_fetch_add(&worker->metrics->samples_sent, sent_samples);
            atomic_fetch_add(&worker->stream_metrics->udp_packets_sent, sent_messages);
            atomic_fetch_add(&worker->stream_metrics->udp_bytes_sent, sent_bytes);
            atomic_fetch_add(&worker->stream_metrics->samples_rendered, sent_samples);
            atomic_fetch_add(&worker->stream_metrics->samples_sent, sent_samples);
        } else {
            if (sent_messages > 0U) {
                const uint64_t sent_samples = worker->block_samples * (uint64_t)sent_messages;
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
            const size_t dropped_messages = prepared_count - sent_messages;
            for (size_t i = 0; i < dropped_messages; i++) {
                record_send_drop(worker, send_error);
            }
        }
        const uint64_t batch_duration_ns = block_duration_ns * (uint64_t)batch_count;
        const uint64_t batch_samples = worker->block_samples * (uint64_t)batch_count;
        pace_or_record_late(worker, &next_send_ns, batch_duration_ns, batch_samples);
    }

    free(payloads);
    free(packets);
    udp_output_close(&udp);
    return NULL;
}

static bool stream_worker_start(stream_worker_t *worker)
{
    worker->packet_bytes = worker->block_samples * sizeof(iq_ci16_t);
    if (!ringbuffer_init(&worker->ringbuffer, worker->packet_bytes, RINGBUFFER_PACKET_CAPACITY)) {
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
     * which serialised them. Empty set -> no pinning. Wideband render threads are assigned first
     * per receiver so they tend to land on their own core. */
    size_t next_cpu = 0;

    for (size_t i = 0; i < config->config->receiver_count; i++) {
        receiver_config_t *receiver = &config->config->receivers[i];
        stream_worker_t *worker = &m->workers[m->worker_count++];
        const int wb_render_cpu = next_stream_cpu(config, &next_cpu);
        const int wb_udp_cpu = next_stream_cpu(config, &next_cpu);
        *worker = (stream_worker_t){
            .running = &m->running,
            .scenario = config->scenario,
            .asset_cache = config->asset_cache,
            .timebase = config->timebase,
            .receiver = receiver,
            .metrics = &config->metrics[i],
            .stream_metrics = &config->metrics[i].streams[0],
            .receiver_lock = config->receiver_lock,
            .kind = STREAM_KIND_80MHZ,
            .block_samples = config->block_samples,
            .render_cpu = wb_render_cpu,
            .udp_cpu = wb_udp_cpu,
        };
        if (!stream_worker_start(worker)) {
            streamer_manager_stop(m);
            return false;
        }

        for (size_t d = 0; d < SIM_DDC_COUNT; d++) {
            stream_worker_t *ddc_worker = &m->workers[m->worker_count++];
            const int ddc_render_cpu = next_stream_cpu(config, &next_cpu);
            const int ddc_udp_cpu = next_stream_cpu(config, &next_cpu);
            *ddc_worker = (stream_worker_t){
                .running = &m->running,
                .scenario = config->scenario,
                .asset_cache = config->asset_cache,
                .timebase = config->timebase,
                .receiver = receiver,
                .metrics = &config->metrics[i],
                .stream_metrics = &config->metrics[i].streams[1 + d],
                .ddc_index = d,
                .receiver_lock = config->receiver_lock,
                .kind = STREAM_KIND_DDC,
                .block_samples = config->block_samples,
                .render_cpu = ddc_render_cpu,
                .udp_cpu = ddc_udp_cpu,
            };
            if (!stream_worker_start(ddc_worker)) {
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
