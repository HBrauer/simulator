#ifndef STREAMER_H
#define STREAMER_H

#include "sim_types.h"
#include "metrics.h"
#include "asset_cache.h"
#include "timebase.h"

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

typedef struct streamer_manager streamer_manager_t;

typedef struct {
    simulator_config_t *config;
    const scenario_t *scenario;
    const asset_cache_t *asset_cache;
    const timebase_t *timebase;
    pthread_mutex_t *receiver_lock;
    receiver_metrics_t *metrics;
    size_t block_samples;
    int stream_cpu;
} streamer_config_t;

bool streamer_manager_start(streamer_manager_t **manager, const streamer_config_t *config);
void streamer_manager_stop(streamer_manager_t *manager);
uint64_t streamer_block_duration_ns(size_t block_samples, uint32_t sample_rate_hz);

/* Deterministic block grid. Block boundaries are anchored to the start of the current
 * UTC day so any two instances with the same sample rate agree on which samples belong
 * to which block, independent of when their threads happen to run. All arithmetic is
 * exact 128-bit integer math on absolute sample indices (never accumulated in ns), so it
 * does not drift even when 1e9 is not divisible by the sample rate (e.g. 96 MS/s). */
uint64_t streamer_block_index_from_time_ns(uint64_t scenario_time_ns, size_t block_samples, uint32_t sample_rate_hz);
uint64_t streamer_block_start_ns(uint64_t block_index, size_t block_samples, uint32_t sample_rate_hz);

#endif
