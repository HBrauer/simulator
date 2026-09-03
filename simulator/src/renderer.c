/* Render model (do not reintroduce wall-clock coupling here):
 *
 * A block is rendered for a scenario time that is always a point on the fixed block grid
 * (see streamer_block_start_ns). From that grid time this file derives the absolute output
 * sample index of the block, and every time-varying quantity -- each signal's frequency-shift
 * phase, its source playback position (integer + fraction), and the noise slice -- is a pure
 * function of that index. Nothing here reads the wall clock. That is what keeps the output
 * phase-continuous across block boundaries and byte-identical across independent instances.
 *
 * All signals and the noise floor accumulate into a wide float mix bus and are saturated to
 * ci16 exactly once, at the end of the block, so the result is independent of signal order. */
#include "renderer.h"
#include "ddc.h"
#include "ddc_cache.h"
#include "iq_file_reader.h"
#include "nco.h"
#include "receiver.h"
#include "scenario.h"
#include "sim_config.h"
#include "timebase.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
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
/* Decimation needs a kernel that spans ~RESAMPLER_RADIUS lobes of the *cutoff-scaled* sinc,
 * i.e. the radius grows with the decimation ratio. The fixed 8-tap kernel had so little
 * stopband for ratios >= 2 that strong out-of-band signals folded back into the channel at
 * visible strength. Capped so the widest kernel stays cheap relative to its (low) output rate. */
#define RESAMPLER_MAX_RADIUS 128
#define RESAMPLER_MAX_TAPS (2U * RESAMPLER_MAX_RADIUS)
#define LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT 0.125
/* Linear interpolation upsampling only rejects the spectral images at multiples of the source
 * rate by its triangular-kernel sinc^2 response, whose first image edge sits at
 * source_rate - bandwidth/2. That is deep in the stopband only when the occupied bandwidth is a
 * small fraction of the source rate; a source oversampled >= 8x relative to its content keeps the
 * first image >~50 dB down (measured), which is below any usable channel SNR. Below that -- e.g. a
 * 15 kHz POCSAG capture in a 32 kHz file (~2x) -- linear leaves the images at only ~-22 dB, so
 * they smear across the channel as ghost carriers; those sources must take the polyphase path. */
#define LINEAR_MIN_SOURCE_OVERSAMPLE 8U

static void resample_ci16(const iq_src_t *samples, size_t sample_count, double source_position, double cutoff, double *out_i, double *out_q);
static void mix_accumulate_sample(float *bus, size_t index, double sample_i, double sample_q, double gain, double osc_c, double osc_s);

/* Anti-alias cutoff (in cycles per source sample) for a given rate ratio. When decimating
 * (source rate above output rate) the interpolation kernel must roll off at the *output*
 * Nyquist, i.e. 1/source_per_output, or everything above it folds back into the band. When
 * upsampling the source Nyquist already bounds the content, so the cutoff stays 1.0. */
static double resampler_cutoff(double source_per_output)
{
    return source_per_output > 1.0 ? 1.0 / source_per_output : 1.0;
}

/* Kernel radius in source samples for a given cutoff: RESAMPLER_RADIUS sinc lobes at the
 * cutoff-scaled lobe width, so decimating kernels keep the same stopband shape as the
 * upsampling one. */
static int resampler_radius_for_cutoff(double cutoff)
{
    if (cutoff >= 1.0) {
        return RESAMPLER_RADIUS;
    }
    /* Decimating kernels span twice the base lobe count: the extra lobes buy ~15 dB more
     * stopband where shifted-out-of-window content would otherwise fold back, and the cost
     * scales with the (low) output rate of the decimated stream. */
    const double radius = ceil((double)(2 * RESAMPLER_RADIUS) / cutoff);
    return radius > (double)RESAMPLER_MAX_RADIUS ? (int)RESAMPLER_MAX_RADIUS : (int)radius;
}

static double sinc_value(double x)
{
    if (fabs(x) < 1e-12) {
        return 1.0;
    }
    return sin(M_PI * x) / (M_PI * x);
}

static double hann_window_radius(double distance, int radius)
{
    const double normalized = fabs(distance) / (double)radius;
    if (normalized >= 1.0) {
        return 0.0;
    }
    return 0.5 + 0.5 * cos(M_PI * normalized);
}

/* Polyphase kernel table. For a given anti-alias cutoff we precompute RESAMPLER_PHASES phase
 * positions x RESAMPLER_TAPS weights from the same sinc(cutoff*d)*hann(d) formula the exact
 * resampler uses, normalised per phase for unity DC gain. Each output sample selects the nearest
 * phase (positional error <= 1/(2*RESAMPLER_PHASES) sample), so the hot path costs RESAMPLER_TAPS
 * MACs instead of ~2*RESAMPLER_TAPS transcendentals. The old hardcoded quarter-rate special case
 * is now just the cutoff==1.0 table with the ratio landing exactly on phases 0/16/32/48. */
#define RESAMPLER_PHASES 256U
#define RESAMPLER_TABLE_CACHE 32U

typedef struct {
    double cutoff;
    int radius; /* taps = 2 * radius */
    float w[RESAMPLER_PHASES][RESAMPLER_MAX_TAPS];
} resampler_table_t;

static void build_resampler_table(resampler_table_t *table, double cutoff)
{
    table->cutoff = cutoff;
    table->radius = resampler_radius_for_cutoff(cutoff);
    const size_t taps = 2U * (size_t)table->radius;
    for (size_t phase = 0; phase < RESAMPLER_PHASES; phase++) {
        const double fraction = (double)phase / (double)RESAMPLER_PHASES;
        double weight_sum = 0.0;
        for (size_t tap_index = 0; tap_index < taps; tap_index++) {
            const int tap = (int)tap_index - table->radius + 1;
            const double distance = fraction - (double)tap;
            const double weight = sinc_value(cutoff * distance) * hann_window_radius(distance, table->radius);
            table->w[phase][tap_index] = (float)weight;
            weight_sum += weight;
        }
        if (fabs(weight_sum) >= 1e-12) {
            for (size_t tap_index = 0; tap_index < taps; tap_index++) {
                table->w[phase][tap_index] = (float)((double)table->w[phase][tap_index] / weight_sum);
            }
        }
    }
}

/* Tables are pure functions of the cutoff and are built once per distinct ratio into a small
 * global cache. The fast path is a lock-free scan of already-built entries; the mutex is taken
 * only to append a new one, so after the first block that uses a ratio the lookup never locks
 * (satisfies "no locking/table construction on the per-block hot path" once warmed). */
static resampler_table_t g_resampler_tables[RESAMPLER_TABLE_CACHE];
static _Atomic size_t g_resampler_table_count = 0;
static pthread_mutex_t g_resampler_table_lock = PTHREAD_MUTEX_INITIALIZER;

static const resampler_table_t *resampler_table_for_cutoff(double cutoff)
{
    size_t count = atomic_load_explicit(&g_resampler_table_count, memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        if (g_resampler_tables[i].cutoff == cutoff) {
            return &g_resampler_tables[i];
        }
    }
    pthread_mutex_lock(&g_resampler_table_lock);
    count = atomic_load_explicit(&g_resampler_table_count, memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (g_resampler_tables[i].cutoff == cutoff) {
            pthread_mutex_unlock(&g_resampler_table_lock);
            return &g_resampler_tables[i];
        }
    }
    const resampler_table_t *result = NULL;
    if (count < RESAMPLER_TABLE_CACHE) {
        build_resampler_table(&g_resampler_tables[count], cutoff);
        atomic_store_explicit(&g_resampler_table_count, count + 1, memory_order_release);
        result = &g_resampler_tables[count];
    }
    pthread_mutex_unlock(&g_resampler_table_lock);
    return result;
}

/* Table-driven resample at a fractional source position, using the nearest polyphase kernel.
 * Interior positions accumulate the full 8-tap window. Positions whose window runs off either end
 * of the buffer sum only the in-range taps and renormalise by their weight -- still transcendental
 * -free, which matters because a heavily-upsampled source (e.g. a 400 kHz pre-render into 98 MHz)
 * spends the first RADIUS/ratio output samples of every block in this edge region. */
static void resample_table_ci16(const iq_src_t *samples, size_t sample_count, double source_position, const resampler_table_t *table, double *out_i, double *out_q)
{
    const double center_f = floor(source_position);
    long phase = lrint((source_position - center_f) * (double)RESAMPLER_PHASES);
    int64_t center = (int64_t)center_f;
    if (phase >= (long)RESAMPLER_PHASES) {
        phase = 0;
        center += 1;
    }
    const float *weights = table->w[phase];
    const int radius = table->radius;
    const size_t taps = 2U * (size_t)radius;
    if (center < radius - 1 || (uint64_t)center + (uint64_t)radius >= sample_count) {
        double acc_i = 0.0;
        double acc_q = 0.0;
        double weight_sum = 0.0;
        for (size_t tap_index = 0; tap_index < taps; tap_index++) {
            const int64_t index = center + (int64_t)tap_index - radius + 1;
            if (index < 0 || (uint64_t)index >= sample_count) {
                continue;
            }
            const double weight = (double)weights[tap_index];
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
        return;
    }
    double acc_i = 0.0;
    double acc_q = 0.0;
    for (size_t tap_index = 0; tap_index < taps; tap_index++) {
        const int64_t index = center + (int64_t)tap_index - radius + 1;
        acc_i += (double)samples[index].i * (double)weights[tap_index];
        acc_q += (double)samples[index].q * (double)weights[tap_index];
    }
    *out_i = acc_i;
    *out_q = acc_q;
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

/* Saturating round of an int16-scale bus value to a 24-bit signed sample. The bus is in int16
 * units, so scale by 2^8 to fill the 24-bit range before rounding: the low 8 bits then carry
 * the float bus's sub-int16 precision instead of being zero. */
static int32_t clip_i24_f(float value)
{
    const float scaled = value * 256.0f;
    if (scaled > 8388607.0f) {
        return 8388607;
    }
    if (scaled < -8388608.0f) {
        return -8388608;
    }
    return (int32_t)lrintf(scaled);
}

/* Store one float-bus complex sample (int16-scale units) into the channel's native output
 * element at index i. CI16/CI24 saturate; CF32 divides to +-1.0 full scale and is NOT clamped
 * (IEEE-754 samples carry overrange, per VITA 49.2 6.1.1.4). */
static void store_output_sample(void *out, size_t i, sim_output_format_t format, float bus_i, float bus_q)
{
    switch (format) {
        case SIM_OUTPUT_FORMAT_CF32: {
            iq_cf32_t *o = (iq_cf32_t *)out;
            o[i].i = bus_i / 32768.0f;
            o[i].q = bus_q / 32768.0f;
            break;
        }
        case SIM_OUTPUT_FORMAT_CI24: {
            iq_ci24_t *o = (iq_ci24_t *)out;
            o[i].i = clip_i24_f(bus_i);
            o[i].q = clip_i24_f(bus_q);
            break;
        }
        case SIM_OUTPUT_FORMAT_CI16:
        default: {
            iq_ci16_t *o = (iq_ci16_t *)out;
            o[i].i = clip_i16_f(bus_i);
            o[i].q = clip_i16_f(bus_q);
            break;
        }
    }
}

#if !SIM_USE_LIQUID_RESAMPLER
static void resample_sinc_ci16(const iq_src_t *samples, size_t sample_count, double source_position, double cutoff, double *out_i, double *out_q)
{
    const int64_t center = (int64_t)floor(source_position);
    const int radius = resampler_radius_for_cutoff(cutoff);
    double acc_i = 0.0;
    double acc_q = 0.0;
    double weight_sum = 0.0;
    for (int tap = -radius + 1; tap <= radius; tap++) {
        const int64_t index = center + tap;
        if (index < 0 || (uint64_t)index >= sample_count) {
            continue;
        }
        const double distance = source_position - (double)index;
        /* Kernel band-limited to `cutoff` (<=1). The window spans the cutoff-scaled radius;
         * normalising by weight_sum keeps unity DC gain. */
        const double weight = sinc_value(cutoff * distance) * hann_window_radius(distance, radius);
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
static void resample_liquid_ci16(const iq_src_t *samples, size_t sample_count, double source_position, double cutoff, double *out_i, double *out_q)
{
    const int64_t center = (int64_t)floor(source_position);
    const int radius = resampler_radius_for_cutoff(cutoff);
    float weights[RESAMPLER_MAX_TAPS];
    liquid_float_complex input[RESAMPLER_MAX_TAPS];
    unsigned int tap_count = 0;
    double weight_sum = 0.0;
    for (int tap = -radius + 1; tap <= radius; tap++) {
        const int64_t index = center + tap;
        if (index < 0 || (uint64_t)index >= sample_count) {
            continue;
        }
        const double distance = source_position - (double)index;
        const double weight = sinc_value(cutoff * distance) * hann_window_radius(distance, radius);
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

static void resample_ci16(const iq_src_t *samples, size_t sample_count, double source_position, double cutoff, double *out_i, double *out_q)
{
#if SIM_USE_LIQUID_RESAMPLER
    resample_liquid_ci16(samples, sample_count, source_position, cutoff, out_i, out_q);
#else
    resample_sinc_ci16(samples, sample_count, source_position, cutoff, out_i, out_q);
#endif
}

static void resample_linear_ci16_f(const iq_src_t *samples, size_t sample_count, float source_position, float *out_i, float *out_q)
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

/* Audio-modulated signals are no longer synthesised on the hot path: they are pre-rendered to
 * complex-baseband int16-scale float at load (asset_cache.c, T3) and rendered through the ordinary
 * IQ dispatch below. The full-rate reference synthesis lives in the test suite as the parity oracle. */

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

/* Precomputed reservoir of unit-variance Gaussian noise (interleaved I/Q). It is generated once
 * with a fixed internal seed; the scenario seed only chooses which slice each block reads, so
 * the noise is white, has the correct crest factor (unlike the old uniform noise, whose RMS ran
 * ~4.8 dB below the configured power), costs almost nothing per block, and is identical across
 * instances with the same scenario seed. */
#define NOISE_POOL_SAMPLES (1U << 20) /* 1,048,576 complex samples */
static float g_noise_pool[2U * NOISE_POOL_SAMPLES];
static pthread_once_t g_noise_once = PTHREAD_ONCE_INIT;

static void init_noise_pool(void)
{
    uint64_t state = 0x1234567890abcdefULL;
    for (size_t i = 0; i < 2U * NOISE_POOL_SAMPLES; i++) {
        /* Irwin-Hall: sum of 12 uniforms in [0,1) minus 6 has unit variance and is close to
         * Gaussian -- a far better noise-floor crest factor than a single uniform. */
        double acc = 0.0;
        for (int k = 0; k < 12; k++) {
            state = splitmix64(state);
            acc += (double)(state >> 11) * (1.0 / 9007199254740992.0); /* [0,1) */
        }
        g_noise_pool[i] = (float)(acc - 6.0);
    }
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

    /* Target RMS amplitude of each I/Q component in ci16 counts. The total in-window power is the
     * spectral density spread over the window bandwidth (density + 10*log10(window_bandwidth)).
     * Because the pool is unit-RMS, the amplitude is exactly the RMS -- no distribution-dependent
     * correction is needed. */
    const double bandwidth = window_bandwidth_hz > 0U ? (double)window_bandwidth_hz : 1.0;
    const double power_dbm = scenario->noise_floor.power_dbm_per_hz + 10.0 * log10(bandwidth);
    const double amplitude = 32767.0 * output_scale * pow(10.0, (power_dbm - rf_reference_power_dbm) / 20.0);
    if (!(amplitude > 0.0)) {
        return;
    }

    pthread_once(&g_noise_once, init_noise_pool);

    const uint64_t sample_index = sample_index_from_time_ns(scenario_time_ns, output_sample_rate_hz);
    const uint64_t block_index = count > 0 ? sample_index / (uint64_t)count : sample_index;
    const uint64_t hash = splitmix64(scenario->noise_floor.seed) ^ splitmix64(block_index) ^
                          splitmix64(window_center_hz) ^ splitmix64(window_bandwidth_hz);
    /* Read a contiguous slice; keep it inside the pool so no bounds check is needed per sample. */
    const uint64_t max_offset = NOISE_POOL_SAMPLES - (count < NOISE_POOL_SAMPLES ? count : NOISE_POOL_SAMPLES);
    const size_t offset = max_offset > 0 ? (size_t)(hash % (max_offset + 1U)) : 0U;
    const float amp_f = (float)amplitude;

    const size_t n = count < NOISE_POOL_SAMPLES ? count : NOISE_POOL_SAMPLES;
    for (size_t i = 0; i < n; i++) {
        bus[2U * i] += amp_f * g_noise_pool[2U * (offset + i)];
        bus[2U * i + 1U] += amp_f * g_noise_pool[2U * (offset + i) + 1U];
    }
}

static void mix_accumulate_sample(float *bus, size_t index, double sample_i, double sample_q, double gain, double osc_c, double osc_s)
{
    const double ii = gain * sample_i;
    const double qq = gain * sample_q;
    bus[2U * index] += (float)(ii * osc_c - qq * osc_s);
    bus[2U * index + 1U] += (float)(ii * osc_s + qq * osc_c);
}

static void accumulate_direct_baseband(float *bus, const iq_src_t *source_samples, size_t index, double source_gain)
{
    bus[2U * index] += (float)(source_gain * (double)source_samples[index].i);
    bus[2U * index + 1U] += (float)(source_gain * (double)source_samples[index].q);
}

static void render_direct_baseband(const iq_src_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain)
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

static void render_direct_nco(const iq_src_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double init_c, double init_s, double step_c, double step_s)
{
    const size_t limit = read_count < count ? read_count : count;
#if SIM_HAVE_VOLK
    /* Aligned stack scratch instead of a per-block volk_malloc/volk_free. Capped at the maximum
     * streaming block size; an oversized one-shot render falls through to the scalar path. */
    if (limit >= 16 && limit <= SIM_MAX_STREAM_BLOCK_SAMPLES) {
        _Alignas(64) lv_32fc_t input[SIM_MAX_STREAM_BLOCK_SAMPLES];
        _Alignas(64) lv_32fc_t rotated[SIM_MAX_STREAM_BLOCK_SAMPLES];
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
        return;
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

/* Resample one output sample at `source_position`, using the polyphase table when available and
 * falling back to the exact per-position kernel when the ratio's table could not be cached. */
static void resample_position(const iq_src_t *source_samples, size_t read_count, double source_position, const resampler_table_t *table, double cutoff, double *out_i, double *out_q)
{
    if (table != NULL) {
        resample_table_ci16(source_samples, read_count, source_position, table, out_i, out_q);
    } else {
        resample_ci16(source_samples, read_count, source_position, cutoff, out_i, out_q);
    }
}

#if SIM_HAVE_VOLK
/* Polyphase-resample a whole block into interleaved float scratch (lv_32fc_t) and return the count
 * of valid output samples (the source is exhausted beyond that). The interior -- where the full
 * 8-tap window fits -- uses a branch-free single-precision dot product against the phase's float
 * kernel, which is what makes a heavily-upsampled source (e.g. a 400 kHz pre-render into 98 MHz,
 * one resample per output sample) cheap; buffer-edge positions fall back to the exact per-position
 * path. */
static size_t fill_scratch_polyphase(const iq_src_t *samples, size_t read_count, size_t count, double source_per_output, double offset_fraction, const resampler_table_t *table, double cutoff, lv_32fc_t *scratch)
{
    /* Interior samples advance the source position with a Q32.32 fixed-point accumulator, so the
     * integer sample index and the RESAMPLER_PHASES-quantised phase come out as a shift and a mask
     * instead of a floor()/lrint()/double-multiply per output sample -- the dominant cost when a
     * narrowband source is upsampled far (one resample per wideband output sample). */
    const uint64_t POS_ONE = 1ULL << 32U;
    const uint64_t step_q = (uint64_t)llround(source_per_output * (double)POS_ONE);
    uint64_t pos_q = (uint64_t)llround(offset_fraction * (double)POS_ONE);
    size_t valid = 0;
    for (; valid < count; valid++, pos_q += step_q) {
        uint64_t center = pos_q >> 32U;
        if (center >= read_count) {
            break;
        }
        if (table != NULL) {
            /* Round the 32-bit fraction to the nearest of RESAMPLER_PHASES phases, carrying into
             * the integer index when it rounds up to a full sample. */
            uint64_t phase = ((pos_q & 0xffffffffULL) * RESAMPLER_PHASES + (1ULL << 31U)) >> 32U;
            if (phase >= RESAMPLER_PHASES) {
                phase = 0;
                center += 1;
            }
            const uint64_t radius = (uint64_t)table->radius;
            if (center >= radius - 1U && center + radius < read_count) {
                const float *weights = table->w[phase];
                const iq_src_t *window = &samples[center - radius + 1U];
                const size_t taps = 2U * (size_t)radius;
                float acc_i = 0.0f;
                float acc_q = 0.0f;
                for (size_t tap_index = 0; tap_index < taps; tap_index++) {
                    acc_i += (float)window[tap_index].i * weights[tap_index];
                    acc_q += (float)window[tap_index].q * weights[tap_index];
                }
                scratch[valid] = acc_i + acc_q * I;
                continue;
            }
        }
        const double source_position = offset_fraction + (double)valid * source_per_output;
        double resampled_i = 0.0;
        double resampled_q = 0.0;
        resample_position(samples, read_count, source_position, table, cutoff, &resampled_i, &resampled_q);
        scratch[valid] = (float)resampled_i + (float)resampled_q * I;
    }
    return valid;
}
#endif

static void render_resampled_baseband(const iq_src_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double source_per_output, double offset_fraction, bool allow_linear)
{
    /* Plain IQ files keep the cheap linear shortcut for very low ratios (AR3); pre-rendered audio
     * disables it because linear image rejection (~-24 dB) is too poor for a modulated carrier. */
    const bool low_rate_linear = allow_linear && source_per_output > 0.0 && source_per_output <= LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT;
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
    const double cutoff = resampler_cutoff(source_per_output);
    const resampler_table_t *table = resampler_table_for_cutoff(cutoff);
    for (size_t i = 0; i < count; i++) {
        const double source_position = offset_fraction + (double)i * source_per_output;
        if ((size_t)floor(source_position) >= read_count) {
            break;
        }
        double resampled_i = 0.0;
        double resampled_q = 0.0;
        resample_position(source_samples, read_count, source_position, table, cutoff, &resampled_i, &resampled_q);
        bus[2U * i] += (float)(source_gain * resampled_i);
        bus[2U * i + 1U] += (float)(source_gain * resampled_q);
    }
}

static void render_resampled_nco(const iq_src_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double source_per_output, double offset_fraction, double init_c, double init_s, double step_c, double step_s, bool allow_linear)
{
    const bool low_rate_linear = allow_linear && source_per_output > 0.0 && source_per_output <= LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT;
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

    const double cutoff = resampler_cutoff(source_per_output);
    const resampler_table_t *table = resampler_table_for_cutoff(cutoff);

#if SIM_HAVE_VOLK
    /* Polyphase-resample the block into aligned float scratch, apply one VOLK rotator pass, then
     * accumulate. Same scratch/rotator machinery as the direct-NCO path (C2), now fed by the
     * resampler. Aligned stack scratch capped at the streaming block size; oversized one-shot
     * renders and near-empty blocks fall through to the scalar loop. */
    if (count >= 16 && count <= SIM_MAX_STREAM_BLOCK_SAMPLES) {
        _Alignas(64) lv_32fc_t scratch[SIM_MAX_STREAM_BLOCK_SAMPLES];
        _Alignas(64) lv_32fc_t rotated[SIM_MAX_STREAM_BLOCK_SAMPLES];
        const size_t valid = fill_scratch_polyphase(source_samples, read_count, count, source_per_output, offset_fraction, table, cutoff, scratch);
        if (valid >= 16) {
            lv_32fc_t phase = (float)init_c + (float)init_s * I;
            const lv_32fc_t phase_inc = (float)step_c + (float)step_s * I;
            volk_32fc_s32fc_x2_rotator2_32fc(rotated, scratch, &phase_inc, &phase, (unsigned int)valid);
            const float gain_f = (float)source_gain;
            for (size_t i = 0; i < valid; i++) {
                bus[2U * i] += gain_f * crealf(rotated[i]);
                bus[2U * i + 1U] += gain_f * cimagf(rotated[i]);
            }
            return;
        }
    }
#endif
    double osc_c = init_c;
    double osc_s = init_s;
    for (size_t i = 0; i < count; i++) {
        const double source_position = offset_fraction + (double)i * source_per_output;
        if ((size_t)floor(source_position) >= read_count) {
            break;
        }
        double resampled_i = 0.0;
        double resampled_q = 0.0;
        resample_position(source_samples, read_count, source_position, table, cutoff, &resampled_i, &resampled_q);
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

#define RESAMPLER_ROTATE_SCRATCH 66000U

/* Mix before decimate. The resample-then-rotate order band-limits the source around its own
 * centre and only then shifts it to the window offset, so any shifted content crossing the
 * output Nyquist folds back into the window as ghost signals. Rotating the source first (still
 * at the source rate) places the signal at its window offset before the decimating kernel
 * applies its output-Nyquist cutoff, which removes everything that cannot be represented --
 * the same order as a hardware DDC. The rotation phase is anchored so the block's first output
 * sample carries exactly phase0, keeping the output phase-continuous across blocks and
 * identical to the rotate-after path for in-band content. The rotated window is stored at half
 * scale (int16-scale float) for per-component headroom; the gain doubles to compensate. */
static bool render_resampled_nco_mix_first(const iq_src_t *source_samples, size_t read_count, float *bus, size_t count, double source_gain, double source_per_output, double offset_fraction, double offset_hz, double source_rate_hz, double phase0, bool allow_linear)
{
    static _Thread_local iq_src_t rotated[RESAMPLER_ROTATE_SCRATCH];
    if (read_count > RESAMPLER_ROTATE_SCRATCH) {
        return false; /* oversized one-shot render: caller falls back to rotate-after */
    }
    const double step = 2.0 * M_PI * offset_hz / source_rate_hz;
    const double theta0 = phase0 - step * offset_fraction;
    double osc_c = cos(theta0);
    double osc_s = sin(theta0);
    const double step_c = cos(step);
    const double step_s = sin(step);
    for (size_t k = 0; k < read_count; k++) {
        const double sample_i = 0.5 * (double)source_samples[k].i;
        const double sample_q = 0.5 * (double)source_samples[k].q;
        rotated[k].i = (float)(sample_i * osc_c - sample_q * osc_s);
        rotated[k].q = (float)(sample_i * osc_s + sample_q * osc_c);
        const double next_c = osc_c * step_c - osc_s * step_s;
        const double next_s = osc_s * step_c + osc_c * step_s;
        osc_c = next_c;
        osc_s = next_s;
        if ((k & 0xffU) == 0xffU) {
            const double inv = 1.0 / sqrt(osc_c * osc_c + osc_s * osc_s);
            osc_c *= inv;
            osc_s *= inv;
        }
    }
    render_resampled_baseband(rotated, read_count, bus, count, 2.0 * source_gain, source_per_output, offset_fraction, allow_linear);
    return true;
}

/* --- Multi-stage DDC path for large integer decimation ratios ---------------------------
 *
 * Narrow channels extracting a sub-band from a wideband looping recording (ratio > 16, where
 * the single-stage resampler's tap cap can no longer reject aliases) render through a
 * designed decimation cascade instead (ddc.h). The block is still a pure function of the
 * absolute grid index: the cascade is re-primed every block with plan->history_source_samples
 * of history, the exact generalization of the resampler's history_samples trick below.
 *
 * Normally the cascade is split in two hops around a cached intermediate sub-band
 * (ddc_cache.h): the source-rate front half runs once per (recording, tune-grid area) and is
 * reused across blocks and revisited tunes; per block only the residual rotation and the tail
 * stages run, at the intermediate rate. The direct single-hop cascade remains the fallback
 * when the loop cannot be cached (length not divisible by the front decimation, budget). */

static ddc_cache_t *g_ddc_cache = NULL;
static size_t g_ddc_cache_max_bytes = RENDERER_DDC_CACHE_DEFAULT_BYTES;
static pthread_mutex_t g_ddc_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_ddc_background_builds = false;

void renderer_ddc_background_builds(bool enabled)
{
    atomic_store_explicit(&g_ddc_background_builds, enabled, memory_order_relaxed);
}

void renderer_ddc_cache_configure(size_t max_bytes)
{
    pthread_mutex_lock(&g_ddc_cache_lock);
    g_ddc_cache_max_bytes = max_bytes;
    if (g_ddc_cache != NULL) {
        ddc_cache_destroy(g_ddc_cache);
        g_ddc_cache = NULL;
    }
    pthread_mutex_unlock(&g_ddc_cache_lock);
}

static ddc_cache_t *renderer_ddc_cache(void)
{
    pthread_mutex_lock(&g_ddc_cache_lock);
    if (g_ddc_cache == NULL && g_ddc_cache_max_bytes > 0) {
        g_ddc_cache = ddc_cache_create(g_ddc_cache_max_bytes);
    }
    ddc_cache_t *cache = g_ddc_cache;
    pthread_mutex_unlock(&g_ddc_cache_lock);
    return cache;
}

/* Run one cascade hop for a whole block and accumulate into the mix bus. `samples` is the
 * hop's input loop (raw recording or cached intermediate); input index i of the hop grid maps
 * to samples[i mod loop] (zero before the signal's start, i < 0). The rotation NCO is
 * anchored to the absolute input index via the Q0.64 phase step -- exact even for negative
 * indices, since the wrapping multiply is modulo one turn. `first_output` is the block's
 * first output index on the signal-relative output grid. */
static void ddc_render_hop(const iq_src_t *samples,
                           uint64_t loop_samples,
                           uint32_t input_rate_hz,
                           const ddc_plan_t *plan,
                           double rotate_hz,
                           int64_t first_output,
                           size_t count,
                           double gain,
                           float *bus)
{
    static _Thread_local ddc_exec_t exec;
    float complex rotated[DDC_EXEC_CHUNK];
    float complex produced[DDC_EXEC_CHUNK / 2U + 2U];

    const int64_t ratio = (int64_t)plan->ratio;
    const int64_t reach = (int64_t)plan->history_source_samples;
    const int64_t feed_first = first_output * ratio - reach;
    const int64_t feed_total = (int64_t)(count - 1) * ratio + 2 * reach + 1;
    const int64_t loop = (int64_t)loop_samples;

    ddc_exec_init(&exec, plan, feed_first);
    const uint64_t step_q64 = nco_phase_step_q64(rotate_hz, (double)input_rate_hz);
    const double step = 2.0 * M_PI * rotate_hz / (double)input_rate_hz;
    const double step_c = cos(step);
    const double step_s = sin(step);
    const float gain_f = (float)gain;

    size_t out_done = 0;
    for (int64_t done = 0; done < feed_total; done += (int64_t)DDC_EXEC_CHUNK) {
        const int64_t remaining = feed_total - done;
        const size_t chunk = remaining < (int64_t)DDC_EXEC_CHUNK ? (size_t)remaining : DDC_EXEC_CHUNK;
        const int64_t chunk_first = feed_first + done;
        /* Drift-free: the oscillator recurrence is re-anchored every chunk from the absolute
         * input index (same pattern as the mix-first path's per-block anchor). */
        const double phase0 = nco_phase_rad_at(step_q64, (uint64_t)chunk_first);
        double osc_c = cos(phase0);
        double osc_s = sin(phase0);
        int64_t wrapped = ((chunk_first % loop) + loop) % loop;
        int64_t index = chunk_first;
        for (size_t k = 0; k < chunk; k++) {
            if (index < 0) {
                rotated[k] = CMPLXF(0.0f, 0.0f);
            } else {
                const double sample_i = (double)samples[wrapped].i;
                const double sample_q = (double)samples[wrapped].q;
                rotated[k] = CMPLXF((float)(sample_i * osc_c - sample_q * osc_s),
                                    (float)(sample_i * osc_s + sample_q * osc_c));
            }
            index++;
            wrapped++;
            if (wrapped == loop) {
                wrapped = 0;
            }
            const double next_c = osc_c * step_c - osc_s * step_s;
            const double next_s = osc_s * step_c + osc_c * step_s;
            osc_c = next_c;
            osc_s = next_s;
            if ((k & 0xffU) == 0xffU) {
                const double inv = 1.0 / sqrt(osc_c * osc_c + osc_s * osc_s);
                osc_c *= inv;
                osc_s *= inv;
            }
        }
        const size_t space = count - out_done;
        const size_t capacity = sizeof(produced) / sizeof(produced[0]) < space
            ? sizeof(produced) / sizeof(produced[0])
            : space;
        const size_t emitted = ddc_exec_push(&exec, rotated, chunk, produced, capacity);
        for (size_t k = 0; k < emitted; k++) {
            bus[2U * (out_done + k)] += gain_f * crealf(produced[k]);
            bus[2U * (out_done + k) + 1U] += gain_f * cimagf(produced[k]);
        }
        out_done += emitted;
    }
}

/* Render a qualifying signal (looping IQ replay, integer ratio > threshold) through the DDC
 * cascade: cached two-hop when possible, direct single hop otherwise. Returns false when no
 * plan can be designed for this rate/bandwidth combination (caller falls back to the legacy
 * resampler) or the block starts before the signal does. */
static bool render_signal_ddc(const iq_src_t *samples_base,
                              uint64_t total_samples,
                              uint32_t source_rate_hz,
                              const char *source_id,
                              const scenario_signal_t *signal,
                              double offset_hz,
                              double source_gain,
                              uint32_t output_sample_rate_hz,
                              uint32_t channel_bandwidth_hz,
                              uint64_t abs_start_sample,
                              float *bus,
                              size_t count)
{
    const ddc_plan_t *full_plan =
        ddc_plan_cache_get(source_rate_hz, output_sample_rate_hz, channel_bandwidth_hz);
    if (full_plan == NULL) {
        return false;
    }
    /* Same start anchor as iq_signal_loop_position: output index m plays unwrapped source
     * sample (m - start_out) * ratio. The rotation phase is likewise anchored to the
     * signal-relative grid, so the cached and direct hops agree exactly. */
    const uint64_t start_out =
        (uint64_t)llround(signal->start_time_s * (double)output_sample_rate_hz);
    if (abs_start_sample < start_out) {
        return false; /* block straddles the signal start: legacy path, once */
    }
    const int64_t first_output = (int64_t)(abs_start_sample - start_out);

    const uint32_t intermediate_rate_hz =
        ddc_intermediate_rate_hz(source_rate_hz, output_sample_rate_hz);
    if (intermediate_rate_hz != 0) {
        const uint32_t front_bandwidth_hz = ddc_front_bandwidth_hz(intermediate_rate_hz);
        const ddc_plan_t *front_plan =
            ddc_plan_cache_get(source_rate_hz, intermediate_rate_hz, front_bandwidth_hz);
        const ddc_plan_t *tail_plan =
            ddc_plan_cache_get(intermediate_rate_hz, output_sample_rate_hz, channel_bandwidth_hz);
        if (front_plan != NULL && tail_plan != NULL && total_samples % front_plan->ratio == 0) {
            /* Quantize the shift to the tune grid so revisited tune areas share one entry;
             * the residual (grid remainder plus the cache's whole-cycles snap) is applied
             * exactly by the NCO at the intermediate rate. */
            const uint32_t grid_step_hz = ddc_grid_step_hz(intermediate_rate_hz);
            const double shift_request =
                (double)ddc_grid_center_hz((int64_t)llround(offset_hz), grid_step_hz);
            const ddc_cache_entry_t *entry = ddc_cache_acquire(
                renderer_ddc_cache(), source_id, samples_base, total_samples, source_rate_hz,
                shift_request, intermediate_rate_hz, front_plan,
                !atomic_load_explicit(&g_ddc_background_builds, memory_order_relaxed));
            if (entry != NULL) {
                const double residual_hz = offset_hz - entry->applied_shift_hz;
                if (fabs(residual_hz) + (double)channel_bandwidth_hz / 2.0 <=
                    (double)front_bandwidth_hz / 2.0) {
                    ddc_render_hop(entry->samples, entry->sample_count, intermediate_rate_hz,
                                   tail_plan, residual_hz, first_output, count,
                                   source_gain / DDC_CACHE_SAMPLE_SCALE, bus);
                    ddc_cache_release(renderer_ddc_cache(), entry);
                    return true;
                }
                ddc_cache_release(renderer_ddc_cache(), entry);
            }
        }
    }

    ddc_render_hop(samples_base, total_samples, source_rate_hz, full_plan, offset_hz,
                   first_output, count, source_gain, bus);
    return true;
}

/* Render one contiguous stretch of a signal into the mix bus, starting at output sample
 * `abs_start_sample` (absolute grid index) and bus position `out_offset`. This is the former
 * tail of the per-signal loop, factored out so looping signals can render a block in segments
 * split at the file seam: the NCO phase is anchored to the absolute index, so phase stays
 * continuous across segments and blocks for free. force_rotate_after keeps shift-mode replay
 * on the pure-rotation paths (never the band-limiting mix-first DDC) for moderate ratios,
 * where spectral wrap-around is that mode's specified semantics; large integer ratios take
 * the render_signal_ddc sub-band extraction path before ever reaching here. */
static void render_signal_segment(
    const iq_src_t *samples_base,
    uint64_t total_samples,
    uint32_t source_rate_hz,
    bool allow_linear,
    uint64_t sample_offset,
    double offset_fraction,
    double offset_hz,
    double source_gain,
    uint32_t output_sample_rate_hz,
    uint64_t abs_start_sample,
    bool force_rotate_after,
    float *bus,
    size_t out_offset,
    size_t count)
{
    float *seg_bus = bus + 2U * out_offset;
    const double source_per_output = (double)source_rate_hz / (double)output_sample_rate_hz;
    const uint64_t kernel_radius = (uint64_t)resampler_radius_for_cutoff(resampler_cutoff(source_per_output));
    const uint64_t needed_source_samples = (uint64_t)ceil((double)(count > 0 ? count - 1 : 0) * source_per_output) + kernel_radius + 2ULL;
    const uint64_t available = sample_offset < total_samples ? total_samples - sample_offset : 0;
    const size_t read_count = available < needed_source_samples ? (size_t)available : (size_t)needed_source_samples;
    const iq_src_t *source_samples = &samples_base[sample_offset];
    /* The resampler additionally sees up to RESAMPLER_RADIUS samples of history before the
     * block's first source sample. Without it, the first output samples of every block were
     * filtered with the edge-truncated kernel while the rest used the interior one -- a tiny
     * filter discontinuity stamped onto every block boundary, which raised the FFT noise
     * floor periodically (visible as row banding on every resampled channel). Only the true
     * start of an asset still uses the edge kernel, once per repeat cycle. */
    const uint64_t history_samples = sample_offset < kernel_radius ? sample_offset : kernel_radius;
    const iq_src_t *resampler_samples = &samples_base[sample_offset - history_samples];
    const double resampler_fraction = offset_fraction + (double)history_samples;
    const uint64_t resampler_needed = needed_source_samples + history_samples;
    const uint64_t resampler_available = available + history_samples;
    const size_t resampler_read = resampler_available < resampler_needed ? (size_t)resampler_available : (size_t)resampler_needed;

    const double phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
    const double step_c = cos(phase_step);
    const double step_s = sin(phase_step);
    /* Continuous starting phase for this segment, from the absolute output-sample index. */
    const uint64_t phase_step_q64 = nco_phase_step_q64(offset_hz, (double)output_sample_rate_hz);
    const double phase0 = nco_phase_rad_at(phase_step_q64, abs_start_sample);
    const double init_c = cos(phase0);
    const double init_s = sin(phase0);
    /* A nonzero fractional playback position means the segment does not start on an integer
     * source sample, so the direct (integer-aligned) paths can't represent it -- fall back
     * to the resampler, which starts at the exact fractional position. */
    const bool integer_aligned = offset_fraction < 1e-9;
    if (source_rate_hz == output_sample_rate_hz && offset_hz == 0.0 && integer_aligned) {
        render_direct_baseband(source_samples, read_count, seg_bus, count, source_gain);
    } else if (source_rate_hz == output_sample_rate_hz && integer_aligned) {
        render_direct_nco(source_samples, read_count, seg_bus, count, source_gain, init_c, init_s, step_c, step_s);
    } else if (offset_hz == 0.0) {
        render_resampled_baseband(resampler_samples, resampler_read, seg_bus, count, source_gain, source_per_output, resampler_fraction, allow_linear);
    } else {
        /* Rotate-after only folds when the shifted source band can cross the output
         * Nyquist; route those cases through the mix-first path. */
        const bool shift_can_fold = !force_rotate_after &&
            (source_per_output > 1.0 ||
             fabs(offset_hz) + (double)source_rate_hz / 2.0 > (double)output_sample_rate_hz / 2.0);
        if (!shift_can_fold ||
            !render_resampled_nco_mix_first(resampler_samples, resampler_read, seg_bus, count, source_gain, source_per_output, resampler_fraction, offset_hz, (double)source_rate_hz, phase0, allow_linear)) {
            render_resampled_nco(resampler_samples, resampler_read, seg_bus, count, source_gain, source_per_output, resampler_fraction, init_c, init_s, step_c, step_s, allow_linear);
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
    void *out,
    sim_output_format_t format,
    size_t count,
    render_stats_t *stats)
{
    memset(out, 0, count * sim_internal_bytes_per_sample(format));
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
        /* Passthrough signals are handled exclusively by renderer_try_passthrough (which owns the
         * whole channel when active); the mixer is only reached when none are active, and their
         * variant sources carry no single samples buffer to mix here anyway. */
        if (signal->passthrough) {
            continue;
        }
        const scenario_source_t *source = scenario_find_source(scenario, signal->source_reference);
        const cached_asset_t *asset = asset_cache_find(cache, signal->source_reference);
        if (source == NULL || asset == NULL) {
            continue;
        }

        /* Audio-modulated signals were pre-rendered to complex-baseband int16-scale float at load
         * (T3); from here they are indistinguishable from an IQ source -- same dispatch, phase/offset
         * machinery. The pre-render buffer, its intermediate rate, and its recorded peak gain
         * stand in for the audio source's samples/rate/gain. */
        const bool is_audio = source->source_kind == SCENARIO_SOURCE_AUDIO_FILE;
        const cached_prerender_t *prerender = is_audio ? asset_cache_prerender(cache, s) : NULL;
        if (is_audio && prerender == NULL) {
            continue;
        }
        const iq_src_t *samples_base = is_audio ? prerender->samples : asset->samples;
        const uint64_t total_samples = is_audio ? prerender->sample_count : asset->sample_count;
        const uint32_t source_rate_hz = is_audio ? prerender->sample_rate_hz : source->sample_rate_hz;
        /* Pre-rendered assets always take the polyphase path: linear interpolation (~-24 dB image
         * rejection) is too poor for a modulated carrier (AR3). Plain IQ files keep the shortcut,
         * but only when the source is oversampled enough (>= LINEAR_MIN_SOURCE_OVERSAMPLE) that the
         * linear images stay deep in the stopband; a source occupying a large fraction of its own
         * rate (e.g. narrowband captures like POCSAG at 15 kHz in 32 ksps) images visibly under
         * linear and takes the polyphase path instead. */
        const bool allow_linear = !is_audio && signal->bandwidth_hz > 0U &&
            (uint64_t)signal->bandwidth_hz * LINEAR_MIN_SOURCE_OVERSAMPLE <= (uint64_t)source_rate_hz;

        scenario_source_t active_source = *source;
        active_source.sample_rate_hz = source_rate_hz;
        active_source.sample_count = total_samples;

        double passband_gain;
        double offset_hz;
        uint64_t sample_offset = 0;
        double offset_fraction = 0.0;
        if (signal->replay_mode != SCENARIO_REPLAY_FIXED) {
            /* Range/shift replay: active purely by tune position, file streamed verbatim
             * (no passband weighting). Range mode follows the tune (offset 0, identical
             * output anywhere in the range); shift mode keeps the content at its absolute
             * RF position via a pure rotation by f0 - f_tune. */
            if (window_center_hz < signal->replay_range_start_hz ||
                window_center_hz > signal->replay_range_stop_hz) {
                continue;
            }
            passband_gain = 1.0;
            offset_hz = signal->replay_mode == SCENARIO_REPLAY_RANGE
                ? 0.0
                : (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
            if (!iq_signal_loop_position(signal, &active_source, start_sample, output_sample_rate_hz, &sample_offset, &offset_fraction, NULL)) {
                continue;
            }
        } else {
            passband_gain = signal_passband_gain(signal->center_frequency_hz, signal->bandwidth_hz, window_center_hz, window_bandwidth_hz);
            if (passband_gain <= 0.0) {
                continue;
            }
            if (signal->loop
                    ? !iq_signal_loop_position(signal, &active_source, start_sample, output_sample_rate_hz, &sample_offset, &offset_fraction, NULL)
                    : !iq_signal_active(signal, &active_source, scenario_time_ns, &sample_offset, &offset_fraction)) {
                continue;
            }
            offset_hz = (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
            /* If the whole signal band, once shifted to baseband, lies beyond the output Nyquist it
             * cannot be represented and would only fold back as aliases -- skip it. signal_passband_gain
             * only tests overlap with the window bandwidth, which can exceed the output rate, so this
             * catches the case the passband gain does not. (The straddle case, where a band crosses the
             * window/Nyquist edge, is still only attenuated -- a documented approximation.) Shift-mode
             * replay never gets here: its wrap-around is accepted semantics. */
            if (fabs(offset_hz) - (double)signal->bandwidth_hz / 2.0 > (double)output_sample_rate_hz / 2.0) {
                continue;
            }
        }
        double source_gain = passband_gain * output_scale * pow(10.0, (signal->power_dbm - rf_reference_power_dbm) / 20.0);
        if (is_audio) {
            source_gain *= prerender->gain;
        }

        /* Looping IQ replay into a much lower-rate channel is a true DDC: sub-band
         * extraction through the multi-stage cascade. Everything else (moderate ratios,
         * bursts, audio pre-renders) keeps the byte-identical legacy paths. */
        if (signal->loop && !is_audio &&
            output_sample_rate_hz != 0 && source_rate_hz % output_sample_rate_hz == 0U &&
            source_rate_hz / output_sample_rate_hz > DDC_CASCADE_RATIO_THRESHOLD &&
            render_signal_ddc(samples_base, total_samples, source_rate_hz,
                              signal->source_reference, signal, offset_hz, source_gain,
                              output_sample_rate_hz, (uint32_t)window_bandwidth_hz,
                              start_sample, bus, count)) {
            if (stats != NULL) {
                stats->active_signals++;
            }
            continue;
        }

        const bool force_rotate_after = signal->replay_mode == SCENARIO_REPLAY_SHIFT;
        if (!signal->loop) {
            render_signal_segment(samples_base, total_samples, source_rate_hz, allow_linear,
                                  sample_offset, offset_fraction, offset_hz, source_gain,
                                  output_sample_rate_hz, start_sample, force_rotate_after,
                                  bus, 0, count);
        } else {
            /* Looping signals render in segments split at the file seam, so a block that
             * straddles the loop boundary carries the end of the file immediately followed
             * by its start instead of a truncated tail. Positions are recomputed from the
             * absolute index each segment (exact, no drift); samples_until_wrap >= 1, so the
             * walk always terminates. */
            size_t out_done = 0;
            while (out_done < count) {
                uint64_t seg_offset = 0;
                double seg_fraction = 0.0;
                uint64_t until_wrap = 0;
                if (!iq_signal_loop_position(signal, &active_source, start_sample + out_done, output_sample_rate_hz, &seg_offset, &seg_fraction, &until_wrap)) {
                    break;
                }
                const size_t remaining = count - out_done;
                const size_t seg_count = until_wrap < (uint64_t)remaining ? (size_t)until_wrap : remaining;
                render_signal_segment(samples_base, total_samples, source_rate_hz, allow_linear,
                                      seg_offset, seg_fraction, offset_hz, source_gain,
                                      output_sample_rate_hz, start_sample + out_done, force_rotate_after,
                                      bus, out_done, seg_count);
                out_done += seg_count;
            }
        }
        if (stats != NULL) {
            stats->active_signals++;
        }
    }

    /* Single conversion of the whole block into the channel's native output format. */
    for (size_t i = 0; i < count; i++) {
        store_output_sample(out, i, format, bus[2U * i], bus[2U * i + 1U]);
    }
    free(bus_heap);

    if (stats != NULL) {
        stats->samples_rendered = count;
    }
    return true;
}

/* Read a native passthrough-variant sample (whose format matches the channel) into int16-scale
 * float I/Q, so the gain/rotation math and store_output_sample below are shared across formats.
 * CI24 divides by 2^8 and CF32 multiplies by 2^15 to reach int16 units; store_output_sample
 * applies the inverse, so a unit-gain no-shift copy is loss-free (and skipped via memcpy). */
static void read_variant_int16scale(const void *src, size_t i, sim_output_format_t format, float *fi, float *fq)
{
    switch (format) {
        case SIM_OUTPUT_FORMAT_CF32: {
            const iq_cf32_t *p = (const iq_cf32_t *)src;
            *fi = p[i].i * 32768.0f;
            *fq = p[i].q * 32768.0f;
            break;
        }
        case SIM_OUTPUT_FORMAT_CI24: {
            const iq_ci24_t *p = (const iq_ci24_t *)src;
            *fi = (float)p[i].i / 256.0f;
            *fq = (float)p[i].q / 256.0f;
            break;
        }
        case SIM_OUTPUT_FORMAT_CI16:
        default: {
            const iq_ci16_t *p = (const iq_ci16_t *)src;
            *fi = (float)p[i].i;
            *fq = (float)p[i].q;
            break;
        }
    }
}

static void passthrough_copy_segment(const void *src, void *out, size_t count, double gain, sim_output_format_t format)
{
    if (fabs(gain - 1.0) < 1e-9) {
        /* Exact unit gain: literal file samples, byte-for-byte -- the "just wrap VITA49 around
         * the IQ data" case. src and out are the same native format. */
        memcpy(out, src, count * sim_internal_bytes_per_sample(format));
        return;
    }
    for (size_t i = 0; i < count; i++) {
        float fi, fq;
        read_variant_int16scale(src, i, format, &fi, &fq);
        store_output_sample(out, i, format, (float)(gain * (double)fi), (float)(gain * (double)fq));
    }
}

static void passthrough_rotate_segment(const void *src, void *out, size_t count, double gain, double init_c, double init_s, double step_c, double step_s, sim_output_format_t format)
{
#if SIM_HAVE_VOLK
    if (count >= 16 && count <= SIM_MAX_STREAM_BLOCK_SAMPLES) {
        _Alignas(64) lv_32fc_t input[SIM_MAX_STREAM_BLOCK_SAMPLES];
        _Alignas(64) lv_32fc_t rotated[SIM_MAX_STREAM_BLOCK_SAMPLES];
        for (size_t i = 0; i < count; i++) {
            float fi, fq;
            read_variant_int16scale(src, i, format, &fi, &fq);
            input[i] = fi + fq * I;
        }
        lv_32fc_t phase = (float)init_c + (float)init_s * I;
        const lv_32fc_t phase_inc = (float)step_c + (float)step_s * I;
        volk_32fc_s32fc_x2_rotator2_32fc(rotated, input, &phase_inc, &phase, (unsigned int)count);
        const float gain_f = (float)gain;
        for (size_t i = 0; i < count; i++) {
            store_output_sample(out, i, format, gain_f * crealf(rotated[i]), gain_f * cimagf(rotated[i]));
        }
        return;
    }
#endif
    double osc_c = init_c;
    double osc_s = init_s;
    for (size_t i = 0; i < count; i++) {
        float fi, fq;
        read_variant_int16scale(src, i, format, &fi, &fq);
        const double ii = gain * (double)fi;
        const double qq = gain * (double)fq;
        store_output_sample(out, i, format, (float)(ii * osc_c - qq * osc_s), (float)(ii * osc_s + qq * osc_c));
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

/* Direct file-to-packet replay for a channel dedicated to one `passthrough: true` signal: no
 * float mix bus, no noise floor, no other signals considered -- just this source's samples,
 * optionally gain-scaled and/or rotated for shift mode, written straight to the output block.
 * This is the "just wrap VITA49 around the IQ data" path the general mixer
 * (renderer_render_window_block) cannot offer, because that one exists to combine an arbitrary
 * number of overlapping signals plus noise into one window.
 *
 * The passthrough source carries one capture per channel rate (its variants). This function
 * selects the variant whose rate matches the channel and streams it verbatim; it never resamples.
 * Return values:
 *  - false: no passthrough signal is active at this tune (out of every range) -- the caller falls
 *    back to the general mixer, which handles any ordinary signals.
 *  - true with rendered samples: an active passthrough signal has a variant matching the channel
 *    rate; it took over the channel (passthrough is exclusive by design).
 *  - true with silence: an active passthrough signal has NO variant for this channel rate. Rather
 *    than resample (which passthrough must never do), the channel goes silent -- the intended
 *    behaviour for a bandwidth the capture set does not cover. */
static bool renderer_try_passthrough(const scenario_t *scenario, const asset_cache_t *cache, uint64_t window_center_hz, uint32_t output_sample_rate_hz, double output_scale, double rf_reference_power_dbm, uint64_t scenario_time_ns, void *out, sim_output_format_t format, size_t count, render_stats_t *stats)
{
    const size_t bps = sim_internal_bytes_per_sample(format);
    uint8_t *out_bytes = (uint8_t *)out;
    for (size_t s = 0; s < scenario->signal_count; s++) {
        const scenario_signal_t *signal = &scenario->signals[s];
        if (!signal->passthrough) {
            continue;
        }
        if (window_center_hz < signal->replay_range_start_hz || window_center_hz > signal->replay_range_stop_hz) {
            continue;
        }
        /* A passthrough signal is active for this tune -- it owns the channel from here, whether
         * or not a matching-rate capture exists. */
        const cached_asset_t *asset = asset_cache_find(cache, signal->source_reference);
        const cached_iq_buffer_t *variant = NULL;
        if (asset != NULL) {
            for (size_t k = 0; k < asset->variant_count; k++) {
                /* Verbatim replay requires a variant at this rate AND in the channel's format;
                 * a format mismatch is treated like a rate mismatch -> silence, never convert. */
                if (asset->variants[k].sample_rate_hz == output_sample_rate_hz &&
                    asset->variants[k].format == format) {
                    variant = &asset->variants[k];
                    break;
                }
            }
        }
        if (variant == NULL || variant->samples == NULL || variant->sample_count == 0) {
            /* No capture at this bandwidth: silence, never resample. */
            memset(out, 0, count * bps);
            if (stats != NULL) {
                memset(stats, 0, sizeof(*stats));
                stats->samples_rendered = count;
            }
            return true;
        }

        /* iq_signal_loop_position keys off the source's rate and sample count; feed it the
         * selected variant's so the wrap period matches the file actually being streamed. */
        scenario_source_t variant_source;
        memset(&variant_source, 0, sizeof(variant_source));
        variant_source.sample_rate_hz = variant->sample_rate_hz;
        variant_source.sample_count = variant->sample_count;

        const double offset_hz = signal->replay_mode == SCENARIO_REPLAY_RANGE
            ? 0.0
            : (double)((int64_t)signal->center_frequency_hz - (int64_t)window_center_hz);
        const double gain = output_scale * pow(10.0, (signal->power_dbm - rf_reference_power_dbm) / 20.0);
        const uint64_t start_sample =
            (uint64_t)(((__uint128_t)scenario_time_ns * (uint64_t)output_sample_rate_hz + 500000000ULL) / 1000000000ULL);

        size_t out_done = 0;
        while (out_done < count) {
            uint64_t seg_offset = 0;
            double seg_fraction = 0.0;
            uint64_t until_wrap = 0;
            if (!iq_signal_loop_position(signal, &variant_source, start_sample + out_done, output_sample_rate_hz, &seg_offset, &seg_fraction, &until_wrap)) {
                memset(out_bytes + out_done * bps, 0, (count - out_done) * bps);
                break;
            }
            /* Equal rates guarantee seg_fraction is always exactly 0 (iq_signal_loop_position's
             * contract), so the copy/rotate below never needs sub-sample interpolation. */
            const size_t remaining = count - out_done;
            const size_t seg_count = until_wrap < (uint64_t)remaining ? (size_t)until_wrap : remaining;
            const void *src = (const uint8_t *)variant->samples + seg_offset * bps;
            if (offset_hz == 0.0) {
                passthrough_copy_segment(src, out_bytes + out_done * bps, seg_count, gain, format);
            } else {
                const double phase_step = 2.0 * M_PI * offset_hz / (double)output_sample_rate_hz;
                const uint64_t phase_step_q64 = nco_phase_step_q64(offset_hz, (double)output_sample_rate_hz);
                const double phase0 = nco_phase_rad_at(phase_step_q64, start_sample + out_done);
                passthrough_rotate_segment(src, out_bytes + out_done * bps, seg_count, gain, cos(phase0), sin(phase0), cos(phase_step), sin(phase_step), format);
            }
            out_done += seg_count;
        }
        if (stats != NULL) {
            memset(stats, 0, sizeof(*stats));
            stats->samples_rendered = count;
            stats->active_signals = 1;
        }
        return true;
    }
    return false;
}

bool renderer_render_channel_block(const scenario_t *scenario, const asset_cache_t *cache, const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns, void *out, size_t count, render_stats_t *stats)
{
    const sim_output_format_t format = channel->output_format;
    /* A channel whose span leaves the front-end (ADC) window carries no signal, matching a
     * hardware DDC tuned outside the digitised band: the stream keeps flowing, but empty. */
    if (!receiver_channel_in_window(receiver, channel, scenario_time_ns)) {
        memset(out, 0, count * sim_internal_bytes_per_sample(format));
        if (stats != NULL) {
            memset(stats, 0, sizeof(*stats));
            stats->samples_rendered = count;
        }
        return true;
    }
    const uint64_t center_hz = receiver_channel_center_hz(receiver, channel, scenario_time_ns);
    if (renderer_try_passthrough(scenario, cache, center_hz, channel->sample_rate_hz, channel->output_scale, channel->rf_reference_power_dbm, scenario_time_ns, out, format, count, stats)) {
        return true;
    }
    return renderer_render_window_block(scenario, cache, center_hz, channel->bandwidth_hz, channel->sample_rate_hz, channel->output_scale, channel->rf_reference_power_dbm, scenario_time_ns, out, format, count, stats);
}
