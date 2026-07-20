#include "asset_cache.h"
#include "config.h"
#include "renderer.h"
#include "scenario.h"
#include "timebase.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double elapsed_seconds(struct timespec start, struct timespec stop)
{
    return (double)(stop.tv_sec - start.tv_sec) + (double)(stop.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static bool write_json_report(const char *path, size_t blocks, size_t samples_per_block, size_t receivers, double seconds, double samples_per_second)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }

    const size_t total_samples = blocks * samples_per_block * receivers;
    const int written = fprintf(file,
                                "{\n"
                                "  \"benchmark\": \"renderer_80mhz_scalar\",\n"
                                "  \"blocks\": %zu,\n"
                                "  \"samples_per_block\": %zu,\n"
                                "  \"receivers\": %zu,\n"
                                "  \"total_samples\": %zu,\n"
                                "  \"seconds\": %.9f,\n"
                                "  \"samples_per_second\": %.3f\n"
                                "}\n",
                                blocks,
                                samples_per_block,
                                receivers,
                                total_samples,
                                seconds,
                                samples_per_second);
    const int closed = fclose(file);
    return written > 0 && closed == 0;
}

int main(int argc, char **argv)
{
    size_t blocks = 1000;
    size_t samples_per_block = 4096;
    size_t receivers = 1;
    bool all_channels = false;
    bool sum_rates = false;
    double assert_realtime = 0.0;
    const char *config_path = "simulator/configs/receiver_scanner.yaml";
    const char *scenario_path = "simulator/scenarios/scanner_fsk.json";
    const char *json_path = NULL;
    if (argc > 1) {
        blocks = (size_t)strtoull(argv[1], NULL, 10);
    }
    if (argc > 2) {
        samples_per_block = (size_t)strtoull(argv[2], NULL, 10);
    }
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--all-channels") == 0 || strcmp(argv[i], "--with-ddc") == 0) {
            all_channels = true;
        } else if (strcmp(argv[i], "--sum-rates") == 0) {
            sum_rates = true;
        } else if (i + 1 < argc && strcmp(argv[i], "--json") == 0) {
            json_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--receivers") == 0) {
            receivers = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (i + 1 < argc && strcmp(argv[i], "--config") == 0) {
            config_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--scenario") == 0) {
            scenario_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--assert-realtime") == 0) {
            assert_realtime = strtod(argv[++i], NULL);
        }
    }
    if (receivers == 0 || receivers > SIM_MAX_RECEIVERS) {
        fprintf(stderr, "receivers must be 1..%d\n", SIM_MAX_RECEIVERS);
        return 2;
    }

    char error[256];
    simulator_config_t config;
    scenario_t scenario;
    asset_cache_t cache;
    if (!config_load_yaml(config_path, &config, error, sizeof(error)) ||
        !scenario_load_json(scenario_path, &scenario, error, sizeof(error)) ||
        !scenario_validate(&scenario, ".", error, sizeof(error)) ||
        !asset_cache_load(&cache, &scenario, error, sizeof(error))) {
        fprintf(stderr, "setup failed: %s\n", error);
        return 2;
    }

    iq_ci16_t *buffer = calloc(samples_per_block, sizeof(*buffer));
    if (buffer == NULL) {
        asset_cache_free(&cache);
        return 3;
    }

    struct timespec start;
    struct timespec stop;
    clock_gettime(CLOCK_MONOTONIC, &start);
    render_stats_t stats;
    receiver_config_t benchmark_receivers[SIM_MAX_RECEIVERS];
    for (size_t receiver_index = 0; receiver_index < receivers; receiver_index++) {
        benchmark_receivers[receiver_index] = config.receivers[0];
        benchmark_receivers[receiver_index].id = (uint32_t)receiver_index;
    }

    for (size_t i = 0; i < blocks; i++) {
        const uint64_t t = 450000ULL + (uint64_t)i * 1000000ULL;
        for (size_t receiver_index = 0; receiver_index < receivers; receiver_index++) {
            const receiver_config_t *rx = &benchmark_receivers[receiver_index];
            renderer_render_channel_block(&scenario, &cache, rx, &rx->channels[0], t, buffer, samples_per_block, &stats);
            if (all_channels) {
                for (size_t c = 1; c < rx->channel_count; c++) {
                    renderer_render_channel_block(&scenario, &cache, rx, &rx->channels[c], t, buffer, samples_per_block, &stats);
                }
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);

    const double seconds = elapsed_seconds(start, stop);
    const size_t streams_per_receiver = all_channels ? config.receivers[0].channel_count : 1;
    const double samples = (double)blocks * (double)samples_per_block * (double)receivers * (double)streams_per_receiver;
    const double samples_per_second = seconds > 0.0 ? samples / seconds : 0.0;
    /* Wall-clock time these blocks represent for the wideband stream. If the machine renders the
     * whole configured load (all receivers, wideband + DDCs) in less than this, one core sustains
     * it in real time; the ratio is the headroom (>=1.3 == the P9 target of 30% headroom).
     * With --sum-rates the represented time sums every rendered channel's own rate instead --
     * the right measure for mixed-rate loads (e.g. one wideband channel plus many narrow DDC
     * channels, where a low-rate block represents far more stream time than a wideband one). */
    double represented_seconds = (double)blocks * (double)samples_per_block / (double)config.receivers[0].channels[0].sample_rate_hz;
    if (sum_rates && all_channels) {
        for (size_t c = 1; c < config.receivers[0].channel_count; c++) {
            represented_seconds += (double)blocks * (double)samples_per_block / (double)config.receivers[0].channels[c].sample_rate_hz;
        }
        represented_seconds *= (double)receivers;
    }
    const double realtime_ratio = seconds > 0.0 ? represented_seconds / seconds : 0.0;
    printf("blocks=%zu samples_per_block=%zu receivers=%zu all_channels=%s seconds=%.6f samples_per_second=%.3f realtime_ratio=%.2f\n",
           blocks, samples_per_block, receivers, all_channels ? "yes" : "no", seconds, samples_per_second, realtime_ratio);
    if (json_path != NULL && !write_json_report(json_path, blocks, samples_per_block, receivers, seconds, samples_per_second)) {
        fprintf(stderr, "failed to write benchmark report: %s\n", json_path);
        free(buffer);
        asset_cache_free(&cache);
        return 4;
    }

    free(buffer);
    asset_cache_free(&cache);
    if (assert_realtime > 0.0 && realtime_ratio < assert_realtime) {
        fprintf(stderr, "realtime_ratio %.2f below required %.2f\n", realtime_ratio, assert_realtime);
        return 5;
    }
    return 0;
}
