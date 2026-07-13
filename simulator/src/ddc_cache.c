/* Intermediate sub-band cache (see ddc_cache.h for the model). The build walks the loop once
 * through a streaming executor, gathering source samples with modulo addressing so the filter
 * history wraps the seam, and rotating with the same drift-free anchored-recurrence oscillator
 * pattern the renderer's mix-first path uses. */
#include "ddc_cache.h"
#include "nco.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Source samples gathered/rotated per executor push during a build. */
#define DDC_CACHE_BUILD_CHUNK 65536U

struct ddc_cache {
    pthread_mutex_t lock;
    pthread_cond_t built;
    size_t max_bytes;
    size_t used_bytes;
    uint64_t use_counter;
    ddc_cache_entry_t *entries;
};

ddc_cache_t *ddc_cache_create(size_t max_bytes)
{
    ddc_cache_t *cache = calloc(1, sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    pthread_mutex_init(&cache->lock, NULL);
    pthread_cond_init(&cache->built, NULL);
    cache->max_bytes = max_bytes;
    return cache;
}

static void ddc_cache_entry_free(ddc_cache_entry_t *entry)
{
    free(entry->samples);
    free(entry);
}

void ddc_cache_destroy(ddc_cache_t *cache)
{
    if (cache == NULL) {
        return;
    }
    ddc_cache_entry_t *entry = cache->entries;
    while (entry != NULL) {
        ddc_cache_entry_t *next = entry->next;
        ddc_cache_entry_free(entry);
        entry = next;
    }
    pthread_cond_destroy(&cache->built);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

size_t ddc_cache_used_bytes(ddc_cache_t *cache)
{
    pthread_mutex_lock(&cache->lock);
    const size_t used = cache->used_bytes;
    pthread_mutex_unlock(&cache->lock);
    return used;
}

/* Evict ready, unpinned entries (oldest use first) until `needed` fits the budget. Caller
 * holds the lock. Returns false when the budget cannot be met. */
static bool ddc_cache_make_room(ddc_cache_t *cache, size_t needed)
{
    if (needed > cache->max_bytes) {
        return false;
    }
    while (cache->used_bytes + needed > cache->max_bytes) {
        ddc_cache_entry_t **victim_link = NULL;
        for (ddc_cache_entry_t **link = &cache->entries; *link != NULL; link = &(*link)->next) {
            ddc_cache_entry_t *candidate = *link;
            if (!candidate->ready || candidate->refcount > 0) {
                continue;
            }
            if (victim_link == NULL || candidate->last_use < (*victim_link)->last_use) {
                victim_link = link;
            }
        }
        if (victim_link == NULL) {
            return false; /* everything left is pinned or building */
        }
        ddc_cache_entry_t *victim = *victim_link;
        *victim_link = victim->next;
        cache->used_bytes -= victim->bytes;
        ddc_cache_entry_free(victim);
    }
    return true;
}

/* One pass over the loop: gather (modulo the seam), rotate, run the front cascade, quantize
 * to half-scale ci16. Runs without the cache lock held. */
static bool ddc_cache_build(ddc_cache_entry_t *entry,
                            const iq_ci16_t *source_samples,
                            uint64_t source_sample_count,
                            uint32_t source_rate_hz,
                            const ddc_plan_t *front_plan)
{
    const int64_t loop_samples = (int64_t)source_sample_count;
    const int64_t ratio = (int64_t)front_plan->ratio;
    const int64_t reach = (int64_t)front_plan->history_source_samples;
    const int64_t feed_start = -reach;
    const int64_t feed_total = ((int64_t)entry->sample_count - 1) * ratio + 2 * reach + 1;

    ddc_exec_t *exec = malloc(sizeof(*exec));
    float complex *rotated = malloc(DDC_CACHE_BUILD_CHUNK * sizeof(*rotated));
    const size_t out_capacity = DDC_CACHE_BUILD_CHUNK / 2U + 2U;
    float complex *out = malloc(out_capacity * sizeof(*out));
    bool ok = exec != NULL && rotated != NULL && out != NULL;

    if (ok) {
        ddc_exec_init(exec, front_plan, feed_start);
        const uint64_t step_q64 = nco_phase_step_q64(entry->applied_shift_hz, (double)source_rate_hz);
        uint64_t written = 0;
        for (int64_t done = 0; done < feed_total && ok; done += (int64_t)DDC_CACHE_BUILD_CHUNK) {
            const int64_t remaining = feed_total - done;
            const size_t chunk = remaining < (int64_t)DDC_CACHE_BUILD_CHUNK
                ? (size_t)remaining
                : DDC_CACHE_BUILD_CHUNK;
            /* Oscillator re-anchored per chunk at the wrapped source index: the applied shift
             * is a whole number of cycles per loop, so the phase is the same function of the
             * wrapped and the unwrapped index, and the recurrence never drifts more than a
             * chunk's worth of rounding. */
            const int64_t chunk_start = feed_start + done;
            const uint64_t wrapped_start =
                (uint64_t)(((chunk_start % loop_samples) + loop_samples) % loop_samples);
            const double phase0 = nco_phase_rad_at(step_q64, wrapped_start);
            double osc_c = cos(phase0);
            double osc_s = sin(phase0);
            const double step = 2.0 * M_PI * entry->applied_shift_hz / (double)source_rate_hz;
            const double step_c = cos(step);
            const double step_s = sin(step);
            int64_t wrapped = (int64_t)wrapped_start;
            for (size_t k = 0; k < chunk; k++) {
                const iq_ci16_t sample = source_samples[wrapped];
                wrapped++;
                if (wrapped == loop_samples) {
                    wrapped = 0;
                }
                const double sample_i = (double)sample.i;
                const double sample_q = (double)sample.q;
                rotated[k] = CMPLXF((float)(sample_i * osc_c - sample_q * osc_s),
                                    (float)(sample_i * osc_s + sample_q * osc_c));
                const double next_c = osc_c * step_c - osc_s * step_s;
                const double next_s = osc_s * step_c + osc_c * step_s;
                osc_c = next_c;
                osc_s = next_s;
                if ((k & 0xffU) == 0xffU) {
                    const double inv = 1.0 / sqrt(osc_c * osc_c + osc_s * osc_s);
                    osc_c *= inv;
                    osc_s *= inv;
                }
            }
            const size_t produced = ddc_exec_push(exec, rotated, chunk, out, out_capacity);
            if (written + produced > entry->sample_count) {
                ok = false; /* alignment bug guard; must never happen */
                break;
            }
            for (size_t k = 0; k < produced; k++) {
                entry->samples[written + k].i =
                    sim_clip_i16(DDC_CACHE_SAMPLE_SCALE * (double)crealf(out[k]));
                entry->samples[written + k].q =
                    sim_clip_i16(DDC_CACHE_SAMPLE_SCALE * (double)cimagf(out[k]));
            }
            written += produced;
        }
        ok = ok && written == entry->sample_count;
    }

    free(out);
    free(rotated);
    free(exec);
    return ok;
}

const ddc_cache_entry_t *ddc_cache_acquire(ddc_cache_t *cache,
                                           const char *source_id,
                                           const iq_ci16_t *source_samples,
                                           uint64_t source_sample_count,
                                           uint32_t source_rate_hz,
                                           double shift_hz,
                                           uint32_t intermediate_rate_hz,
                                           const ddc_plan_t *front_plan)
{
    if (cache == NULL || front_plan == NULL || source_sample_count == 0 ||
        front_plan->ratio == 0 || source_sample_count % front_plan->ratio != 0) {
        return NULL;
    }
    /* Snap to whole cycles per loop so the circular build is exactly periodic. */
    const double cycles_exact = shift_hz * (double)source_sample_count / (double)source_rate_hz;
    const int64_t cycles = (int64_t)llround(cycles_exact);
    const double applied_shift_hz =
        (double)cycles * (double)source_rate_hz / (double)source_sample_count;
    const uint64_t intermediate_samples = source_sample_count / front_plan->ratio;
    const size_t bytes = (size_t)intermediate_samples * sizeof(iq_ci16_t);

    pthread_mutex_lock(&cache->lock);
    for (;;) {
        ddc_cache_entry_t *found = NULL;
        for (ddc_cache_entry_t *entry = cache->entries; entry != NULL; entry = entry->next) {
            if (entry->shift_cycles == cycles &&
                entry->intermediate_rate_hz == intermediate_rate_hz &&
                strncmp(entry->source_id, source_id, sizeof(entry->source_id)) == 0) {
                found = entry;
                break;
            }
        }
        if (found == NULL) {
            break; /* build it below */
        }
        if (found->ready) {
            found->refcount++;
            found->last_use = ++cache->use_counter;
            pthread_mutex_unlock(&cache->lock);
            return found;
        }
        /* Someone else is building this key: wait and re-scan (the entry may have been
         * removed if their build failed). */
        pthread_cond_wait(&cache->built, &cache->lock);
    }

    if (!ddc_cache_make_room(cache, bytes)) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    ddc_cache_entry_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    snprintf(entry->source_id, sizeof(entry->source_id), "%s", source_id);
    entry->shift_cycles = cycles;
    entry->intermediate_rate_hz = intermediate_rate_hz;
    entry->applied_shift_hz = applied_shift_hz;
    entry->sample_count = intermediate_samples;
    entry->bytes = bytes;
    entry->samples = malloc(bytes);
    entry->ready = false;
    entry->next = cache->entries;
    cache->entries = entry;
    cache->used_bytes += bytes;
    pthread_mutex_unlock(&cache->lock);

    const bool built = entry->samples != NULL &&
        ddc_cache_build(entry, source_samples, source_sample_count, source_rate_hz, front_plan);

    pthread_mutex_lock(&cache->lock);
    if (!built) {
        /* Unlink and drop; waiters re-scan and may retry the build themselves. */
        for (ddc_cache_entry_t **link = &cache->entries; *link != NULL; link = &(*link)->next) {
            if (*link == entry) {
                *link = entry->next;
                break;
            }
        }
        cache->used_bytes -= entry->bytes;
        ddc_cache_entry_free(entry);
        pthread_cond_broadcast(&cache->built);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    entry->ready = true;
    entry->refcount = 1;
    entry->last_use = ++cache->use_counter;
    pthread_cond_broadcast(&cache->built);
    pthread_mutex_unlock(&cache->lock);
    return entry;
}

void ddc_cache_release(ddc_cache_t *cache, const ddc_cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL) {
        return;
    }
    ddc_cache_entry_t *mutable_entry = (ddc_cache_entry_t *)(uintptr_t)entry;
    pthread_mutex_lock(&cache->lock);
    if (mutable_entry->refcount > 0) {
        mutable_entry->refcount--;
    }
    mutable_entry->last_use = ++cache->use_counter;
    pthread_mutex_unlock(&cache->lock);
}
