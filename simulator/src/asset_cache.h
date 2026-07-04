#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include "sim_types.h"

#include <stdbool.h>

typedef struct {
    char source_id[SIM_MAX_ID];
    scenario_source_kind_t source_kind;
    uint64_t sample_count;
    iq_ci16_t *samples;
    float *audio_samples;
    float *audio_hilbert;
    double *audio_integral;
    double normalization_gain; /* factor applied to audio_samples at load (1.0 for IQ) */
} cached_asset_t;

typedef struct {
    size_t asset_count;
    cached_asset_t assets[SIM_MAX_SOURCES];
} asset_cache_t;

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size);
bool asset_cache_load_limited(asset_cache_t *cache, const scenario_t *scenario, size_t max_bytes, size_t batch_samples, char *error, size_t error_size);
void asset_cache_free(asset_cache_t *cache);
const cached_asset_t *asset_cache_find(const asset_cache_t *cache, const char *source_id);

#endif
