#include "asset_cache.h"
#include "iq_file_reader.h"
#include "util.h"
#include "wav_reader.h"

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HILBERT_RADIUS 31

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

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size)
{
    return asset_cache_load_limited(cache, scenario, 0, 4096, error, error_size);
}

bool asset_cache_load_limited(asset_cache_t *cache, const scenario_t *scenario, size_t max_bytes, size_t batch_samples, char *error, size_t error_size)
{
    memset(cache, 0, sizeof(*cache));
    if (batch_samples == 0) {
        batch_samples = 4096;
    }
    cache->asset_count = scenario->source_count;
    size_t total_bytes = 0;
    for (size_t i = 0; i < scenario->source_count; i++) {
        const scenario_source_t *source = &scenario->sources[i];
        cached_asset_t *asset = &cache->assets[i];
        sim_strlcpy(asset->source_id, source->id, sizeof(asset->source_id));
        asset->source_kind = source->source_kind;
        asset->sample_count = source->sample_count;
        size_t asset_bytes = 0;
        if (asset->source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
            if (asset->sample_count > (SIZE_MAX - sizeof(*asset->audio_integral)) /
                                          (sizeof(*asset->audio_samples) + sizeof(*asset->audio_hilbert) +
                                           sizeof(*asset->audio_integral))) {
                snprintf(error, error_size, "asset_cache_too_large");
                asset_cache_free(cache);
                return false;
            }
            asset_bytes = (size_t)asset->sample_count *
                              (sizeof(*asset->audio_samples) + sizeof(*asset->audio_hilbert) +
                               sizeof(*asset->audio_integral)) +
                          sizeof(*asset->audio_integral);
        } else if (asset->sample_count > SIZE_MAX / sizeof(*asset->samples)) {
            snprintf(error, error_size, "asset_cache_too_large");
            asset_cache_free(cache);
            return false;
        } else {
            asset_bytes = (size_t)asset->sample_count * sizeof(*asset->samples);
        }
        if (max_bytes > 0 && (asset_bytes > max_bytes || total_bytes > max_bytes - asset_bytes)) {
            snprintf(error, error_size, "asset_cache_limit_exceeded");
            asset_cache_free(cache);
            return false;
        }
        total_bytes += asset_bytes;

        if (asset->source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
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
            if (!build_audio_helpers(asset, error, error_size)) {
                asset_cache_free(cache);
                return false;
            }
        } else {
            asset->samples = calloc((size_t)asset->sample_count, sizeof(*asset->samples));
            if (asset->samples == NULL) {
                snprintf(error, error_size, "asset_cache_alloc_failed");
                asset_cache_free(cache);
                return false;
            }

            iq_file_reader_t reader;
            if (!iq_file_reader_open(&reader, source->file, source->sample_count, error, error_size)) {
                asset_cache_free(cache);
                return false;
            }
            size_t read_count = 0;
            bool ok = true;
            while (read_count < asset->sample_count) {
                const size_t remaining = (size_t)asset->sample_count - read_count;
                const size_t wanted = remaining < batch_samples ? remaining : batch_samples;
                size_t batch_read = 0;
                if (!iq_file_reader_read(&reader, read_count, asset->samples + read_count, wanted, &batch_read) || batch_read != wanted) {
                    ok = false;
                    break;
                }
                read_count += batch_read;
            }
            iq_file_reader_close(&reader);
            if (!ok || read_count != asset->sample_count) {
                snprintf(error, error_size, "asset_cache_read_failed");
                asset_cache_free(cache);
                return false;
            }
        }
    }
    snprintf(error, error_size, "ok");
    return true;
}

void asset_cache_free(asset_cache_t *cache)
{
    for (size_t i = 0; i < cache->asset_count; i++) {
        free(cache->assets[i].samples);
        free(cache->assets[i].audio_samples);
        free(cache->assets[i].audio_hilbert);
        free(cache->assets[i].audio_integral);
        cache->assets[i].samples = NULL;
        cache->assets[i].audio_samples = NULL;
        cache->assets[i].audio_hilbert = NULL;
        cache->assets[i].audio_integral = NULL;
        cache->assets[i].sample_count = 0;
    }
    cache->asset_count = 0;
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
