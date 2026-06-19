#include "renderer.h"
#include "iq_file_reader.h"
#include "nco.h"
#include "receiver.h"
#include "scenario.h"
#include "timebase.h"

#include <math.h>
#include <string.h>

static bool signal_intersects(uint64_t signal_center, uint32_t signal_bw, uint64_t window_center, uint64_t window_bw)
{
    const int64_t sig_lo = (int64_t)signal_center - (int64_t)(signal_bw / 2U);
    const int64_t sig_hi = (int64_t)signal_center + (int64_t)(signal_bw / 2U);
    const int64_t win_lo = (int64_t)window_center - (int64_t)(window_bw / 2ULL);
    const int64_t win_hi = (int64_t)window_center + (int64_t)(window_bw / 2ULL);
    return sig_hi >= win_lo && sig_lo <= win_hi;
}

static bool renderer_render_window_block(
    const scenario_t *scenario,
    const asset_cache_t *cache,
    uint64_t window_center_hz,
    uint64_t window_bandwidth_hz,
    uint32_t output_sample_rate_hz,
    uint64_t scenario_time_ns,
    iq_ci16_t *out,
    size_t count,
    render_stats_t *stats)
{
    memset(out, 0, count * sizeof(*out));
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    const double day_s = timebase_day_seconds_from_ns(scenario_time_ns);

    for (size_t s = 0; s < scenario->signal_count; s++) {
        const scenario_signal_t *signal = &scenario->signals[s];
        const scenario_source_t *source = scenario_find_source(scenario, signal->source_reference);
        const cached_asset_t *asset = asset_cache_find(cache, signal->source_reference);
        uint64_t sample_offset = 0;
        if (source == NULL || asset == NULL || !signal_intersects(signal->center_frequency_hz, signal->bandwidth_hz, window_center_hz, window_bandwidth_hz) ||
            !iq_signal_active(signal, source, day_s, &sample_offset)) {
            continue;
        }

        const double source_per_output = (double)source->sample_rate_hz / (double)output_sample_rate_hz;
        const uint64_t needed_source_samples = (uint64_t)ceil((double)count * source_per_output) + 1ULL;
        if (needed_source_samples > 4096ULL) {
            continue;
        }

        const uint64_t available = sample_offset < asset->sample_count ? asset->sample_count - sample_offset : 0;
        const size_t read_count = available < needed_source_samples ? (size_t)available : (size_t)needed_source_samples;
        const iq_ci16_t *source_samples = &asset->samples[sample_offset];

        const double offset_hz = (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
        const double phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
        const double step_c = cos(phase_step);
        const double step_s = sin(phase_step);
        double osc_c = 1.0;
        double osc_s = 0.0;
        for (size_t i = 0; i < count; i++) {
            const size_t src_index = (size_t)floor((double)i * source_per_output);
            if (src_index >= read_count) {
                break;
            }
            const double ii = (double)source_samples[src_index].i;
            const double qq = (double)source_samples[src_index].q;
            out[i].i = sim_clip_i16((double)out[i].i + (ii * osc_c - qq * osc_s));
            out[i].q = sim_clip_i16((double)out[i].q + (ii * osc_s + qq * osc_c));
            const double next_c = osc_c * step_c - osc_s * step_s;
            const double next_s = osc_s * step_c + osc_c * step_s;
            osc_c = next_c;
            osc_s = next_s;
        }
        if (stats != NULL) {
            stats->active_signals++;
        }
    }
    if (stats != NULL) {
        stats->samples_rendered = count;
    }
    return true;
}

bool renderer_render_80mhz_block(const scenario_t *scenario, const asset_cache_t *cache, const receiver_config_t *receiver, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    const uint64_t center_hz = receiver_center_frequency_hz(receiver, scenario_time_ns);
    return renderer_render_window_block(scenario, cache, center_hz, SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, scenario_time_ns, out, count, stats);
}

bool renderer_render_ddc_block(const scenario_t *scenario, const asset_cache_t *cache, const ddc_config_t *ddc, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    return renderer_render_window_block(scenario, cache, ddc->center_frequency_hz, ddc->bandwidth_hz, ddc->sample_rate_hz, scenario_time_ns, out, count, stats);
}
