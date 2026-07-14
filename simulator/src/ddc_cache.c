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
#include <unistd.h>

/* Source samples gathered/rotated per executor push during a build. Also the absolute
 * oscillator re-anchor grid: the rotation of source index i is always computed by recurrence
 * from the anchor at floor(i / CHUNK) * CHUNK, so rotated values -- and therefore entry
 * content -- are bit-identical no matter how a build is sliced across threads. */
#define DDC_CACHE_BUILD_CHUNK 65536U
/* Builds run one full loop through the front cascade at the source rate (the expensive part
 * of a cold retune). Slices are independent thanks to history re-priming, so they parallelize
 * across threads; each slice must be large enough that its re-primed history is noise. */
#define DDC_CACHE_BUILD_MAX_THREADS 8U
#define DDC_CACHE_BUILD_MIN_SLICE 16384U /* intermediate samples per slice, minimum */

static unsigned g_ddc_cache_build_threads = 0; /* 0 = auto (online CPUs, capped) */

void ddc_cache_set_build_threads(unsigned threads)
{
    g_ddc_cache_build_threads = threads;
}

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
    /* Drain in-flight background builds: a builder still holds pointers to its entry and the
     * caller's source samples until it finishes (failed builds unlink their entry, so a
     * rescan finding only ready entries means no builder is running). */
    pthread_mutex_lock(&cache->lock);
    for (;;) {
        bool building = false;
        for (ddc_cache_entry_t *scan = cache->entries; scan != NULL; scan = scan->next) {
            if (!scan->ready) {
                building = true;
                break;
            }
        }
        if (!building) {
            break;
        }
        pthread_cond_wait(&cache->built, &cache->lock);
    }
    pthread_mutex_unlock(&cache->lock);
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

static int64_t ddc_cache_floor_div(int64_t numerator, int64_t denominator)
{
    const int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && (numerator < 0) != (denominator < 0))
        ? quotient - 1
        : quotient;
}

/* Gather and rotate the absolute source range [start, start + count) into dst, wrapping the
 * loop seam. The oscillator recurrence is always replayed from the absolute anchor at
 * floor(i / CHUNK) * CHUNK, so a given index rotates to bit-identical values regardless of
 * where a build slice begins (the idle replay to mid-chunk starts is at most one chunk). */
static void ddc_cache_rotate_range(float complex *dst,
                                   const iq_ci16_t *source_samples,
                                   int64_t loop_samples,
                                   uint64_t step_q64,
                                   double step_c,
                                   double step_s,
                                   int64_t start,
                                   size_t count)
{
    size_t filled = 0;
    while (filled < count) {
        const int64_t index = start + (int64_t)filled;
        const int64_t anchor = ddc_cache_floor_div(index, (int64_t)DDC_CACHE_BUILD_CHUNK)
            * (int64_t)DDC_CACHE_BUILD_CHUNK;
        const int64_t segment_end = anchor + (int64_t)DDC_CACHE_BUILD_CHUNK;
        const size_t segment = (size_t)(segment_end - index) < count - filled
            ? (size_t)(segment_end - index)
            : count - filled;

        const double phase0 = nco_phase_rad_at(step_q64, (uint64_t)anchor);
        double osc_c = cos(phase0);
        double osc_s = sin(phase0);
        uint64_t k = 0;
        for (int64_t idle = anchor; idle < index; idle++, k++) {
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
        int64_t wrapped = ((index % loop_samples) + loop_samples) % loop_samples;
        for (size_t j = 0; j < segment; j++, k++) {
            const double sample_i = (double)source_samples[wrapped].i;
            const double sample_q = (double)source_samples[wrapped].q;
            dst[filled + j] = CMPLXF((float)(sample_i * osc_c - sample_q * osc_s),
                                     (float)(sample_i * osc_s + sample_q * osc_c));
            wrapped++;
            if (wrapped == loop_samples) {
                wrapped = 0;
            }
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
        filled += segment;
    }
}

typedef struct {
    ddc_cache_entry_t *entry;
    const iq_ci16_t *source_samples;
    int64_t loop_samples;
    const ddc_plan_t *front_plan;
    uint64_t step_q64;
    double step_c;
    double step_s;
    uint64_t out_first; /* intermediate output range [out_first, out_last) of this slice */
    uint64_t out_last;
    bool ok;
} ddc_cache_build_slice_t;

/* Build one contiguous slice of the intermediate: prime a fresh executor with the slice's
 * history and stream its span through the front cascade. Because every output is a single
 * dot product over absolutely-anchored rotated values, slice boundaries do not affect the
 * result. */
static void *ddc_cache_build_slice(void *arg)
{
    ddc_cache_build_slice_t *slice = arg;
    const ddc_plan_t *plan = slice->front_plan;
    const int64_t ratio = (int64_t)plan->ratio;
    const int64_t reach = (int64_t)plan->history_source_samples;
    const int64_t feed_start = (int64_t)slice->out_first * ratio - reach;
    const int64_t feed_total = ((int64_t)(slice->out_last - slice->out_first) - 1) * ratio + 2 * reach + 1;

    ddc_exec_t *exec = malloc(sizeof(*exec));
    float complex *rotated = malloc(DDC_CACHE_BUILD_CHUNK * sizeof(*rotated));
    const size_t out_capacity = DDC_CACHE_BUILD_CHUNK / 2U + 2U;
    float complex *out = malloc(out_capacity * sizeof(*out));
    bool ok = exec != NULL && rotated != NULL && out != NULL;

    if (ok) {
        ddc_exec_init(exec, plan, feed_start);
        uint64_t written = slice->out_first;
        for (int64_t done = 0; done < feed_total && ok; done += (int64_t)DDC_CACHE_BUILD_CHUNK) {
            const int64_t remaining = feed_total - done;
            const size_t chunk = remaining < (int64_t)DDC_CACHE_BUILD_CHUNK
                ? (size_t)remaining
                : DDC_CACHE_BUILD_CHUNK;
            ddc_cache_rotate_range(rotated, slice->source_samples, slice->loop_samples,
                                   slice->step_q64, slice->step_c, slice->step_s,
                                   feed_start + done, chunk);
            const size_t produced = ddc_exec_push(exec, rotated, chunk, out, out_capacity);
            if (written + produced > slice->out_last) {
                ok = false; /* alignment bug guard; must never happen */
                break;
            }
            for (size_t k = 0; k < produced; k++) {
                slice->entry->samples[written + k].i =
                    sim_clip_i16(DDC_CACHE_SAMPLE_SCALE * (double)crealf(out[k]));
                slice->entry->samples[written + k].q =
                    sim_clip_i16(DDC_CACHE_SAMPLE_SCALE * (double)cimagf(out[k]));
            }
            written += produced;
        }
        ok = ok && written == slice->out_last;
    }

    free(out);
    free(rotated);
    free(exec);
    slice->ok = ok;
    return NULL;
}

/* One pass over the loop: gather (modulo the seam), rotate, run the front cascade, quantize
 * to half-scale ci16. Runs without the cache lock held. Sliced across threads (history
 * re-priming makes slices independent); content is bit-identical for any thread count. */
static bool ddc_cache_build(ddc_cache_entry_t *entry,
                            const iq_ci16_t *source_samples,
                            uint64_t source_sample_count,
                            uint32_t source_rate_hz,
                            const ddc_plan_t *front_plan)
{
    unsigned threads = g_ddc_cache_build_threads;
    if (threads == 0) {
        const long online = sysconf(_SC_NPROCESSORS_ONLN);
        threads = online > 0 ? (unsigned)online : 1U;
    }
    if (threads > DDC_CACHE_BUILD_MAX_THREADS) {
        threads = DDC_CACHE_BUILD_MAX_THREADS;
    }
    const uint64_t max_slices = entry->sample_count / DDC_CACHE_BUILD_MIN_SLICE;
    if ((uint64_t)threads > max_slices) {
        threads = max_slices > 0 ? (unsigned)max_slices : 1U;
    }

    const double step = 2.0 * M_PI * entry->applied_shift_hz / (double)source_rate_hz;
    ddc_cache_build_slice_t slices[DDC_CACHE_BUILD_MAX_THREADS];
    pthread_t slice_threads[DDC_CACHE_BUILD_MAX_THREADS];
    for (unsigned t = 0; t < threads; t++) {
        slices[t] = (ddc_cache_build_slice_t){
            .entry = entry,
            .source_samples = source_samples,
            .loop_samples = (int64_t)source_sample_count,
            .front_plan = front_plan,
            .step_q64 = nco_phase_step_q64(entry->applied_shift_hz, (double)source_rate_hz),
            .step_c = cos(step),
            .step_s = sin(step),
            .out_first = entry->sample_count * t / threads,
            .out_last = entry->sample_count * (t + 1U) / threads,
            .ok = false,
        };
    }
    unsigned started = 0;
    for (unsigned t = 1; t < threads; t++) {
        if (pthread_create(&slice_threads[t], NULL, ddc_cache_build_slice, &slices[t]) != 0) {
            break;
        }
        started = t;
    }
    ddc_cache_build_slice(&slices[0]);
    bool ok = slices[0].ok;
    for (unsigned t = 1; t <= started; t++) {
        pthread_join(slice_threads[t], NULL);
        ok = ok && slices[t].ok;
    }
    /* If some worker threads never started, run their slices inline. */
    for (unsigned t = started + 1U; t < threads; t++) {
        ddc_cache_build_slice(&slices[t]);
        ok = ok && slices[t].ok;
    }
    return ok;
}

/* Arguments a background builder needs to outlive the acquire call. The source samples are
 * owned by the asset cache, which lives until shutdown (ddc_cache_destroy waits for builds
 * before the assets are freed); the plan lives in the static plan cache. */
typedef struct {
    ddc_cache_t *cache;
    ddc_cache_entry_t *entry;
    const iq_ci16_t *source_samples;
    uint64_t source_sample_count;
    uint32_t source_rate_hz;
    const ddc_plan_t *front_plan;
} ddc_cache_build_task_t;

/* Finish a build under the lock: publish the entry or unlink it on failure, and wake both
 * blocking acquirers and ddc_cache_destroy. Caller passes the pin count for the entry
 * (1 when the builder's caller keeps it, 0 for background builds). */
static void ddc_cache_finish_build(ddc_cache_t *cache, ddc_cache_entry_t *entry, bool built, uint32_t pins)
{
    if (!built) {
        for (ddc_cache_entry_t **link = &cache->entries; *link != NULL; link = &(*link)->next) {
            if (*link == entry) {
                *link = entry->next;
                break;
            }
        }
        cache->used_bytes -= entry->bytes;
        ddc_cache_entry_free(entry);
    } else {
        entry->ready = true;
        entry->refcount = pins;
        entry->last_use = ++cache->use_counter;
    }
    pthread_cond_broadcast(&cache->built);
}

static void *ddc_cache_background_build_main(void *arg)
{
    ddc_cache_build_task_t *task = arg;
    const bool built = task->entry->samples != NULL &&
        ddc_cache_build(task->entry, task->source_samples, task->source_sample_count,
                        task->source_rate_hz, task->front_plan);
    pthread_mutex_lock(&task->cache->lock);
    ddc_cache_finish_build(task->cache, task->entry, built, 0);
    pthread_mutex_unlock(&task->cache->lock);
    free(task);
    return NULL;
}

const ddc_cache_entry_t *ddc_cache_acquire(ddc_cache_t *cache,
                                           const char *source_id,
                                           const iq_ci16_t *source_samples,
                                           uint64_t source_sample_count,
                                           uint32_t source_rate_hz,
                                           double shift_hz,
                                           uint32_t intermediate_rate_hz,
                                           const ddc_plan_t *front_plan,
                                           bool wait_for_build)
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
        if (!wait_for_build) {
            /* A build for this key is already running; caller renders the direct path
             * meanwhile and re-tries next block. */
            pthread_mutex_unlock(&cache->lock);
            return NULL;
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

    if (!wait_for_build) {
        /* Kick off a detached background build and return immediately; the caller keeps
         * rendering through the direct full-rate cascade until a later acquire finds the
         * entry ready. */
        ddc_cache_build_task_t *task = malloc(sizeof(*task));
        bool spawned = false;
        if (task != NULL && entry->samples != NULL) {
            *task = (ddc_cache_build_task_t){
                .cache = cache,
                .entry = entry,
                .source_samples = source_samples,
                .source_sample_count = source_sample_count,
                .source_rate_hz = source_rate_hz,
                .front_plan = front_plan,
            };
            pthread_t builder;
            if (pthread_create(&builder, NULL, ddc_cache_background_build_main, task) == 0) {
                pthread_detach(builder);
                spawned = true;
            }
        }
        if (!spawned) {
            free(task);
            ddc_cache_finish_build(cache, entry, false, 0); /* unlink; next block retries */
        }
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    pthread_mutex_unlock(&cache->lock);

    const bool built = entry->samples != NULL &&
        ddc_cache_build(entry, source_samples, source_sample_count, source_rate_hz, front_plan);

    pthread_mutex_lock(&cache->lock);
    ddc_cache_finish_build(cache, entry, built, 1);
    pthread_mutex_unlock(&cache->lock);
    return built ? entry : NULL;
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
