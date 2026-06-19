#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include "sim_types.h"

#include <stdbool.h>

typedef struct {
    char source_id[SIM_MAX_ID];
    uint64_t sample_count;
    iq_ci16_t *samples;
} cached_asset_t;

typedef struct {
    size_t asset_count;
    cached_asset_t assets[SIM_MAX_SOURCES];
} asset_cache_t;

bool asset_cache_load(asset_cache_t *cache, const scenario_t *scenario, char *error, size_t error_size);
void asset_cache_free(asset_cache_t *cache);
const cached_asset_t *asset_cache_find(const asset_cache_t *cache, const char *source_id);

#endif
