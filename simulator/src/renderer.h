#ifndef RENDERER_H
#define RENDERER_H

#include "sim_types.h"
#include "asset_cache.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t active_signals;
    uint64_t samples_rendered;
} render_stats_t;

/* Render one block of a channel. The effective center follows the receiver tuner for
 * track_tuner channels; a channel outside the front-end window renders silence. */
bool renderer_render_channel_block(const scenario_t *scenario, const asset_cache_t *cache, const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats);

#endif
