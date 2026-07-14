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

/* Byte budget for the DDC intermediate sub-band cache (see ddc_cache.h). Configuring it
 * replaces the process-wide cache, so call it at startup before rendering begins; a budget
 * of 0 disables caching (DDC channels then use the direct full-rate cascade every block). */
#define RENDERER_DDC_CACHE_DEFAULT_BYTES (2ULL << 30)
void renderer_ddc_cache_configure(size_t max_bytes);

/* Background DDC intermediate builds (default off). Off: a block whose intermediate is
 * missing builds it synchronously -- strictly deterministic output, but the channel stalls
 * for the build on a cold retune. On: the block renders through the direct full-rate cascade
 * immediately while the intermediate builds on a background thread, switching to the cached
 * path on a block boundary once ready -- streaming never stalls, at the cost of a bounded
 * transition window in which output content depends on when the build finishes (the two
 * paths agree within the filter design tolerance, not bit-exactly). The streamer enables
 * this after startup prewarm; one-shot renders and tests keep the blocking default. */
void renderer_ddc_background_builds(bool enabled);

/* Render one block of a channel. The effective center follows the receiver tuner for
 * track_tuner channels; a channel outside the front-end window renders silence. */
bool renderer_render_channel_block(const scenario_t *scenario, const asset_cache_t *cache, const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats);

#endif
