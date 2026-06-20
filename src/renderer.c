#include "renderer.h"
#include "iq_file_reader.h"
#include "nco.h"
#include "receiver.h"
#include "scenario.h"
#include "timebase.h"

#include <math.h>
#include <string.h>

#define RESAMPLER_RADIUS 4

static double sinc_value(double x)
{
    if (fabs(x) < 1e-12) {
        return 1.0;
    }
    return sin(M_PI * x) / (M_PI * x);
}

static double hann_window(double distance)
{
    const double normalized = fabs(distance) / (double)RESAMPLER_RADIUS;
    if (normalized >= 1.0) {
        return 0.0;
    }
    return 0.5 + 0.5 * cos(M_PI * normalized);
}

static void resample_sinc_ci16(const iq_ci16_t *samples, size_t sample_count, double source_position, double *out_i, double *out_q)
{
    const int64_t center = (int64_t)floor(source_position);
    double acc_i = 0.0;
    double acc_q = 0.0;
    double weight_sum = 0.0;
    for (int tap = -RESAMPLER_RADIUS + 1; tap <= RESAMPLER_RADIUS; tap++) {
        const int64_t index = center + tap;
        if (index < 0 || (uint64_t)index >= sample_count) {
            continue;
        }
        const double distance = source_position - (double)index;
        const double weight = sinc_value(distance) * hann_window(distance);
        acc_i += (double)samples[index].i * weight;
        acc_q += (double)samples[index].q * weight;
        weight_sum += weight;
    }
    if (fabs(weight_sum) < 1e-12) {
        *out_i = 0.0;
        *out_q = 0.0;
        return;
    }
    *out_i = acc_i / weight_sum;
    *out_q = acc_q / weight_sum;
}

static double signal_passband_gain(uint64_t signal_center, uint32_t signal_bw, uint64_t window_center, uint64_t window_bw)
{
    const int64_t sig_lo = (int64_t)signal_center - (int64_t)(signal_bw / 2U);
    const int64_t sig_hi = (int64_t)signal_center + (int64_t)(signal_bw / 2U);
    const int64_t win_lo = (int64_t)window_center - (int64_t)(window_bw / 2ULL);
    const int64_t win_hi = (int64_t)window_center + (int64_t)(window_bw / 2ULL);
    if (signal_bw == 0U) {
        const int64_t signal = (int64_t)signal_center;
        return signal >= win_lo && signal <= win_hi ? 1.0 : 0.0;
    }
    const int64_t overlap_lo = sig_lo > win_lo ? sig_lo : win_lo;
    const int64_t overlap_hi = sig_hi < win_hi ? sig_hi : win_hi;
    if (overlap_hi <= overlap_lo) {
        return 0.0;
    }
    const double power_fraction = (double)(overlap_hi - overlap_lo) / (double)signal_bw;
    return sqrt(power_fraction);
}

static bool renderer_render_window_block(
    const scenario_t *scenario,
    const asset_cache_t *cache,
    uint64_t window_center_hz,
    uint64_t window_bandwidth_hz,
    uint32_t output_sample_rate_hz,
    double output_scale,
    double rf_reference_power_dbm,
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
        const double passband_gain = signal_passband_gain(signal->center_frequency_hz, signal->bandwidth_hz, window_center_hz, window_bandwidth_hz);
        uint64_t sample_offset = 0;
        if (source == NULL || asset == NULL || passband_gain <= 0.0 || !iq_signal_active(signal, source, day_s, &sample_offset)) {
            continue;
        }

        const double source_per_output = (double)source->sample_rate_hz / (double)output_sample_rate_hz;
        const uint64_t needed_source_samples = (uint64_t)ceil((double)(count > 0 ? count - 1 : 0) * source_per_output) + (uint64_t)RESAMPLER_RADIUS + 2ULL;
        if (needed_source_samples > 4096ULL) {
            continue;
        }

        const uint64_t available = sample_offset < asset->sample_count ? asset->sample_count - sample_offset : 0;
        const size_t read_count = available < needed_source_samples ? (size_t)available : (size_t)needed_source_samples;
        const iq_ci16_t *source_samples = &asset->samples[sample_offset];

        const double offset_hz = (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
        const double source_gain = passband_gain * output_scale * pow(10.0, (signal->power_dbm - rf_reference_power_dbm) / 20.0);
        const double phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
        const double step_c = cos(phase_step);
        const double step_s = sin(phase_step);
        double osc_c = 1.0;
        double osc_s = 0.0;
        for (size_t i = 0; i < count; i++) {
            const double source_position = (double)i * source_per_output;
            if ((size_t)floor(source_position) >= read_count) {
                break;
            }
            double resampled_i = 0.0;
            double resampled_q = 0.0;
            resample_sinc_ci16(source_samples, read_count, source_position, &resampled_i, &resampled_q);
            const double ii = source_gain * resampled_i;
            const double qq = source_gain * resampled_q;
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
    return renderer_render_window_block(scenario, cache, center_hz, SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, receiver->output_scale, receiver->rf_reference_power_dbm, scenario_time_ns, out, count, stats);
}

bool renderer_render_ddc_block(const scenario_t *scenario, const asset_cache_t *cache, const ddc_config_t *ddc, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    return renderer_render_window_block(scenario, cache, ddc->center_frequency_hz, ddc->bandwidth_hz, ddc->sample_rate_hz, ddc->output_scale, ddc->rf_reference_power_dbm, scenario_time_ns, out, count, stats);
}
