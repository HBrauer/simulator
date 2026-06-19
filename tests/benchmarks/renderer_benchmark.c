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
    const char *json_path = NULL;
    if (argc > 1) {
        blocks = (size_t)strtoull(argv[1], NULL, 10);
    }
    if (argc > 2) {
        samples_per_block = (size_t)strtoull(argv[2], NULL, 10);
    }
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json_path = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--receivers") == 0) {
            receivers = (size_t)strtoull(argv[i + 1], NULL, 10);
            i++;
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
    if (!config_load_yaml("configs/instance_001.yaml", &config, error, sizeof(error)) ||
        !scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)) ||
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
        for (size_t receiver_index = 0; receiver_index < receivers; receiver_index++) {
            renderer_render_80mhz_block(&scenario, &cache, &benchmark_receivers[receiver_index], 450000ULL, buffer, samples_per_block, &stats);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);

    const double seconds = elapsed_seconds(start, stop);
    const double samples = (double)blocks * (double)samples_per_block * (double)receivers;
    const double samples_per_second = seconds > 0.0 ? samples / seconds : 0.0;
    printf("blocks=%zu samples_per_block=%zu receivers=%zu seconds=%.6f samples_per_second=%.3f\n", blocks, samples_per_block, receivers, seconds, samples_per_second);
    if (json_path != NULL && !write_json_report(json_path, blocks, samples_per_block, receivers, seconds, samples_per_second)) {
        fprintf(stderr, "failed to write benchmark report: %s\n", json_path);
        free(buffer);
        asset_cache_free(&cache);
        return 4;
    }

    free(buffer);
    asset_cache_free(&cache);
    return 0;
}
