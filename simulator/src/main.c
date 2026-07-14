#include "asset_cache.h"
#include "config.h"
#include "ddc.h"
#include "metrics.h"
#include "renderer.h"
#include "rest_server.h"
#include "scenario.h"
#include "streamer.h"
#include "timebase.h"
#include "udp_output.h"

#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void on_signal(int signum)
{
    (void)signum;
    keep_running = 0;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

/* Warn once per (channel, looping IQ replay source) pair whose extraction quality or DDC
 * cache eligibility is degraded, so misconfigured rate combinations surface at startup
 * instead of as silent quality loss. */
static void warn_ddc_rate_combinations(const simulator_config_t *config, const scenario_t *scenario)
{
    for (size_t r = 0; r < config->receiver_count; r++) {
        const receiver_config_t *receiver = &config->receivers[r];
        for (size_t c = 0; c < receiver->channel_count; c++) {
            const uint32_t rate = receiver->channels[c].sample_rate_hz;
            for (size_t s = 0; s < scenario->signal_count; s++) {
                const scenario_signal_t *sig = &scenario->signals[s];
                if (!sig->loop || sig->passthrough) {
                    continue;
                }
                const scenario_source_t *src = scenario_find_source(scenario, sig->source_reference);
                if (src == NULL || src->source_kind != SCENARIO_SOURCE_IQ_FILE ||
                    src->sample_rate_hz == 0 || rate == 0 || src->sample_rate_hz <= rate) {
                    continue;
                }
                if (src->sample_rate_hz % rate != 0) {
                    if ((double)src->sample_rate_hz / (double)rate > (double)DDC_CASCADE_RATIO_THRESHOLD) {
                        fprintf(stderr,
                                "warning: receiver %u channel %u rate %u Hz does not divide source '%s' rate %u Hz; "
                                "using the legacy resampler with reduced alias rejection\n",
                                receiver->id, receiver->channels[c].id, rate, src->id, src->sample_rate_hz);
                    }
                    continue;
                }
                const uint32_t ratio = src->sample_rate_hz / rate;
                if (ratio <= DDC_CASCADE_RATIO_THRESHOLD) {
                    continue;
                }
                const uint32_t intermediate_hz = ddc_intermediate_rate_hz(src->sample_rate_hz, rate);
                if (intermediate_hz == 0) {
                    fprintf(stderr,
                            "warning: receiver %u channel %u rate %u Hz from source '%s': no cached DDC "
                            "intermediate for ratio %u; running the full-rate cascade every block\n",
                            receiver->id, receiver->channels[c].id, rate, src->id, ratio);
                } else if (src->sample_count % (src->sample_rate_hz / intermediate_hz) != 0) {
                    fprintf(stderr,
                            "warning: source '%s' length %llu is not divisible by the DDC front decimation %u; "
                            "intermediate cache disabled for channel rate %u Hz (full-rate cascade every block)\n",
                            src->id, (unsigned long long)src->sample_count,
                            src->sample_rate_hz / intermediate_hz, rate);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *config_path = arg_value(argc, argv, "--config");
    const char *scenario_path = arg_value(argc, argv, "--scenario");
    const char *scenario_time_arg = arg_value(argc, argv, "--scenario-time-ns");
    const char *once_arg = arg_value(argc, argv, "--render-once-samples");
    const char *block_samples_arg = arg_value(argc, argv, "--stream-block-samples");
    if (config_path == NULL) {
        config_path = "simulator/configs/instance_001.yaml";
    }

    char error[256];
    simulator_config_t config;
    if (!config_load_yaml(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 2;
    }
    if (scenario_path == NULL) {
        scenario_path = config.scenario_file;
    }
    scenario_t scenario;
    if (!scenario_load_json(scenario_path, &scenario, error, sizeof(error)) ||
        !scenario_validate(&scenario, ".", error, sizeof(error))) {
        fprintf(stderr, "scenario error: %s\n", error);
        return 3;
    }
    asset_cache_t asset_cache;
    const prerender_params_t prerender_params = {
        .oversample = config.audio_prerender_oversample,
        .max_rate_hz = config.audio_prerender_max_rate_hz,
    };
    if (!asset_cache_load_limited(&asset_cache, &scenario, config.asset_cache_max_bytes, 4096, &prerender_params, error, sizeof(error))) {
        fprintf(stderr, "asset cache error: %s\n", error);
        return 4;
    }
    renderer_ddc_cache_configure(config.ddc_cache_max_bytes);
    warn_ddc_rate_combinations(&config, &scenario);

    timebase_t timebase;
    timebase_init(&timebase);
    if (scenario_time_arg != NULL) {
        timebase_set_override(&timebase, strtoull(scenario_time_arg, NULL, 10));
    }

    if (once_arg != NULL) {
        const size_t samples = (size_t)strtoull(once_arg, NULL, 10);
        const char *channel_arg = arg_value(argc, argv, "--render-channel");
        const size_t channel_index = channel_arg != NULL ? (size_t)strtoull(channel_arg, NULL, 10) : 0;
        if (channel_index >= config.receivers[0].channel_count) {
            fprintf(stderr, "invalid --render-channel\n");
            asset_cache_free(&asset_cache);
            return 5;
        }
        iq_ci16_t *buffer = calloc(samples, sizeof(*buffer));
        if (buffer == NULL) {
            asset_cache_free(&asset_cache);
            return 4;
        }
        render_stats_t stats;
        renderer_render_channel_block(&scenario, &asset_cache, &config.receivers[0], &config.receivers[0].channels[channel_index], timebase_now_ns(&timebase), buffer, samples, &stats);
        fwrite(buffer, sizeof(*buffer), samples, stdout);
        free(buffer);
        asset_cache_free(&asset_cache);
        return 0;
    }

    /* Prewarm the DDC intermediates for fixed-center channels so their streams start hot.
     * Track-tuner channels stay lazy (their center moves with the scan). Builds block here
     * (background mode is enabled only afterwards), so startup takes roughly one front-
     * cascade pass per unique tune area -- proportional to the recording length. */
    for (size_t i = 0; i < config.receiver_count; i++) {
        for (size_t c = 0; c < config.receivers[i].channel_count; c++) {
            const channel_config_t *channel = &config.receivers[i].channels[c];
            if (channel->track_tuner) {
                continue;
            }
            iq_ci16_t warm[64];
            render_stats_t warm_stats;
            renderer_render_channel_block(&scenario, &asset_cache, &config.receivers[i], channel,
                                          timebase_now_ns(&timebase), warm, 64, &warm_stats);
        }
    }
    /* From here on, a retune to a cold tune area keeps streaming through the direct
     * full-rate cascade while its intermediate builds in the background. */
    renderer_ddc_background_builds(true);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pthread_mutex_t receiver_lock;
    if (pthread_mutex_init(&receiver_lock, NULL) != 0) {
        fprintf(stderr, "failed to initialize receiver lock\n");
        return 6;
    }
    receiver_metrics_t metrics[SIM_MAX_RECEIVERS];
    for (size_t i = 0; i < SIM_MAX_RECEIVERS; i++) {
        receiver_metrics_init(&metrics[i]);
    }

    size_t block_samples = config.stream_block_samples;
    if (block_samples_arg != NULL) {
        block_samples = (size_t)strtoull(block_samples_arg, NULL, 10);
        if (block_samples == 0 || block_samples > SIM_MAX_STREAM_BLOCK_SAMPLES) {
            fprintf(stderr, "invalid --stream-block-samples\n");
            return 6;
        }
    }

    rest_server_t *servers[SIM_MAX_RECEIVERS] = {0};
    for (size_t i = 0; i < config.receiver_count; i++) {
        rest_context_t context = {
            .receiver = &config.receivers[i],
            .scenario = &scenario,
            .timebase = &timebase,
            .receiver_lock = &receiver_lock,
            .metrics = &metrics[i],
            .version = "0.1.0",
        };
        if (!rest_server_start(&servers[i], &context)) {
            fprintf(stderr, "failed to start REST server for receiver %u\n", config.receivers[i].id);
            pthread_mutex_destroy(&receiver_lock);
            asset_cache_free(&asset_cache);
            return 5;
        }
        printf("receiver %u REST http://%s:%u/api/v1\n", config.receivers[i].id, config.receivers[i].rest_bind_host, config.receivers[i].rest_port);
    }

    streamer_manager_t *streamer = NULL;
    streamer_config_t streamer_config = {
        .config = &config,
        .scenario = &scenario,
        .asset_cache = &asset_cache,
        .timebase = &timebase,
        .receiver_lock = &receiver_lock,
        .metrics = metrics,
        .block_samples = block_samples,
        .stream_cpus = config.stream_cpus,
        .stream_cpu_count = config.stream_cpu_count,
    };
    if (!streamer_manager_start(&streamer, &streamer_config)) {
        fprintf(stderr, "failed to start UDP streamers\n");
        for (size_t i = 0; i < config.receiver_count; i++) {
            rest_server_stop(servers[i]);
        }
        pthread_mutex_destroy(&receiver_lock);
        asset_cache_free(&asset_cache);
        return 7;
    }

    while (keep_running) {
        sleep(1);
    }

    streamer_manager_stop(streamer);
    for (size_t i = 0; i < config.receiver_count; i++) {
        rest_server_stop(servers[i]);
    }
    pthread_mutex_destroy(&receiver_lock);
    /* Drain in-flight background DDC builds (they read asset memory) before freeing assets. */
    renderer_ddc_cache_configure(0);
    asset_cache_free(&asset_cache);
    return 0;
}
