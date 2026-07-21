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
    uint64_t max_batch_latency_ns; /* time cap on a paced UDP send batch; 0 = count cap only */
    const int *stream_cpus;   /* CPUs to spread stream threads across; NULL/empty = no pinning */
    size_t stream_cpu_count;
} streamer_config_t;

bool streamer_manager_start(streamer_manager_t **manager, const streamer_config_t *config);
void streamer_manager_stop(streamer_manager_t *manager);
uint64_t streamer_block_duration_ns(size_t block_samples, uint32_t sample_rate_hz);
/* Effective per-block sample count for a channel rate: low rates shrink below the configured
 * block size (largest power of two keeping >= ~4 blocks/s, floor 64) so a 2 kS/s DDC channel
 * doesn't emit multi-second packets or starve the ~1 s context heartbeat. */
size_t streamer_block_samples_for_rate(size_t configured_block_samples, uint32_t sample_rate_hz);

/* How many blocks the UDP thread may lump into one paced sendmmsg burst before the batch spans
 * more than max_batch_latency_ns of wall-clock time. Always >= 1. A budget of 0 (or a degenerate
 * block duration) means "no time limit" and returns SIZE_MAX, leaving the fixed count cap as the
 * only bound. Low rates return a small count (fine, evenly paced updates); high rates return far
 * more than the count cap, so their batching -- essential for 80 MHz throughput -- is untouched. */
size_t streamer_batch_blocks_for_latency(size_t block_samples, uint32_t sample_rate_hz, uint64_t max_batch_latency_ns);

/* Deterministic block grid. Block boundaries are anchored to the start of the current
 * UTC day so any two instances with the same sample rate agree on which samples belong
 * to which block, independent of when their threads happen to run. All arithmetic is
 * exact 128-bit integer math on absolute sample indices (never accumulated in ns), so it
 * does not drift even when 1e9 is not divisible by the sample rate (e.g. 96 MS/s). */
uint64_t streamer_block_index_from_time_ns(uint64_t scenario_time_ns, size_t block_samples, uint32_t sample_rate_hz);
uint64_t streamer_block_start_ns(uint64_t block_index, size_t block_samples, uint32_t sample_rate_hz);

#endif
