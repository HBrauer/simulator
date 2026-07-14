#include "asset_cache.h"
#include "iq_file_reader.h"
#include "scenario.h"
#include "util.h"
#include "wav_reader.h"

#include <fcntl.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HILBERT_RADIUS 95

/* Target RMS the audio is normalised to at load, so power_dbm means the same thing for AM/SSB
 * regardless of how hot the source file was mastered (a full-scale sine already sits here). */
#define AUDIO_TARGET_RMS 0.70710678118654752440 /* 1/sqrt(2) */

static double normalize_audio_rms(float *samples, size_t count)
{
    if (count == 0) {
        return 1.0;
    }
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum_sq += (double)samples[i] * (double)samples[i];
    }
    const double rms = sqrt(sum_sq / (double)count);
    if (!(rms > 1e-9)) {
        return 1.0;
    }
    const double gain = AUDIO_TARGET_RMS / rms;
    for (size_t i = 0; i < count; i++) {
        samples[i] = (float)((double)samples[i] * gain);
    }
    return gain;
}

static bool build_audio_helpers(cached_asset_t *asset, char *error, size_t error_size)
{
    const size_t count = (size_t)asset->sample_count;
    asset->audio_hilbert = calloc(count, sizeof(*asset->audio_hilbert));
    asset->audio_integral = calloc(count + 1U, sizeof(*asset->audio_integral));
    if (asset->audio_hilbert == NULL || asset->audio_integral == NULL) {
        snprintf(error, error_size, "asset_cache_alloc_failed");
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        asset->audio_integral[i + 1U] = asset->audio_integral[i] + (double)asset->audio_samples[i];
    }

    for (size_t i = 0; i < count; i++) {
        double acc = 0.0;
        for (int n = -HILBERT_RADIUS; n <= HILBERT_RADIUS; n++) {
            if (n == 0 || (n & 1) == 0) {
                continue;
            }
            const int64_t index = (int64_t)i - (int64_t)n;
            if (index < 0 || (uint64_t)index >= asset->sample_count) {
                continue;
            }
            const double window = 0.54 + 0.46 * cos(M_PI * (double)n / (double)HILBERT_RADIUS);
            const double coeff = (2.0 / (M_PI * (double)n)) * window;
            acc += coeff * (double)asset->audio_samples[index];
        }
        asset->audio_hilbert[i] = (float)acc;
    }
    return true;
}

/* ---- audio -> complex-baseband IQ pre-render (T3) ----
 *
 * Each signal that references an audio source is modulated into complex baseband once here, at a
 * low intermediate rate matched to its content bandwidth, so the streaming hot path treats it as
 * an ordinary IQ source (see renderer.c). The modulation formulas are the same ones the renderer
 * used to evaluate per output sample at the full rate -- reused verbatim at the low rate. If ever
 * hour-long clips are needed, this is the seam to swap for streaming low-rate synthesis behind the
 * same cached-asset interface without touching the renderer. */

static double prerender_audio_linear_at(const float *samples, uint64_t sample_count, double position)
{
    if (samples == NULL || sample_count == 0 || position < 0.0) {
        return 0.0;
    }
    uint64_t index = (uint64_t)position;
    if (index + 1ULL >= sample_count) {
        return index < sample_count ? (double)samples[index] : 0.0;
    }
    const double fraction = position - (double)index;
    const double a = (double)samples[index];
    const double b = (double)samples[index + 1ULL];
    return a + (b - a) * fraction;
}

static double prerender_audio_integral_at(const cached_asset_t *asset, double position)
{
    if (asset->audio_integral == NULL || asset->sample_count == 0 || position <= 0.0) {
        return 0.0;
    }
    uint64_t index = (uint64_t)position;
    if (index >= asset->sample_count) {
        return asset->audio_integral[asset->sample_count];
    }
    const double fraction = position - (double)index;
    return asset->audio_integral[index] + fraction * (double)asset->audio_samples[index];
}

/* Unshifted complex baseband for one modulation at a fractional audio position. Identical to the
 * base computation the renderer's full-rate synthesis produced; the frequency shift and amplitude
 * calibration are applied later by the IQ render path. */
static void prerender_base_sample(const cached_asset_t *asset, const scenario_signal_t *signal, double audio_rate, double audio_position, double *base_i, double *base_q)
{
    if (signal->modulation == SCENARIO_MODULATION_WBFM) {
        const double integral = prerender_audio_integral_at(asset, audio_position);
        const double fm_phase = 2.0 * M_PI * signal->fm_deviation_hz * integral / audio_rate;
        sincos(fm_phase, base_q, base_i);
    } else if (signal->modulation == SCENARIO_MODULATION_AM) {
        const double audio = prerender_audio_linear_at(asset->audio_samples, asset->sample_count, audio_position);
        const double envelope = (1.0 + signal->am_depth * audio) / (1.0 + signal->am_depth);
        *base_i = envelope < 0.0 ? 0.0 : envelope;
        *base_q = 0.0;
    } else {
        const double audio = prerender_audio_linear_at(asset->audio_samples, asset->sample_count, audio_position);
        const double hilbert = prerender_audio_linear_at(asset->audio_hilbert, asset->sample_count, audio_position);
        *base_i = audio;
        *base_q = signal->modulation == SCENARIO_MODULATION_USB ? hilbert : -hilbert;
    }
}

/* Intermediate pre-render rate (AF3): oversample x content bandwidth, floored to the declared
 * signal bandwidth and capped at the configured ceiling, rounded up so the value is exact. */
static uint32_t prerender_rate_hz(const scenario_signal_t *signal, const scenario_source_t *source, const prerender_params_t *params)
{
    const double audio_rate = (double)source->sample_rate_hz;
    double content_bw;
    if (signal->modulation == SCENARIO_MODULATION_WBFM) {
        content_bw = 2.0 * (signal->fm_deviation_hz + audio_rate / 2.0); /* Carson */
    } else if (signal->modulation == SCENARIO_MODULATION_AM) {
        content_bw = audio_rate; /* DSB: 2 * audio_rate/2 */
    } else {
        content_bw = audio_rate / 2.0; /* SSB */
    }
    double rate = ceil(params->oversample * content_bw);
    if (rate < (double)signal->bandwidth_hz) {
        rate = (double)signal->bandwidth_hz;
    }
    if (rate > (double)params->max_rate_hz) {
        rate = (double)params->max_rate_hz;
    }
    if (rate < 1.0) {
        rate = 1.0;
    }
    return (uint32_t)rate;
}

static int16_t prerender_clip_i16(double value)
{
    const double rounded = round(value);
    if (rounded > 32767.0) {
        return 32767;
    }
    if (rounded < -32768.0) {
        return -32768;
    }
    return (int16_t)rounded;
}

/* Synthesise the complex baseband for one signal into `buffer` (pr_count samples) and return the
 * peak-normalisation gain to fold back into source_gain. FM is constant-modulus (peak exactly 1,
 * scale exactly 32767); AM/SSB scan for the data-dependent peak. A silent asset keeps gain 1.0 and
 * renders as zeros (no divide-by-zero). Shared by the load-time engine and the test entry point. */
static double prerender_fill_buffer(const cached_asset_t *asset, const scenario_signal_t *signal, double audio_rate, uint32_t rate, uint64_t pr_count, iq_ci16_t *buffer)
{
    double peak = 0.0;
    if (signal->modulation == SCENARIO_MODULATION_WBFM) {
        peak = 1.0;
    } else {
        for (uint64_t i = 0; i < pr_count; i++) {
            const double position = (double)i * audio_rate / (double)rate;
            double bi = 0.0;
            double bq = 0.0;
            prerender_base_sample(asset, signal, audio_rate, position, &bi, &bq);
            const double ai = fabs(bi);
            const double aq = fabs(bq);
            if (ai > peak) {
                peak = ai;
            }
            if (aq > peak) {
                peak = aq;
            }
        }
    }
    const double scale = peak > 1e-12 ? 32767.0 / peak : 0.0;
    for (uint64_t i = 0; i < pr_count; i++) {
        const double position = (double)i * audio_rate / (double)rate;
        double bi = 0.0;
        double bq = 0.0;
        prerender_base_sample(asset, signal, audio_rate, position, &bi, &bq);
        buffer[i].i = prerender_clip_i16(bi * scale);
        buffer[i].q = prerender_clip_i16(bq * scale);
    }
    return peak > 1e-12 ? peak : 1.0;
}

static bool prerender_signals(asset_cache_t *cache, const scenario_t *scenario, const prerender_params_t *params, size_t max_bytes, size_t *total_bytes, char *error, size_t error_size)
{
    for (size_t s = 0; s < scenario->signal_count; s++) {
        const scenario_signal_t *signal = &scenario->signals[s];
        const scenario_source_t *source = scenario_find_source(scenario, signal->source_reference);
        if (source == NULL || source->source_kind != SCENARIO_SOURCE_AUDIO_FILE) {
            continue;
        }
        const cached_asset_t *asset = asset_cache_find(cache, signal->source_reference);
        if (asset == NULL) {
            continue;
        }

        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        const uint32_t rate = prerender_rate_hz(signal, source, params);
        const double audio_rate = (double)source->sample_rate_hz;
        const uint64_t pr_count = (uint64_t)llround((double)asset->sample_count * (double)rate / audio_rate);

        if (pr_count > SIZE_MAX / sizeof(iq_ci16_t)) {
            snprintf(error, error_size, "asset_cache_too_large");
            return false;
        }
        const size_t pr_bytes = (size_t)pr_count * sizeof(iq_ci16_t);
        if (max_bytes > 0 && (pr_bytes > max_bytes || *total_bytes > max_bytes - pr_bytes)) {
            snprintf(error, error_size, "asset_cache_limit_exceeded");
            return false;
        }
        *total_bytes += pr_bytes;

        iq_ci16_t *buffer = NULL;
        if (pr_count > 0) {
            buffer = calloc((size_t)pr_count, sizeof(*buffer));
            if (buffer == NULL) {
                snprintf(error, error_size, "asset_cache_alloc_failed");
                return false;
            }
        }

        const double gain = pr_count > 0 ? prerender_fill_buffer(asset, signal, audio_rate, rate, pr_count, buffer) : 1.0;

        cache->prerenders[s].valid = true;
        cache->prerenders[s].sample_rate_hz = rate;
        cache->prerenders[s].sample_count = pr_count;
        cache->prerenders[s].samples = buffer;
        cache->prerenders[s].gain = gain;

        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        const double synth_ms = (double)(t_end.tv_sec - t_start.tv_sec) * 1000.0 +
                                (double)(t_end.tv_nsec - t_start.tv_nsec) / 1.0e6;
        fprintf(stderr, "prerender signal %s: %u Hz, %llu samples, %.2f MB, %.1f ms\n",
                signal->signal_id, rate, (unsigned long long)pr_count,
                (double)pr_bytes / (1024.0 * 1024.0), synth_ms);
    }
    return true;
}

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size)
{
    const prerender_params_t params = {.oversample = 2.0, .max_rate_hz = 4000000U};
    return asset_cache_load_limited(cache, scenario, 0, 4096, &params, error, error_size);
}

/* Load one IQ file into `out`, either heap-copied (counting against the budget) or, when it does
 * not fit the remaining budget, memory-mapped read-only. Writes directly into `out` and zeroes it
 * on failure so asset_cache_free can safely skip it. Shared by single-file sources and each
 * passthrough rate variant. */
static bool load_iq_buffer(const char *path, uint64_t sample_count, size_t max_bytes, size_t *total_bytes, size_t batch_samples, cached_iq_buffer_t *out, char *error, size_t error_size)
{
    memset(out, 0, sizeof(*out));
    out->sample_count = sample_count;
    if (sample_count > SIZE_MAX / sizeof(iq_ci16_t)) {
        snprintf(error, error_size, "asset_cache_too_large");
        return false;
    }
    const size_t bytes = (size_t)sample_count * sizeof(iq_ci16_t);
    const bool over_budget = max_bytes > 0 && (bytes > max_bytes || *total_bytes > max_bytes - bytes);
    if (over_budget) {
        if (bytes == 0) {
            snprintf(error, error_size, "asset_cache_limit_exceeded");
            return false;
        }
        const int fd = open(path, O_RDONLY);
        if (fd < 0) {
            snprintf(error, error_size, "asset_cache_open_failed");
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != bytes) {
            close(fd);
            snprintf(error, error_size, "asset_cache_size_mismatch");
            return false;
        }
        void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (map == MAP_FAILED) {
            snprintf(error, error_size, "asset_cache_mmap_failed");
            return false;
        }
        (void)madvise(map, bytes, MADV_WILLNEED);
        out->samples = map;
        out->mmapped = true;
        out->map_bytes = bytes;
        fprintf(stderr, "asset_cache: mmap %s (%zu bytes, over cache budget)\n", path, bytes);
        return true;
    }
    *total_bytes += bytes;
    out->samples = calloc((size_t)sample_count, sizeof(iq_ci16_t));
    if (out->samples == NULL) {
        snprintf(error, error_size, "asset_cache_alloc_failed");
        return false;
    }
    iq_file_reader_t reader;
    if (!iq_file_reader_open(&reader, path, error, error_size)) {
        free(out->samples);
        out->samples = NULL;
        return false;
    }
    size_t read_count = 0;
    bool ok = true;
    while (read_count < sample_count) {
        const size_t remaining = (size_t)sample_count - read_count;
        const size_t wanted = remaining < batch_samples ? remaining : batch_samples;
        size_t batch_read = 0;
        if (!iq_file_reader_read(&reader, read_count, out->samples + read_count, wanted, &batch_read) || batch_read != wanted) {
            ok = false;
            break;
        }
        read_count += batch_read;
    }
    iq_file_reader_close(&reader);
    if (!ok || read_count != sample_count) {
        free(out->samples);
        out->samples = NULL;
        snprintf(error, error_size, "asset_cache_read_failed");
        return false;
    }
    return true;
}

bool asset_cache_load_limited(asset_cache_t *cache, const scenario_t *scenario, size_t max_bytes, size_t batch_samples, const prerender_params_t *prerender, char *error, size_t error_size)
{
    memset(cache, 0, sizeof(*cache));
    if (batch_samples == 0) {
        batch_samples = 4096;
    }
    const prerender_params_t default_prerender = {.oversample = 2.0, .max_rate_hz = 4000000U};
    if (prerender == NULL) {
        prerender = &default_prerender;
    }
    cache->asset_count = scenario->source_count;
    size_t total_bytes = 0;
    for (size_t i = 0; i < scenario->source_count; i++) {
        const scenario_source_t *source = &scenario->sources[i];
        cached_asset_t *asset = &cache->assets[i];
        sim_strlcpy(asset->source_id, source->id, sizeof(asset->source_id));
        asset->source_kind = source->source_kind;
        asset->sample_count = source->sample_count;

        /* Passthrough source: load one buffer per rate variant; the top-level `file` is unused. */
        if (source->source_kind == SCENARIO_SOURCE_IQ_FILE && source->passthrough_variant_count > 0) {
            asset->variant_count = source->passthrough_variant_count;
            for (size_t k = 0; k < source->passthrough_variant_count; k++) {
                const scenario_passthrough_variant_t *v = &source->passthrough_variants[k];
                if (!load_iq_buffer(v->file, v->sample_count, max_bytes, &total_bytes, batch_samples, &asset->variants[k], error, error_size)) {
                    asset_cache_free(cache);
                    return false;
                }
                asset->variants[k].sample_rate_hz = v->sample_rate_hz;
            }
            continue;
        }

        if (asset->source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
            /* Audio stays on the RAM path (it is rewritten in place during normalisation and
             * pre-render), so it cannot be mmap-backed and fails hard when over budget. */
            if (asset->sample_count > (SIZE_MAX - sizeof(*asset->audio_integral)) /
                                          (sizeof(*asset->audio_samples) + sizeof(*asset->audio_hilbert) +
                                           sizeof(*asset->audio_integral))) {
                snprintf(error, error_size, "asset_cache_too_large");
                asset_cache_free(cache);
                return false;
            }
            const size_t asset_bytes = (size_t)asset->sample_count *
                              (sizeof(*asset->audio_samples) + sizeof(*asset->audio_hilbert) +
                               sizeof(*asset->audio_integral)) +
                          sizeof(*asset->audio_integral);
            if (max_bytes > 0 && (asset_bytes > max_bytes || total_bytes > max_bytes - asset_bytes)) {
                snprintf(error, error_size, "asset_cache_limit_exceeded");
                asset_cache_free(cache);
                return false;
            }
            total_bytes += asset_bytes;

            wav_audio_t audio;
            if (!wav_reader_load_mono_f32(source->file, &audio, error, error_size)) {
                asset_cache_free(cache);
                return false;
            }
            if (audio.frame_count != asset->sample_count || audio.sample_rate_hz != source->sample_rate_hz) {
                wav_audio_free(&audio);
                snprintf(error, error_size, "wav_cache_mismatch");
                asset_cache_free(cache);
                return false;
            }
            asset->audio_samples = audio.samples;
            asset->normalization_gain = normalize_audio_rms(asset->audio_samples, (size_t)asset->sample_count);
            if (!build_audio_helpers(asset, error, error_size)) {
                asset_cache_free(cache);
                return false;
            }
        } else {
            cached_iq_buffer_t buf;
            if (!load_iq_buffer(source->file, source->sample_count, max_bytes, &total_bytes, batch_samples, &buf, error, error_size)) {
                asset_cache_free(cache);
                return false;
            }
            asset->samples = buf.samples;
            asset->mmapped = buf.mmapped;
            asset->map_bytes = buf.map_bytes;
        }
    }

    if (!prerender_signals(cache, scenario, prerender, max_bytes, &total_bytes, error, error_size)) {
        asset_cache_free(cache);
        return false;
    }

    /* The Hilbert transform and running integral were only needed to synthesise the pre-renders;
     * the render path never touches them again, so reclaim them now (the dominant per-signal audio
     * memory). audio_samples is kept -- it is small and still inspected by the RMS-normalisation
     * test. */
    for (size_t i = 0; i < cache->asset_count; i++) {
        if (cache->assets[i].source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
            free(cache->assets[i].audio_hilbert);
            free(cache->assets[i].audio_integral);
            cache->assets[i].audio_hilbert = NULL;
            cache->assets[i].audio_integral = NULL;
        }
    }

    snprintf(error, error_size, "ok");
    return true;
}

void asset_cache_free(asset_cache_t *cache)
{
    for (size_t i = 0; i < cache->asset_count; i++) {
        if (cache->assets[i].mmapped) {
            munmap(cache->assets[i].samples, cache->assets[i].map_bytes);
            cache->assets[i].mmapped = false;
            cache->assets[i].map_bytes = 0;
        } else {
            free(cache->assets[i].samples);
        }
        free(cache->assets[i].audio_samples);
        free(cache->assets[i].audio_hilbert);
        free(cache->assets[i].audio_integral);
        for (size_t k = 0; k < cache->assets[i].variant_count; k++) {
            cached_iq_buffer_t *v = &cache->assets[i].variants[k];
            if (v->mmapped) {
                munmap(v->samples, v->map_bytes);
            } else {
                free(v->samples);
            }
            v->samples = NULL;
        }
        cache->assets[i].variant_count = 0;
        cache->assets[i].samples = NULL;
        cache->assets[i].audio_samples = NULL;
        cache->assets[i].audio_hilbert = NULL;
        cache->assets[i].audio_integral = NULL;
        cache->assets[i].sample_count = 0;
    }
    cache->asset_count = 0;
    for (size_t i = 0; i < SIM_MAX_SIGNALS; i++) {
        free(cache->prerenders[i].samples);
        cache->prerenders[i].samples = NULL;
        cache->prerenders[i].valid = false;
        cache->prerenders[i].sample_count = 0;
    }
}

const cached_prerender_t *asset_cache_prerender(const asset_cache_t *cache, size_t signal_index)
{
    if (cache == NULL || signal_index >= SIM_MAX_SIGNALS || !cache->prerenders[signal_index].valid) {
        return NULL;
    }
    return &cache->prerenders[signal_index];
}

bool asset_cache_prerender_from_audio(cached_prerender_t *out, const float *audio, uint64_t audio_count, uint32_t audio_rate_hz, const scenario_signal_t *signal, const prerender_params_t *params, char *error, size_t error_size)
{
    memset(out, 0, sizeof(*out));
    const prerender_params_t default_params = {.oversample = 2.0, .max_rate_hz = 4000000U};
    if (params == NULL) {
        params = &default_params;
    }

    /* Build a throwaway asset that owns a copy of the audio plus the Hilbert/integral helpers, so
     * the exact same synthesis the load path uses is exercised on caller-supplied audio (no RMS
     * normalisation here -- the caller controls the sample values directly). */
    cached_asset_t asset;
    memset(&asset, 0, sizeof(asset));
    asset.source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    asset.sample_count = audio_count;
    asset.normalization_gain = 1.0;
    if (audio_count > 0) {
        asset.audio_samples = malloc((size_t)audio_count * sizeof(*asset.audio_samples));
        if (asset.audio_samples == NULL) {
            snprintf(error, error_size, "asset_cache_alloc_failed");
            return false;
        }
        memcpy(asset.audio_samples, audio, (size_t)audio_count * sizeof(*asset.audio_samples));
        if (!build_audio_helpers(&asset, error, error_size)) {
            free(asset.audio_samples);
            return false;
        }
    }

    scenario_source_t source;
    memset(&source, 0, sizeof(source));
    source.source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    source.sample_rate_hz = audio_rate_hz;
    source.sample_count = audio_count;

    const uint32_t rate = prerender_rate_hz(signal, &source, params);
    const uint64_t pr_count = audio_count > 0 ? (uint64_t)llround((double)audio_count * (double)rate / (double)audio_rate_hz) : 0;
    iq_ci16_t *buffer = NULL;
    if (pr_count > 0) {
        buffer = calloc((size_t)pr_count, sizeof(*buffer));
        if (buffer == NULL) {
            snprintf(error, error_size, "asset_cache_alloc_failed");
            free(asset.audio_samples);
            free(asset.audio_hilbert);
            free(asset.audio_integral);
            return false;
        }
    }
    out->gain = pr_count > 0 ? prerender_fill_buffer(&asset, signal, (double)audio_rate_hz, rate, pr_count, buffer) : 1.0;
    out->valid = true;
    out->sample_rate_hz = rate;
    out->sample_count = pr_count;
    out->samples = buffer;

    free(asset.audio_samples);
    free(asset.audio_hilbert);
    free(asset.audio_integral);
    snprintf(error, error_size, "ok");
    return true;
}

const cached_asset_t *asset_cache_find(const asset_cache_t *cache, const char *source_id)
{
    if (cache == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cache->asset_count; i++) {
        if (strcmp(cache->assets[i].source_id, source_id) == 0) {
            return &cache->assets[i];
        }
    }
    return NULL;
}
