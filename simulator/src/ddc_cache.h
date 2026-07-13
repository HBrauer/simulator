#ifndef DDC_CACHE_H
#define DDC_CACHE_H

#include "ddc.h"
#include "sim_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cache of precomputed intermediate sub-bands for DDC channels.
 *
 * An entry is one full loop of a recording, frequency-shifted by a fixed offset and run
 * through a front decimation cascade down to an intermediate rate. Narrow channels then
 * render from the entry (residual rotation + short tail cascade) at a small fraction of the
 * source-rate cost, and a detector revisiting the same tune area keeps hitting the same
 * entry. Entries are built synchronously on first request (the caller's render thread stalls
 * for the build -- an accepted, bounded retune cost) and evicted LRU under a byte budget.
 *
 * Exactness at the loop seam: the entry is built circularly (filter history wraps around the
 * loop), which is only consistent if the shift completes a whole number of cycles per loop.
 * The requested shift is therefore snapped to the nearest whole-cycles-per-loop frequency
 * (granularity 1/loop_duration Hz; error <= 0.5 Hz for a 1 s loop) and the applied value is
 * stored in the entry, so the caller can fold the snap error into its exact residual NCO.
 * Entry content is a pure function of (source samples, applied shift, intermediate rate,
 * front plan), so instances stay byte-identical no matter when entries get (re)built. */

/* Entries store samples at half scale (a rotated ci16 component can reach sqrt(2) * 32767,
 * and the front filter adds ~dB of overshoot); consumers double their gain to compensate --
 * the same convention as the renderer's mix-first rotation scratch. */
#define DDC_CACHE_SAMPLE_SCALE 0.5

typedef struct ddc_cache_entry {
    char source_id[SIM_MAX_ID];
    int64_t shift_cycles;          /* applied shift in whole cycles per source loop */
    uint32_t intermediate_rate_hz;
    double applied_shift_hz;       /* == shift_cycles * source_rate / loop_samples */
    uint64_t sample_count;         /* one seamless loop at the intermediate rate */
    iq_ci16_t *samples;
    /* internal (guarded by the cache lock) */
    bool ready;
    uint32_t refcount;
    uint64_t last_use;
    size_t bytes;
    struct ddc_cache_entry *next;
} ddc_cache_entry_t;

typedef struct ddc_cache ddc_cache_t;

ddc_cache_t *ddc_cache_create(size_t max_bytes);
void ddc_cache_destroy(ddc_cache_t *cache);
size_t ddc_cache_used_bytes(ddc_cache_t *cache);

/* Builds are sliced across threads (default: online CPUs, capped at 8); entry content is
 * bit-identical for any thread count, because rotation is anchored to an absolute chunk grid
 * and every output is a single dot product. 0 restores the auto default. */
void ddc_cache_set_build_threads(unsigned threads);

/* Get-or-build the intermediate for `shift_hz` (snapped as described above). Returns a
 * pinned entry -- pair every successful acquire with ddc_cache_release. Returns NULL when
 * the loop length is not divisible by the front decimation (a circular build would smear
 * the seam), when the entry cannot fit the budget, or on allocation failure. Concurrent
 * acquires of the same key wait for the first builder instead of building twice. */
const ddc_cache_entry_t *ddc_cache_acquire(ddc_cache_t *cache,
                                           const char *source_id,
                                           const iq_ci16_t *source_samples,
                                           uint64_t source_sample_count,
                                           uint32_t source_rate_hz,
                                           double shift_hz,
                                           uint32_t intermediate_rate_hz,
                                           const ddc_plan_t *front_plan);

void ddc_cache_release(ddc_cache_t *cache, const ddc_cache_entry_t *entry);

#endif
