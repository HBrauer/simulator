#include "streamer.h"
#include "receiver.h"
#include "renderer.h"
#include "ringbuffer.h"
#include "udp_output.h"

#ifdef __linux__
#include <sched.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    pthread_mutex_t ringbuffer_lock;
    pthread_t render_thread;
    pthread_t udp_thread;
    bool render_started;
    bool udp_started;
    bool ringbuffer_initialized;
    bool ringbuffer_lock_initialized;
    int stream_cpu;
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
    atomic_fetch_add(&worker->stream_metrics->ringbuffer_underruns, 1);
}

static void *stream_render_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->stream_cpu);
    iq_ci16_t *buffer = calloc(worker->block_samples, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    while (atomic_load(worker->running)) {
        const uint64_t scenario_time_ns = timebase_now_ns(worker->timebase);
        pthread_mutex_lock(worker->receiver_lock);
        receiver_config_t receiver_snapshot = *worker->receiver;
        ddc_config_t ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
        pthread_mutex_unlock(worker->receiver_lock);

        if (!stream_enabled(worker, &receiver_snapshot, &ddc_snapshot)) {
            stream_worker_set_active(worker, false);
            pthread_mutex_lock(&worker->ringbuffer_lock);
            ringbuffer_clear(&worker->ringbuffer);
            pthread_mutex_unlock(&worker->ringbuffer_lock);
            sleep_for_block(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));
            continue;
        }
        stream_worker_set_active(worker, true);

        render_one_block(worker, buffer, &receiver_snapshot, &ddc_snapshot, scenario_time_ns);

        pthread_mutex_lock(&worker->ringbuffer_lock);
        if (ringbuffer_available(&worker->ringbuffer) >= worker->packet_bytes) {
            (void)ringbuffer_write(&worker->ringbuffer, (const uint8_t *)buffer, worker->packet_bytes);
        } else {
            record_overrun(worker);
        }
        pthread_mutex_unlock(&worker->ringbuffer_lock);

        sleep_for_block(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));
    }

    free(buffer);
    return NULL;
}

static void *stream_udp_thread_main(void *arg)
{
    stream_worker_t *worker = arg;
    apply_stream_affinity(worker->stream_cpu);
    pthread_mutex_lock(worker->receiver_lock);
    receiver_config_t receiver_snapshot = *worker->receiver;
    ddc_config_t ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
    pthread_mutex_unlock(worker->receiver_lock);

    const char *host = receiver_snapshot.udp_output_host;
    const uint16_t port = worker->kind == STREAM_KIND_80MHZ
        ? receiver_snapshot.udp_80mhz_output.port
        : ddc_snapshot.udp_output.port;

    udp_output_t udp = {.fd = -1};
    if (!udp_output_open(&udp, host, port)) {
        return NULL;
    }

    uint8_t *packet = calloc(1, worker->packet_bytes);
    if (packet == NULL) {
        udp_output_close(&udp);
        return NULL;
    }

    while (atomic_load(worker->running)) {
        pthread_mutex_lock(worker->receiver_lock);
        receiver_snapshot = *worker->receiver;
        ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
        pthread_mutex_unlock(worker->receiver_lock);

        if (!stream_enabled(worker, &receiver_snapshot, &ddc_snapshot)) {
            sleep_for_block(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));
            continue;
        }

        bool got_packet = false;
        pthread_mutex_lock(&worker->ringbuffer_lock);
        if (ringbuffer_fill(&worker->ringbuffer) >= worker->packet_bytes) {
            (void)ringbuffer_read(&worker->ringbuffer, packet, worker->packet_bytes);
            got_packet = true;
        }
        pthread_mutex_unlock(&worker->ringbuffer_lock);

        if (!got_packet) {
            record_underrun(worker);
            sleep_for_block(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));
            continue;
        }

        size_t sent = 0;
        if (udp_output_send(&udp, packet, worker->packet_bytes, &sent)) {
            atomic_fetch_add(&worker->metrics->udp_packets_sent, 1);
            atomic_fetch_add(&worker->metrics->udp_bytes_sent, sent);
            atomic_fetch_add(&worker->metrics->samples_rendered, worker->block_samples);
            atomic_fetch_add(&worker->stream_metrics->udp_packets_sent, 1);
            atomic_fetch_add(&worker->stream_metrics->udp_bytes_sent, sent);
            atomic_fetch_add(&worker->stream_metrics->samples_rendered, worker->block_samples);
        } else {
            atomic_fetch_add(&worker->metrics->udp_send_errors, 1);
            atomic_fetch_add(&worker->stream_metrics->udp_send_errors, 1);
        }
        sleep_for_block(worker->block_samples, stream_sample_rate(worker, &receiver_snapshot, &ddc_snapshot));
    }

    free(packet);
    udp_output_close(&udp);
    return NULL;
}

static bool stream_worker_start(stream_worker_t *worker)
{
    worker->packet_bytes = worker->block_samples * sizeof(iq_ci16_t);
    if (!ringbuffer_init(&worker->ringbuffer, worker->packet_bytes * 8U)) {
        return false;
    }
    worker->ringbuffer_initialized = true;
    if (pthread_mutex_init(&worker->ringbuffer_lock, NULL) != 0) {
        return false;
    }
    worker->ringbuffer_lock_initialized = true;
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
    if (worker->ringbuffer_lock_initialized) {
        pthread_mutex_destroy(&worker->ringbuffer_lock);
        worker->ringbuffer_lock_initialized = false;
    }
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

    for (size_t i = 0; i < config->config->receiver_count; i++) {
        receiver_config_t *receiver = &config->config->receivers[i];
        stream_worker_t *worker = &m->workers[m->worker_count++];
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
            .stream_cpu = config->stream_cpu,
        };
        if (!stream_worker_start(worker)) {
            streamer_manager_stop(m);
            return false;
        }

        for (size_t d = 0; d < SIM_DDC_COUNT; d++) {
            stream_worker_t *ddc_worker = &m->workers[m->worker_count++];
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
                .stream_cpu = config->stream_cpu,
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
