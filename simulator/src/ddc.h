#ifndef DDC_H
#define DDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Multi-stage digital down-converter filter design.
 *
 * A plan factors an integer decimation ratio into a cascade of FIR decimation stages and
 * designs a Kaiser windowed-sinc lowpass for each. Every stage protects only the *final*
 * channel passband: its stopband starts where content would fold onto that passband after
 * the stage's decimation (stage_output_rate - passband_edge); everything between the
 * passband and that edge aliases into frequencies a later stage removes anyway. That is
 * what keeps the full-rate early stages short (tens of taps) even for six-figure ratios,
 * where a single-stage filter would need hundreds of thousands of taps. */

#define DDC_MAX_STAGES 8
#define DDC_MAX_STAGE_TAPS 512U
/* Non-final stages run at high input rates, so their per-input cost is capped harder. */
#define DDC_NONFINAL_STAGE_TAPS 96U
#define DDC_MAX_STAGE_DECIM 32U
#define DDC_STOPBAND_DB 80.0
/* Integer decimation ratios above this go through the cascade; at or below it the renderer's
 * single-stage polyphase resampler still has enough taps for full alias rejection. */
#define DDC_CASCADE_RATIO_THRESHOLD 16U

typedef struct {
    uint32_t decim;
    uint32_t tap_count; /* odd; symmetric taps with unity DC gain */
    float taps[DDC_MAX_STAGE_TAPS];
} ddc_stage_t;

typedef struct {
    uint32_t source_rate_hz;
    uint32_t output_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t ratio; /* == product of stage decimations */
    size_t stage_count;
    ddc_stage_t stages[DDC_MAX_STAGES];
    /* Group-delay reach of the whole centered cascade, in source samples: output sample m is
     * computed from source samples [m*ratio - reach, m*ratio + reach]. A block render must
     * feed the cascade this many samples of history before (and lookahead after) the block's
     * own source span so block boundaries carry no filter transient. */
    uint64_t history_source_samples;
} ddc_plan_t;

/* Design a cascade for source_rate -> output_rate (source must be an integer multiple of
 * output) protecting the +/- bandwidth/2 channel passband with ~stopband_db alias rejection.
 * Deterministic: the same inputs always produce the same plan. On failure writes a short
 * error code (receiver_validate style) and returns false. */
bool ddc_plan_design(ddc_plan_t *plan,
                     uint32_t source_rate_hz,
                     uint32_t output_rate_hz,
                     uint32_t bandwidth_hz,
                     double stopband_db,
                     char *error,
                     size_t error_size);

/* Cached plans (stopband fixed at DDC_STOPBAND_DB), built once per distinct rate/bandwidth
 * triple. Lock-free lookup of already-built plans; mutex only to append. NULL when the
 * design fails or the cache is full. */
const ddc_plan_t *ddc_plan_cache_get(uint32_t source_rate_hz,
                                     uint32_t output_rate_hz,
                                     uint32_t bandwidth_hz);

/* Cached-intermediate parameters. A DDC channel is served in two hops: a cached sub-band of
 * the recording at the intermediate rate (shifted to a grid-quantized center, front cascade),
 * then the per-block tail (residual rotation + tail cascade) at ~1/16th and less of the
 * source-rate cost. Pure functions so cache keys are deterministic. */

/* Smallest rate that divides source_rate, is a multiple of output_rate, and leaves a tail
 * ratio >= 16 with a front decimation >= 4. Returns 0 when no such rate exists (caching not
 * worthwhile; use the direct cascade). */
uint32_t ddc_intermediate_rate_hz(uint32_t source_rate_hz, uint32_t output_rate_hz);

/* Usable (flat) bandwidth of a cached intermediate: 0.8 * intermediate rate. The front plan
 * is designed for this band so every channel/grid-offset combination fits inside it. */
uint32_t ddc_front_bandwidth_hz(uint32_t intermediate_rate_hz);

/* Tune-center quantization grid for cache sharing: intermediate_rate / 5. Any tune within
 * +/- step/2 of a grid point reuses that point's cached intermediate; the residual offset is
 * absorbed by an NCO at the intermediate rate. */
uint32_t ddc_grid_step_hz(uint32_t intermediate_rate_hz);

/* Nearest grid multiple (ties round up). */
int64_t ddc_grid_center_hz(int64_t center_hz, uint32_t grid_step_hz);

#endif
