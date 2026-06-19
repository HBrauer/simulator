#include "asset_cache.h"
#include "config.h"
#include "renderer.h"
#include "scenario.h"
#include "timebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double elapsed_seconds(struct timespec start, struct timespec stop)
{
    return (double)(stop.tv_sec - start.tv_sec) + (double)(stop.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv)
{
    size_t blocks = 1000;
    size_t samples_per_block = 4096;
    if (argc > 1) {
        blocks = (size_t)strtoull(argv[1], NULL, 10);
    }
    if (argc > 2) {
        samples_per_block = (size_t)strtoull(argv[2], NULL, 10);
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
    for (size_t i = 0; i < blocks; i++) {
        renderer_render_80mhz_block(&scenario, &cache, &config.receivers[0], 450000ULL, buffer, samples_per_block, &stats);
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);

    const double seconds = elapsed_seconds(start, stop);
    const double samples = (double)blocks * (double)samples_per_block;
    printf("blocks=%zu samples_per_block=%zu seconds=%.6f samples_per_second=%.3f\n", blocks, samples_per_block, seconds, samples / seconds);

    free(buffer);
    asset_cache_free(&cache);
    return 0;
}
