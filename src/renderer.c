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
        uint64_t sample_offset = 0;
        if (source == NULL || !signal_intersects(signal->center_frequency_hz, signal->bandwidth_hz, window_center_hz, window_bandwidth_hz) ||
            !iq_signal_active(signal, source, day_s, &sample_offset)) {
            continue;
        }

        const double source_per_output = (double)source->sample_rate_hz / (double)output_sample_rate_hz;
        const uint64_t needed_source_samples = (uint64_t)ceil((double)count * source_per_output) + 1ULL;
        if (needed_source_samples > 4096ULL) {
            continue;
        }

        iq_file_reader_t reader;
        char error[128];
        if (!iq_file_reader_open(&reader, source->file, source->sample_count, error, sizeof(error))) {
            continue;
        }
        iq_ci16_t scratch[4096];
        size_t read_count = 0;
        (void)iq_file_reader_read(&reader, sample_offset, scratch, (size_t)needed_source_samples, &read_count);
        iq_file_reader_close(&reader);

        const double offset_hz = (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
        double phase = 0.0;
        const double phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
        for (size_t i = 0; i < count; i++) {
            const size_t src_index = (size_t)floor((double)i * source_per_output);
            if (src_index >= read_count) {
                break;
            }
            const double c = cos(phase);
            const double sin_phase = sin(phase);
            const double ii = (double)scratch[src_index].i;
            const double qq = (double)scratch[src_index].q;
            out[i].i = sim_clip_i16((double)out[i].i + (ii * c - qq * sin_phase));
            out[i].q = sim_clip_i16((double)out[i].q + (ii * sin_phase + qq * c));
            phase += phase_step;
            if (phase > M_PI) {
                phase -= 2.0 * M_PI;
            } else if (phase < -M_PI) {
                phase += 2.0 * M_PI;
            }
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

bool renderer_render_80mhz_block(const scenario_t *scenario, const receiver_config_t *receiver, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    const uint64_t center_hz = receiver_center_frequency_hz(receiver, scenario_time_ns);
    return renderer_render_window_block(scenario, center_hz, SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, scenario_time_ns, out, count, stats);
}

bool renderer_render_ddc_block(const scenario_t *scenario, const ddc_config_t *ddc, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    return renderer_render_window_block(scenario, ddc->center_frequency_hz, ddc->bandwidth_hz, ddc->sample_rate_hz, scenario_time_ns, out, count, stats);
}
