#include "renderer.h"
#include "iq_file_reader.h"
#include "nco.h"
#include "receiver.h"
#include "scenario.h"
#include "sim_config.h"
#include "timebase.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if SIM_USE_LIQUID_RESAMPLER
#include <liquid/liquid.h>
#endif
#if SIM_HAVE_VOLK
#include <volk/volk.h>
#endif

#define RESAMPLER_RADIUS 4
#define RESAMPLER_TAPS 8U
#define LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT 0.125

static void resample_ci16(const iq_ci16_t *samples, size_t sample_count, double source_position, double *out_i, double *out_q);
static void mix_accumulate_sample(float *bus, size_t index, double sample_i, double sample_q, double gain, double osc_c, double osc_s);

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

static void build_quarter_phase_weights(double weights[4][RESAMPLER_TAPS])
{
    for (size_t phase = 0; phase < 4; phase++) {
        const double fraction = (double)phase * 0.25;
        double weight_sum = 0.0;
        for (size_t tap_index = 0; tap_index < RESAMPLER_TAPS; tap_index++) {
            const int tap = (int)tap_index - RESAMPLER_RADIUS + 1;
            const double distance = fraction - (double)tap;
            const double weight = sinc_value(distance) * hann_window(distance);
            weights[phase][tap_index] = weight;
            weight_sum += weight;
        }
        if (fabs(weight_sum) >= 1e-12) {
            for (size_t tap_index = 0; tap_index < RESAMPLER_TAPS; tap_index++) {
                weights[phase][tap_index] /= weight_sum;
            }
        }
    }
}

static void build_quarter_phase_weights_f(float weights[4][RESAMPLER_TAPS])
{
    double double_weights[4][RESAMPLER_TAPS];
    build_quarter_phase_weights(double_weights);
    for (size_t phase = 0; phase < 4; phase++) {
        for (size_t tap_index = 0; tap_index < RESAMPLER_TAPS; tap_index++) {
            weights[phase][tap_index] = (float)double_weights[phase][tap_index];
        }
    }
}

static int16_t clip_i16_f(float value)
{
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)lrintf(value);
}

static void resample_quarter_ci16(const iq_ci16_t *samples, size_t sample_count, size_t output_index, double weights[4][RESAMPLER_TAPS], double *out_i, double *out_q)
{
    const size_t center = output_index / 4U;
    const size_t phase = output_index & 3U;
    if (center < RESAMPLER_RADIUS - 1 || center + RESAMPLER_RADIUS >= sample_count) {
        resample_ci16(samples, sample_count, (double)output_index * 0.25, out_i, out_q);
        return;
    }

    double acc_i = 0.0;
    double acc_q = 0.0;
    for (size_t tap_index = 0; tap_index < RESAMPLER_TAPS; tap_index++) {
        const size_t index = center + tap_index - RESAMPLER_RADIUS + 1U;
        const double weight = weights[phase][tap_index];
        acc_i += (double)samples[index].i * weight;
        acc_q += (double)samples[index].q * weight;
    }
    *out_i = acc_i;
    *out_q = acc_q;
}

static bool resample_quarter_ci16_f(const iq_ci16_t *samples, size_t sample_count, size_t output_index, float weights[4][RESAMPLER_TAPS], float *out_i, float *out_q)
{
    const size_t center = output_index / 4U;
    const size_t phase = output_index & 3U;
    if (center < RESAMPLER_RADIUS - 1 || center + RESAMPLER_RADIUS >= sample_count) {
        return false;
    }

    float acc_i = 0.0f;
    float acc_q = 0.0f;
    for (size_t tap_index = 0; tap_index < RESAMPLER_TAPS; tap_index++) {
        const size_t index = center + tap_index - RESAMPLER_RADIUS + 1U;
        const float weight = weights[phase][tap_index];
        acc_i += (float)samples[index].i * weight;
        acc_q += (float)samples[index].q * weight;
    }
    *out_i = acc_i;
    *out_q = acc_q;
    return true;
}

#if !SIM_USE_LIQUID_RESAMPLER
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
#endif

#if SIM_USE_LIQUID_RESAMPLER
static void resample_liquid_ci16(const iq_ci16_t *samples, size_t sample_count, double source_position, double *out_i, double *out_q)
{
    const int64_t center = (int64_t)floor(source_position);
    float weights[2 * RESAMPLER_RADIUS];
    liquid_float_complex input[2 * RESAMPLER_RADIUS];
    unsigned int tap_count = 0;
    double weight_sum = 0.0;
    for (int tap = -RESAMPLER_RADIUS + 1; tap <= RESAMPLER_RADIUS; tap++) {
        const int64_t index = center + tap;
        if (index < 0 || (uint64_t)index >= sample_count) {
            continue;
        }
        const double distance = source_position - (double)index;
        const double weight = sinc_value(distance) * hann_window(distance);
        weights[tap_count] = (float)weight;
        input[tap_count] = (float)samples[index].i + (float)samples[index].q * I;
        weight_sum += weight;
        tap_count++;
    }
    if (tap_count == 0 || fabs(weight_sum) < 1e-12) {
        *out_i = 0.0;
        *out_q = 0.0;
        return;
    }
    for (unsigned int i = 0; i < tap_count; i++) {
        weights[i] = (float)((double)weights[i] / weight_sum);
    }
    liquid_float_complex y = 0.0f;
    dotprod_crcf_run4(weights, input, tap_count, &y);
    *out_i = (double)crealf(y);
    *out_q = (double)cimagf(y);
}
#endif

static void resample_ci16(const iq_ci16_t *samples, size_t sample_count, double source_position, double *out_i, double *out_q)
{
#if SIM_USE_LIQUID_RESAMPLER
    resample_liquid_ci16(samples, sample_count, source_position, out_i, out_q);
#else
    resample_sinc_ci16(samples, sample_count, source_position, out_i, out_q);
#endif
}

static void resample_linear_ci16_f(const iq_ci16_t *samples, size_t sample_count, float source_position, float *out_i, float *out_q)
{
    if (sample_count == 0) {
        *out_i = 0.0f;
        *out_q = 0.0f;
        return;
    }

    size_t index = (size_t)source_position;
    if (index + 1U >= sample_count) {
        if (index >= sample_count) {
            index = sample_count - 1U;
        }
        *out_i = (float)samples[index].i;
        *out_q = (float)samples[index].q;
        return;
    }

    const float fraction = source_position - (float)index;
    const float i0 = (float)samples[index].i;
    const float q0 = (float)samples[index].q;
    *out_i = i0 + ((float)samples[index + 1U].i - i0) * fraction;
    *out_q = q0 + ((float)samples[index + 1U].q - q0) * fraction;
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

static double audio_linear_at(const float *samples, uint64_t sample_count, double position)
{
    if (samples == NULL || sample_count == 0 || position < 0.0) {
        return 0.0;
    }
    uint64_t index = (uint64_t)position;
    if (index + 1ULL >= sample_count) {
        return index < sample_count ? (double)samples[index] : 0.0;
    }
    const double fraction = position - (double)index;
    const double a = (double)samples[index];
    const double b = (double)samples[index + 1ULL];
    return a + (b - a) * fraction;
}

static double audio_integral_at(const cached_asset_t *asset, double position)
{
    if (asset == NULL || asset->audio_integral == NULL || asset->sample_count == 0 || position <= 0.0) {
        return 0.0;
    }
    uint64_t index = (uint64_t)position;
    if (index >= asset->sample_count) {
        return asset->audio_integral[asset->sample_count];
    }
    const double fraction = position - (double)index;
    return asset->audio_integral[index] + fraction * (double)asset->audio_samples[index];
}

static void render_audio_modulated(
    const scenario_signal_t *signal,
    const scenario_source_t *source,
    const cached_asset_t *asset,
    uint64_t sample_offset,
    double offset_fraction,
    double offset_hz,
    uint32_t output_sample_rate_hz,
    uint64_t start_sample,
    double source_gain,
    float *bus,
    size_t count)
{
    const double source_per_output = (double)source->sample_rate_hz / (double)output_sample_rate_hz;
    const double shift_phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
    const double shift_step_c = cos(shift_phase_step);
    const double shift_step_s = sin(shift_phase_step);
    /* Continuous starting phase from the absolute output-sample index (see nco.h). */
    const uint64_t shift_step_q64 = nco_phase_step_q64(offset_hz, (double)output_sample_rate_hz);
    const double shift_phase0 = nco_phase_rad_at(shift_step_q64, start_sample);
    double shift_c = cos(shift_phase0);
    double shift_s = sin(shift_phase0);
    const double amplitude = 32767.0 * source_gain;

    for (size_t i = 0; i < count; i++) {
        const double audio_position = (double)sample_offset + offset_fraction + (double)i * source_per_output;
        double base_i = 0.0;
        double base_q = 0.0;

        if (signal->modulation == SCENARIO_MODULATION_WBFM) {
            const double integral = audio_integral_at(asset, audio_position);
            const double fm_phase = 2.0 * M_PI * signal->fm_deviation_hz * integral / (double)source->sample_rate_hz;
            base_i = cos(fm_phase);
            base_q = sin(fm_phase);
        } else if (signal->modulation == SCENARIO_MODULATION_AM) {
            const double audio = audio_linear_at(asset->audio_samples, asset->sample_count, audio_position);
            const double envelope = 1.0 + signal->am_depth * audio;
            base_i = envelope < 0.0 ? 0.0 : envelope;
        } else {
            const double audio = audio_linear_at(asset->audio_samples, asset->sample_count, audio_position);
            const double hilbert = audio_linear_at(asset->audio_hilbert, asset->sample_count, audio_position);
            base_i = audio;
            base_q = signal->modulation == SCENARIO_MODULATION_USB ? hilbert : -hilbert;
        }

        mix_accumulate_sample(bus, i, amplitude * base_i, amplitude * base_q, 1.0, shift_c, shift_s);
        const double next_c = shift_c * shift_step_c - shift_s * shift_step_s;
        const double next_s = shift_s * shift_step_c + shift_c * shift_step_s;
        shift_c = next_c;
        shift_s = next_s;
        if ((i & 0xffU) == 0xffU) {
            const double inv = 1.0 / sqrt(shift_c * shift_c + shift_s * shift_s);
            shift_c *= inv;
            shift_s *= inv;
        }
    }
}

static uint64_t sample_index_from_time_ns(uint64_t scenario_time_ns, uint32_t sample_rate_hz)
{
    return (uint64_t)(((__uint128_t)scenario_time_ns * (uint64_t)sample_rate_hz) / 1000000000ULL);
}

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

static uint32_t xorshift32_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

static int16_t clip_i32_to_i16(int32_t value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return (int16_t)value;
}

static int16_t scale_noise_i16(int32_t raw, int32_t amplitude)
{
    int64_t scaled = (int64_t)raw * (int64_t)amplitude;
    scaled += scaled >= 0 ? 16384 : -16384;
    return clip_i32_to_i16((int32_t)(scaled / 32768));
}

static void render_noise_floor(
    const scenario_t *scenario,
    uint64_t window_center_hz,
    uint64_t window_bandwidth_hz,
    uint32_t output_sample_rate_hz,
    double output_scale,
    double rf_reference_power_dbm,
    uint64_t scenario_time_ns,
    float *bus,
    size_t count)
{
    if (!scenario->noise_floor.enabled) {
        return;
    }

    const double amplitude_dbfs =
        32767.0 * output_scale * pow(10.0, (scenario->noise_floor.power_dbm - rf_reference_power_dbm) / 20.0);
    if (amplitude_dbfs <= 0.0) {
        return;
    }
    int32_t amplitude = (int32_t)lrint(amplitude_dbfs);
    if (amplitude <= 0) {
        return;
    }
    if (amplitude > 32767) {
        amplitude = 32767;
    }

    const uint64_t sample_index = sample_index_from_time_ns(scenario_time_ns, output_sample_rate_hz);
    const uint64_t seed = splitmix64(scenario->noise_floor.seed) ^ splitmix64(sample_index) ^
                          splitmix64(window_center_hz) ^ splitmix64(window_bandwidth_hz);
    uint32_t state = (uint32_t)(seed ^ (seed >> 32U));
    if (state == 0U) {
        state = 0x6d2b79f5U;
    }

    /* Noise is written first into a zeroed bus, so a plain add is an assignment. */
    for (size_t i = 0; i < count; i++) {
        const int32_t raw_i = (int32_t)(xorshift32_next(&state) & 0xffffU) - 32768;
        const int32_t raw_q = (int32_t)(xorshift32_next(&state) & 0xffffU) - 32768;
        bus[2U * i] += (float)scale_noise_i16(raw_i, amplitude);
        bus[2U * i + 1U] += (float)scale_noise_i16(raw_q, amplitude);
    }
}

static void mix_accumulate_sample(float *bus, size_t index, double sample_i, double sample_q, double gain, double osc_c, double osc_s)
{
    const double ii = gain * sample_i;
    const double qq = gain * sample_q;
    bus[2U * index] += (float)(ii * osc_c - qq * osc_s);
    bus[2U * index + 1U] += (float)(ii * osc_s + qq * osc_c);
}

static void accumulate_direct_baseband(float *bus, const iq_ci16_t *source_samples, size_t index, double source_gain)
{
    bus[2U * index] += (float)(source_gain * (double)source_samples[index].i);
    bus[2U * index + 1U] += (float)(source_gain * (double)source_samples[index].q);
}

static void render_direct_baseband(const iq_ci16_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain)
{
    const size_t limit = read_count < count ? read_count : count;
    size_t i = 0;
    for (; i + 3 < limit; i += 4) {
        accumulate_direct_baseband(bus, source_samples, i, source_gain);
        accumulate_direct_baseband(bus, source_samples, i + 1, source_gain);
        accumulate_direct_baseband(bus, source_samples, i + 2, source_gain);
        accumulate_direct_baseband(bus, source_samples, i + 3, source_gain);
    }
    for (; i < limit; i++) {
        accumulate_direct_baseband(bus, source_samples, i, source_gain);
    }
}

static void render_direct_nco(const iq_ci16_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double init_c, double init_s, double step_c, double step_s)
{
    const size_t limit = read_count < count ? read_count : count;
#if SIM_HAVE_VOLK
    if (limit >= 16) {
        const size_t alignment = volk_get_alignment();
        lv_32fc_t *input = volk_malloc(limit * sizeof(*input), alignment);
        lv_32fc_t *rotated = volk_malloc(limit * sizeof(*rotated), alignment);
        if (input != NULL && rotated != NULL) {
            for (size_t i = 0; i < limit; i++) {
                input[i] = (float)source_samples[i].i + (float)source_samples[i].q * I;
            }
            lv_32fc_t phase = (float)init_c + (float)init_s * I;
            const lv_32fc_t phase_inc = (float)step_c + (float)step_s * I;
            volk_32fc_s32fc_x2_rotator2_32fc(rotated, input, &phase_inc, &phase, (unsigned int)limit);
            const float gain_f = (float)source_gain;
            for (size_t i = 0; i < limit; i++) {
                bus[2U * i] += gain_f * crealf(rotated[i]);
                bus[2U * i + 1U] += gain_f * cimagf(rotated[i]);
            }
            volk_free(rotated);
            volk_free(input);
            return;
        }
        volk_free(rotated);
        volk_free(input);
    }
#endif
    double osc_c = init_c;
    double osc_s = init_s;
    for (size_t i = 0; i < limit; i++) {
        mix_accumulate_sample(bus, i, (double)source_samples[i].i, (double)source_samples[i].q, source_gain, osc_c, osc_s);
        const double next_c = osc_c * step_c - osc_s * step_s;
        const double next_s = osc_s * step_c + osc_c * step_s;
        osc_c = next_c;
        osc_s = next_s;
        if ((i & 0xffU) == 0xffU) {
            const double inv = 1.0 / sqrt(osc_c * osc_c + osc_s * osc_s);
            osc_c *= inv;
            osc_s *= inv;
        }
    }
}

static void render_resampled_baseband(const iq_ci16_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double source_per_output, double offset_fraction)
{
    /* The quarter-rate fast path indexes by output sample, which assumes positions land exactly
     * on i*0.25 -- only valid when the block starts on an integer source sample. */
    const bool quarter_rate = fabs(source_per_output - 0.25) < 1e-12 && offset_fraction < 1e-9;
    const bool low_rate_linear = source_per_output > 0.0 && source_per_output <= LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT;
    if (low_rate_linear) {
        const float gain_f = (float)source_gain;
        const float source_step = (float)source_per_output;
        float source_position = (float)offset_fraction;
        for (size_t i = 0; i < count; i++) {
            if ((size_t)source_position >= read_count) {
                break;
            }
            float resampled_i = 0.0f;
            float resampled_q = 0.0f;
            resample_linear_ci16_f(source_samples, read_count, source_position, &resampled_i, &resampled_q);
            bus[2U * i] += gain_f * resampled_i;
            bus[2U * i + 1U] += gain_f * resampled_q;
            source_position += source_step;
        }
        return;
    }
    double quarter_weights[4][RESAMPLER_TAPS];
    if (quarter_rate) {
        build_quarter_phase_weights(quarter_weights);
    }
    for (size_t i = 0; i < count; i++) {
        const double source_position = offset_fraction + (double)i * source_per_output;
        if ((size_t)floor(source_position) >= read_count) {
            break;
        }
        double resampled_i = 0.0;
        double resampled_q = 0.0;
        if (quarter_rate) {
            resample_quarter_ci16(source_samples, read_count, i, quarter_weights, &resampled_i, &resampled_q);
        } else {
            resample_ci16(source_samples, read_count, source_position, &resampled_i, &resampled_q);
        }
        bus[2U * i] += (float)(source_gain * resampled_i);
        bus[2U * i + 1U] += (float)(source_gain * resampled_q);
    }
}

static void render_resampled_nco(const iq_ci16_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double source_per_output, double offset_fraction, double init_c, double init_s, double step_c, double step_s)
{
    /* The quarter-rate fast path indexes by output sample, valid only when the block starts on
     * an integer source sample (see render_resampled_baseband). */
    const bool quarter_rate = fabs(source_per_output - 0.25) < 1e-12 && offset_fraction < 1e-9;
    const bool low_rate_linear = source_per_output > 0.0 && source_per_output <= LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT;
    if (low_rate_linear) {
        const float gain_f = (float)source_gain;
        const float source_step = (float)source_per_output;
        const float step_c_f = (float)step_c;
        const float step_s_f = (float)step_s;
        float source_position = (float)offset_fraction;
        float osc_c = (float)init_c;
        float osc_s = (float)init_s;
        for (size_t i = 0; i < count; i++) {
            if ((size_t)source_position >= read_count) {
                break;
            }
            float resampled_i = 0.0f;
            float resampled_q = 0.0f;
            resample_linear_ci16_f(source_samples, read_count, source_position, &resampled_i, &resampled_q);
            const float ii = gain_f * resampled_i;
            const float qq = gain_f * resampled_q;
            bus[2U * i] += ii * osc_c - qq * osc_s;
            bus[2U * i + 1U] += ii * osc_s + qq * osc_c;
            const float next_c = osc_c * step_c_f - osc_s * step_s_f;
            const float next_s = osc_s * step_c_f + osc_c * step_s_f;
            osc_c = next_c;
            osc_s = next_s;
            if ((i & 0xffU) == 0xffU) {
                const float inv = 1.0f / sqrtf(osc_c * osc_c + osc_s * osc_s);
                osc_c *= inv;
                osc_s *= inv;
            }
            source_position += source_step;
        }
        return;
    }
    if (quarter_rate && count >= 64) {
        float quarter_weights_f[4][RESAMPLER_TAPS];
        build_quarter_phase_weights_f(quarter_weights_f);
        const float gain_f = (float)source_gain;
        const float step_c_f = (float)step_c;
        const float step_s_f = (float)step_s;
        float osc_c = (float)init_c;
        float osc_s = (float)init_s;
        for (size_t i = 0; i < count; i++) {
            const size_t center = i / 4U;
            if (center >= read_count) {
                break;
            }
            float resampled_i = 0.0f;
            float resampled_q = 0.0f;
            if (!resample_quarter_ci16_f(source_samples, read_count, i, quarter_weights_f, &resampled_i, &resampled_q)) {
                double fallback_i = 0.0;
                double fallback_q = 0.0;
                resample_ci16(source_samples, read_count, (double)i * 0.25, &fallback_i, &fallback_q);
                resampled_i = (float)fallback_i;
                resampled_q = (float)fallback_q;
            }
            const float ii = gain_f * resampled_i;
            const float qq = gain_f * resampled_q;
            bus[2U * i] += ii * osc_c - qq * osc_s;
            bus[2U * i + 1U] += ii * osc_s + qq * osc_c;
            const float next_c = osc_c * step_c_f - osc_s * step_s_f;
            const float next_s = osc_s * step_c_f + osc_c * step_s_f;
            osc_c = next_c;
            osc_s = next_s;
            if ((i & 0xffU) == 0xffU) {
                const float inv = 1.0f / sqrtf(osc_c * osc_c + osc_s * osc_s);
                osc_c *= inv;
                osc_s *= inv;
            }
        }
        return;
    }
    double quarter_weights[4][RESAMPLER_TAPS];
    if (quarter_rate) {
        build_quarter_phase_weights(quarter_weights);
    }
    double osc_c = init_c;
    double osc_s = init_s;
    for (size_t i = 0; i < count; i++) {
        const double source_position = offset_fraction + (double)i * source_per_output;
        if ((size_t)floor(source_position) >= read_count) {
            break;
        }
        double resampled_i = 0.0;
        double resampled_q = 0.0;
        if (quarter_rate) {
            resample_quarter_ci16(source_samples, read_count, i, quarter_weights, &resampled_i, &resampled_q);
        } else {
            resample_ci16(source_samples, read_count, source_position, &resampled_i, &resampled_q);
        }
        mix_accumulate_sample(bus, i, resampled_i, resampled_q, source_gain, osc_c, osc_s);
        const double next_c = osc_c * step_c - osc_s * step_s;
        const double next_s = osc_s * step_c + osc_c * step_s;
        osc_c = next_c;
        osc_s = next_s;
        if ((i & 0xffU) == 0xffU) {
            const double inv = 1.0 / sqrt(osc_c * osc_c + osc_s * osc_s);
            osc_c *= inv;
            osc_s *= inv;
        }
    }
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
    if (count == 0) {
        return true;
    }

    /* All signals and the noise floor accumulate into a wide float mix bus (interleaved I/Q)
     * and are clipped to ci16 exactly once at the end. Accumulating in float keeps far more
     * headroom than the ci16 output and makes the result independent of the order signals are
     * mixed (intermediate sums may exceed +/-32767; only the final conversion saturates).
     * The bus lives on the stack for the capped streaming block size; only an oversized
     * one-shot render (CLI) falls back to a single heap allocation, so the hot path never
     * allocates. */
    float bus_stack[2U * SIM_MAX_STREAM_BLOCK_SAMPLES];
    float *bus = bus_stack;
    float *bus_heap = NULL;
    if (count > SIM_MAX_STREAM_BLOCK_SAMPLES) {
        bus_heap = malloc(2U * count * sizeof(*bus_heap));
        if (bus_heap == NULL) {
            return false;
        }
        bus = bus_heap;
    }
    memset(bus, 0, 2U * count * sizeof(*bus));

    render_noise_floor(scenario,
                       window_center_hz,
                       window_bandwidth_hz,
                       output_sample_rate_hz,
                       output_scale,
                       rf_reference_power_dbm,
                       scenario_time_ns,
                       bus,
                       count);
    const double day_s = timebase_day_seconds_from_ns(scenario_time_ns);
    /* Absolute output-sample index of the first sample in this block. The frequency-shift
     * phase of every signal is derived from it so the mixer stays phase-continuous across
     * block boundaries and identical across instances (scenario_time_ns is a grid time).
     * Rounding (not floor) inverts streamer_block_start_ns() exactly for any output rate
     * below 500 MHz, so a grid timestamp maps back to its exact block sample index without a
     * 1-sample boundary glitch. */
    const uint64_t start_sample =
        (uint64_t)(((__uint128_t)scenario_time_ns * (uint64_t)output_sample_rate_hz + 500000000ULL) / 1000000000ULL);

    for (size_t s = 0; s < scenario->signal_count; s++) {
        const scenario_signal_t *signal = &scenario->signals[s];
        const scenario_source_t *source = scenario_find_source(scenario, signal->source_reference);
        const cached_asset_t *asset = asset_cache_find(cache, signal->source_reference);
        const double passband_gain = signal_passband_gain(signal->center_frequency_hz, signal->bandwidth_hz, window_center_hz, window_bandwidth_hz);
        uint64_t sample_offset = 0;
        double offset_fraction = 0.0;
        if (source == NULL || asset == NULL || passband_gain <= 0.0 || !iq_signal_active(signal, source, day_s, &sample_offset, &offset_fraction)) {
            continue;
        }

        const double offset_hz = (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
        const double source_gain = passband_gain * output_scale * pow(10.0, (signal->power_dbm - rf_reference_power_dbm) / 20.0);
        if (source->source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
            render_audio_modulated(signal,
                                   source,
                                   asset,
                                   sample_offset,
                                   offset_fraction,
                                   offset_hz,
                                   output_sample_rate_hz,
                                   start_sample,
                                   source_gain,
                                   bus,
                                   count);
            if (stats != NULL) {
                stats->active_signals++;
            }
            continue;
        }

        const double source_per_output = (double)source->sample_rate_hz / (double)output_sample_rate_hz;
        const uint64_t needed_source_samples = (uint64_t)ceil((double)(count > 0 ? count - 1 : 0) * source_per_output) + (uint64_t)RESAMPLER_RADIUS + 2ULL;
        const uint64_t available = sample_offset < asset->sample_count ? asset->sample_count - sample_offset : 0;
        const size_t read_count = available < needed_source_samples ? (size_t)available : (size_t)needed_source_samples;
        const iq_ci16_t *source_samples = &asset->samples[sample_offset];

        const double phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
        const double step_c = cos(phase_step);
        const double step_s = sin(phase_step);
        /* Continuous starting phase for this block, from the absolute output-sample index. */
        const uint64_t phase_step_q64 = nco_phase_step_q64(offset_hz, (double)output_sample_rate_hz);
        const double phase0 = nco_phase_rad_at(phase_step_q64, start_sample);
        const double init_c = cos(phase0);
        const double init_s = sin(phase0);
        /* A nonzero fractional playback position means the block does not start on an integer
         * source sample, so the direct (integer-aligned) paths can't represent it -- fall back
         * to the resampler, which starts at the exact fractional position. */
        const bool integer_aligned = offset_fraction < 1e-9;
        if (source->sample_rate_hz == output_sample_rate_hz && offset_hz == 0.0 && integer_aligned) {
            render_direct_baseband(source_samples, read_count, bus, count, source_gain);
        } else if (source->sample_rate_hz == output_sample_rate_hz && integer_aligned) {
            render_direct_nco(source_samples, read_count, bus, count, source_gain, init_c, init_s, step_c, step_s);
        } else if (offset_hz == 0.0) {
            render_resampled_baseband(source_samples, read_count, bus, count, source_gain, source_per_output, offset_fraction);
        } else {
            render_resampled_nco(source_samples, read_count, bus, count, source_gain, source_per_output, offset_fraction, init_c, init_s, step_c, step_s);
        }
        if (stats != NULL) {
            stats->active_signals++;
        }
    }

    /* Single saturating conversion of the whole block. */
    for (size_t i = 0; i < count; i++) {
        out[i].i = clip_i16_f(bus[2U * i]);
        out[i].q = clip_i16_f(bus[2U * i + 1U]);
    }
    free(bus_heap);

    if (stats != NULL) {
        stats->samples_rendered = count;
    }
    return true;
}

bool renderer_render_80mhz_block(const scenario_t *scenario, const asset_cache_t *cache, const receiver_config_t *receiver, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    const uint64_t center_hz = receiver_center_frequency_hz(receiver, scenario_time_ns);
    return renderer_render_window_block(scenario, cache, center_hz, receiver->bandwidth_hz, receiver->sample_rate_hz, receiver->output_scale, receiver->rf_reference_power_dbm, scenario_time_ns, out, count, stats);
}

bool renderer_render_ddc_block(const scenario_t *scenario, const asset_cache_t *cache, const ddc_config_t *ddc, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    return renderer_render_window_block(scenario, cache, ddc->center_frequency_hz, ddc->bandwidth_hz, ddc->sample_rate_hz, ddc->output_scale, ddc->rf_reference_power_dbm, scenario_time_ns, out, count, stats);
}
