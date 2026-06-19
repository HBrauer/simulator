#include "streamer.h"
#include "receiver.h"
#include "renderer.h"
#include "udp_output.h"

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
    const timebase_t *timebase;
    receiver_config_t *receiver;
    receiver_metrics_t *metrics;
    size_t ddc_index;
    pthread_mutex_t *receiver_lock;
    stream_kind_t kind;
    size_t block_samples;
    pthread_t thread;
    bool started;
} stream_worker_t;

struct streamer_manager {
    atomic_bool running;
    size_t worker_count;
    stream_worker_t workers[SIM_MAX_RECEIVERS * (1 + SIM_DDC_COUNT)];
};

static void sleep_for_block(size_t block_samples, uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return;
    }
    const double seconds = (double)block_samples / (double)sample_rate_hz;
    struct timespec ts = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1000000000.0),
    };
    nanosleep(&ts, NULL);
}

static void *stream_worker_main(void *arg)
{
    stream_worker_t *worker = arg;
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

    iq_ci16_t *buffer = calloc(worker->block_samples, sizeof(*buffer));
    if (buffer == NULL) {
        udp_output_close(&udp);
        return NULL;
    }

    while (atomic_load(worker->running)) {
        render_stats_t stats;
        const uint64_t scenario_time_ns = timebase_now_ns(worker->timebase);
        pthread_mutex_lock(worker->receiver_lock);
        receiver_snapshot = *worker->receiver;
        ddc_snapshot = receiver_snapshot.ddc[worker->ddc_index];
        pthread_mutex_unlock(worker->receiver_lock);
        if (worker->kind == STREAM_KIND_80MHZ) {
            renderer_render_80mhz_block(worker->scenario, &receiver_snapshot, scenario_time_ns, buffer, worker->block_samples, &stats);
        } else {
            if (receiver_ddc_in_window(&receiver_snapshot, &ddc_snapshot, scenario_time_ns)) {
                renderer_render_ddc_block(worker->scenario, &ddc_snapshot, scenario_time_ns, buffer, worker->block_samples, &stats);
            } else {
                memset(buffer, 0, worker->block_samples * sizeof(*buffer));
            }
        }
        size_t sent = 0;
        if (udp_output_send(&udp, buffer, worker->block_samples * sizeof(*buffer), &sent)) {
            atomic_fetch_add(&worker->metrics->udp_packets_sent, 1);
            atomic_fetch_add(&worker->metrics->udp_bytes_sent, sent);
            atomic_fetch_add(&worker->metrics->samples_rendered, worker->block_samples);
        } else {
            atomic_fetch_add(&worker->metrics->udp_send_errors, 1);
        }
        const uint32_t sample_rate = worker->kind == STREAM_KIND_80MHZ
            ? receiver_snapshot.sample_rate_hz
            : ddc_snapshot.sample_rate_hz;
        sleep_for_block(worker->block_samples, sample_rate);
    }

    free(buffer);
    udp_output_close(&udp);
    return NULL;
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
            .timebase = config->timebase,
            .receiver = receiver,
            .metrics = &config->metrics[i],
            .receiver_lock = config->receiver_lock,
            .kind = STREAM_KIND_80MHZ,
            .block_samples = config->block_samples,
        };
        if (pthread_create(&worker->thread, NULL, stream_worker_main, worker) != 0) {
            streamer_manager_stop(m);
            return false;
        }
        worker->started = true;

        for (size_t d = 0; d < SIM_DDC_COUNT; d++) {
            stream_worker_t *ddc_worker = &m->workers[m->worker_count++];
            *ddc_worker = (stream_worker_t){
                .running = &m->running,
                .scenario = config->scenario,
                .timebase = config->timebase,
                .receiver = receiver,
                .metrics = &config->metrics[i],
                .ddc_index = d,
                .receiver_lock = config->receiver_lock,
                .kind = STREAM_KIND_DDC,
                .block_samples = config->block_samples,
            };
            if (pthread_create(&ddc_worker->thread, NULL, stream_worker_main, ddc_worker) != 0) {
                streamer_manager_stop(m);
                return false;
            }
            ddc_worker->started = true;
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
        if (manager->workers[i].started) {
            pthread_join(manager->workers[i].thread, NULL);
        }
    }
    free(manager);
}
