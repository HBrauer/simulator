#include "asset_cache.h"
#include "iq_file_reader.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
        asset->sample_count = source->sample_count;
        if (asset->sample_count > SIZE_MAX / sizeof(*asset->samples)) {
            snprintf(error, error_size, "asset_cache_too_large");
            asset_cache_free(cache);
            return false;
        }
        const size_t asset_bytes = (size_t)asset->sample_count * sizeof(*asset->samples);
        if (max_bytes > 0 && (asset_bytes > max_bytes || total_bytes > max_bytes - asset_bytes)) {
            snprintf(error, error_size, "asset_cache_limit_exceeded");
            asset_cache_free(cache);
            return false;
        }
        total_bytes += asset_bytes;
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
    snprintf(error, error_size, "ok");
    return true;
}

void asset_cache_free(asset_cache_t *cache)
{
    for (size_t i = 0; i < cache->asset_count; i++) {
        free(cache->assets[i].samples);
        cache->assets[i].samples = NULL;
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
