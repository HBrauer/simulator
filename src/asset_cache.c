#include "asset_cache.h"
#include "iq_file_reader.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size)
{
    memset(cache, 0, sizeof(*cache));
    cache->asset_count = scenario->source_count;
    for (size_t i = 0; i < scenario->source_count; i++) {
        const scenario_source_t *source = &scenario->sources[i];
        cached_asset_t *asset = &cache->assets[i];
        sim_strlcpy(asset->source_id, source->id, sizeof(asset->source_id));
        asset->sample_count = source->sample_count;
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
        const bool ok = iq_file_reader_read(&reader, 0, asset->samples, (size_t)asset->sample_count, &read_count);
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
