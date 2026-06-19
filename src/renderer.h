#ifndef RENDERER_H
#define RENDERER_H

#include "sim_types.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t active_signals;
    uint64_t samples_rendered;
} render_stats_t;

bool renderer_render_80mhz_block(const scenario_t *scenario, const receiver_config_t *receiver, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats);
bool renderer_render_ddc_block(const scenario_t *scenario, const ddc_config_t *ddc, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats);

#endif
