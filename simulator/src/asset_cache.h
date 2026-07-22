#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include "sim_types.h"

#include <stdbool.h>

/* A loaded IQ buffer: either heap-allocated (mmapped=false) or a read-only file mapping.
 * `samples` holds `sample_count` complex samples in `format`'s native type (iq_ci16_t for the
 * general mixer source; iq_ci16_t/iq_ci24_t/iq_cf32_t for a passthrough variant). */
typedef struct {
    void *samples;
    sim_output_format_t format;
    uint64_t sample_count;
    uint32_t sample_rate_hz;
    bool mmapped;
    size_t map_bytes; /* mapping length for munmap when mmapped */
} cached_iq_buffer_t;

typedef struct {
    char source_id[SIM_MAX_ID];
    scenario_source_kind_t source_kind;
    uint64_t sample_count;
    iq_ci16_t *samples;
    bool mmapped;     /* samples points into a read-only file mapping, not the heap */
    size_t map_bytes; /* mapping length for munmap when mmapped */
    float *audio_samples;
    float *audio_hilbert;
    double *audio_integral;
    double normalization_gain; /* factor applied to audio_samples at load (1.0 for IQ) */
    /* Passthrough sources load one buffer per rate variant here (parallel to
     * source->passthrough_variants); the renderer picks the one matching the channel rate. */
    size_t variant_count;
    cached_iq_buffer_t variants[SIM_MAX_PASSTHROUGH_VARIANTS];
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

/* Seam crossfade applied when loop-conditioning the audio of a continuous signal: the clip's
 * tail is overlap-added onto its head, shortening the loop by this many seconds (clamped to a
 * quarter of the clip), so the pre-rendered buffer wraps without a phase/envelope discontinuity. */
#define AUDIO_LOOP_CROSSFADE_S 0.02

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size);
bool asset_cache_load_limited(asset_cache_t *cache, const scenario_t *scenario, size_t max_bytes, size_t batch_samples, const prerender_params_t *prerender, char *error, size_t error_size);
void asset_cache_free(asset_cache_t *cache);
const cached_asset_t *asset_cache_find(const asset_cache_t *cache, const char *source_id);
const cached_prerender_t *asset_cache_prerender(const asset_cache_t *cache, size_t signal_index);

/* Pre-render one signal from an in-memory audio buffer (no RMS normalisation applied), using the
 * exact load-time synthesis. Test/tool entry point; the caller owns and must free out->samples. */
bool asset_cache_prerender_from_audio(cached_prerender_t *out, const float *audio, uint64_t audio_count, uint32_t audio_rate_hz, const scenario_signal_t *signal, const prerender_params_t *params, char *error, size_t error_size);

#endif
