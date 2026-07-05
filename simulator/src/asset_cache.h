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

/* Complex-baseband IQ pre-rendered once at load for a signal that references an audio source.
 * Keyed per signal (modulation, deviation, depth are per-signal). Stored as ci16 with peak
 * normalisation; `gain` folds the recorded peak back into the renderer's source_gain so the
 * output amplitude is calibrated exactly as the direct full-rate synthesis would produce. */
typedef struct {
    bool valid;
    uint32_t sample_rate_hz;
    uint64_t sample_count;
    iq_ci16_t *samples;
    double gain;
} cached_prerender_t;

typedef struct {
    size_t asset_count;
    cached_asset_t assets[SIM_MAX_SOURCES];
    cached_prerender_t prerenders[SIM_MAX_SIGNALS]; /* indexed parallel to scenario->signals */
} asset_cache_t;

/* Pre-render tuning (see simulator_config_t / §5). Passed explicitly so asset_cache stays
 * independent of the config module. */
typedef struct {
    double oversample;
    uint32_t max_rate_hz;
} prerender_params_t;

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size);
bool asset_cache_load_limited(asset_cache_t *cache, const scenario_t *scenario, size_t max_bytes, size_t batch_samples, const prerender_params_t *prerender, char *error, size_t error_size);
void asset_cache_free(asset_cache_t *cache);
const cached_asset_t *asset_cache_find(const asset_cache_t *cache, const char *source_id);
const cached_prerender_t *asset_cache_prerender(const asset_cache_t *cache, size_t signal_index);

#endif
