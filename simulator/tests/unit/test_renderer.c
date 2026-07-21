#include "config.h"
#include "asset_cache.h"
#include "renderer.h"
#include "scenario.h"
#include "streamer.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t interpolate_scaled_i16(int16_t a, int16_t b, double frac, double gain)
{
    const double value = gain * ((double)a + frac * ((double)b - (double)a));
    if (value > 32767.0) {
        return 32767;
    }
    if (value < -32768.0) {
        return -32768;
    }
    return (int16_t)lrint(value);
}

static double test_sinc(double x)
{
    if (fabs(x) < 1e-12) {
        return 1.0;
    }
    return sin(M_PI * x) / (M_PI * x);
}

static double test_hann(double distance)
{
    const double normalized = fabs(distance) / 4.0;
    if (normalized >= 1.0) {
        return 0.0;
    }
    return 0.5 + 0.5 * cos(M_PI * normalized);
}

static int16_t sinc_scaled_i16(const iq_ci16_t *samples, size_t sample_count, double source_position, bool q, double gain)
{
    const int64_t center = (int64_t)floor(source_position);
    double acc = 0.0;
    double weight_sum = 0.0;
    for (int tap = -3; tap <= 4; tap++) {
        const int64_t index = center + tap;
        if (index < 0 || (uint64_t)index >= sample_count) {
            continue;
        }
        const double distance = source_position - (double)index;
        const double weight = test_sinc(distance) * test_hann(distance);
        acc += (double)(q ? samples[index].q : samples[index].i) * weight;
        weight_sum += weight;
    }
    return interpolate_scaled_i16((int16_t)lrint(acc / weight_sum), (int16_t)lrint(acc / weight_sum), 0.0, gain);
}

static bool samples_have_energy(const iq_ci16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (samples[i].i != 0 || samples[i].q != 0) {
            return true;
        }
    }
    return false;
}

/* Complex-DFT magnitude of a ci16 block at frequency f (Hz). */
static double dft_bin_mag(const iq_ci16_t *x, size_t n, double f, double fs)
{
    double re = 0.0;
    double im = 0.0;
    for (size_t t = 0; t < n; t++) {
        const double th = 2.0 * M_PI * f * (double)t / fs;
        const double c = cos(th);
        const double s = sin(th);
        re += (double)x[t].i * c + (double)x[t].q * s;
        im += (double)x[t].q * c - (double)x[t].i * s;
    }
    return hypot(re, im) / (double)n;
}

/* The old tests configured windows directly on receiver_config_t / ddc_config_t. The channel
 * model expresses the same windows as a receiver wrapping a single channel. */
static receiver_config_t window_receiver(uint64_t f_start, uint64_t f_stop, uint64_t bandwidth_hz, uint32_t sample_rate_hz, double scan_rate, double output_scale, double ref_dbm)
{
    receiver_config_t rx = {
        .id = 0,
        .frequency_min_hz = f_start,
        .frequency_max_hz = f_stop,
        .bandwidth_hz = bandwidth_hz,
        .scan_rate_hz_per_s = scan_rate,
        .output_scale = output_scale,
        .rf_reference_power_dbm = ref_dbm,
        .channel_count = 1,
    };
    rx.channels[0] = (channel_config_t){
        .id = 0,
        .track_tuner = true,
        .bandwidth_hz = (uint32_t)bandwidth_hz,
        .sample_rate_hz = sample_rate_hz,
        .output_scale = output_scale,
        .rf_reference_power_dbm = ref_dbm,
        .stream_enabled = true,
    };
    return rx;
}

/* Fixed-centre channel (the old ddc_config_t) inside a front end centred on it. */
static receiver_config_t fixed_channel_receiver(uint64_t center_hz, uint32_t bandwidth_hz, uint32_t sample_rate_hz, double output_scale, double ref_dbm)
{
    receiver_config_t rx = {
        .id = 0,
        .frequency_min_hz = center_hz - SIM_RECEIVER_BANDWIDTH_HZ / 2ULL,
        .frequency_max_hz = center_hz + SIM_RECEIVER_BANDWIDTH_HZ / 2ULL,
        .bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ,
        .output_scale = output_scale,
        .rf_reference_power_dbm = ref_dbm,
        .channel_count = 1,
    };
    rx.channels[0] = (channel_config_t){
        .id = 0,
        .center_frequency_hz = center_hz,
        .bandwidth_hz = bandwidth_hz,
        .sample_rate_hz = sample_rate_hz,
        .output_scale = output_scale,
        .rf_reference_power_dbm = ref_dbm,
        .stream_enabled = true,
    };
    return rx;
}

static bool render_rx_block(const scenario_t *scenario, const asset_cache_t *cache, const receiver_config_t *rx, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    return renderer_render_channel_block(scenario, cache, rx, &rx->channels[0], scenario_time_ns, out, count, stats);
}

/* Set up a single audio signal, pre-render it with `pp`, and render one output block at out_rate
 * (signal at window centre, offset 0). Frees the pre-render buffer before returning. */
static void render_audio_once(scenario_modulation_t mod, uint32_t audio_rate, const float *audio, size_t an,
                              uint32_t bw, double dev, const prerender_params_t *pp, uint32_t out_rate,
                              iq_ci16_t *out, size_t n)
{
    scenario_t sc;
    asset_cache_t cache;
    memset(&sc, 0, sizeof(sc));
    memset(&cache, 0, sizeof(cache));
    sc.schema_version = 1;
    sc.source_count = 1;
    sc.signal_count = 1;
    scenario_source_t *src = &sc.sources[0];
    snprintf(src->id, sizeof(src->id), "%s", "audio");
    src->source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    src->sample_rate_hz = audio_rate;
    src->bandwidth_hz = bw;
    src->sample_count = an;
    scenario_signal_t *g = &sc.signals[0];
    snprintf(g->signal_id, sizeof(g->signal_id), "%s", "sig");
    snprintf(g->source_reference, sizeof(g->source_reference), "%s", "audio");
    g->center_frequency_hz = 10000000000ULL;
    g->bandwidth_hz = bw;
    g->power_dbm = -40.0;
    g->modulation = mod;
    g->fm_deviation_hz = dev;
    g->am_depth = 0.8;
    g->repeat_interval_s = 1000.0;
    cache.asset_count = 1;
    snprintf(cache.assets[0].source_id, sizeof(cache.assets[0].source_id), "%s", "audio");
    cache.assets[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    cache.assets[0].sample_count = an;

    cached_prerender_t pr;
    char err[128];
    ck_assert_msg(asset_cache_prerender_from_audio(&pr, audio, an, audio_rate, g, pp, err, sizeof(err)), "%s", err);
    cache.prerenders[0] = pr;

    const receiver_config_t rx = window_receiver(10000000000ULL - out_rate / 2, 10000000000ULL + out_rate / 2, out_rate, out_rate, 0.0, 1.0, -40.0);
    render_stats_t st;
    ck_assert(render_rx_block(&sc, &cache, &rx, 0, out, n, &st));
    ck_assert_uint_eq(st.active_signals, 1);
    free(pr.samples);
}

static void setup_two_signal_mix(scenario_t *scenario, asset_cache_t *cache, iq_ci16_t *asset_a, iq_ci16_t *asset_b, int16_t a_i, int16_t a_q, int16_t b_i, int16_t b_q)
{
    memset(scenario, 0, sizeof(*scenario));
    memset(cache, 0, sizeof(*cache));
    scenario->schema_version = 1;
    snprintf(scenario->scenario_id, sizeof(scenario->scenario_id), "%s", "unit_mix");
    scenario->source_count = 2;
    scenario->signal_count = 2;
    cache->asset_count = 2;

    const char *ids[] = {"src_a", "src_b"};
    iq_ci16_t *assets[] = {asset_a, asset_b};
    const int16_t i_values[] = {a_i, b_i};
    const int16_t q_values[] = {a_q, b_q};

    for (size_t index = 0; index < 2; index++) {
        for (size_t sample = 0; sample < 8; sample++) {
            assets[index][sample] = (iq_ci16_t){.i = i_values[index], .q = q_values[index]};
        }

        scenario_source_t *source = &scenario->sources[index];
        snprintf(source->id, sizeof(source->id), "%s", ids[index]);
        snprintf(source->source_type, sizeof(source->source_type), "%s", "iq_file");
        snprintf(source->format, sizeof(source->format), "%s", "ci16");
        snprintf(source->byte_order, sizeof(source->byte_order), "%s", "little_endian");
        snprintf(source->iq_layout, sizeof(source->iq_layout), "%s", "interleaved_iq");
        source->sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ;
        source->bandwidth_hz = 1000;
        source->sample_count = 8;
        source->nominal_level_dbfs = 0.0;

        scenario_signal_t *signal = &scenario->signals[index];
        snprintf(signal->signal_id, sizeof(signal->signal_id), "sig_%zu", index);
        snprintf(signal->source_reference, sizeof(signal->source_reference), "%s", ids[index]);
        signal->center_frequency_hz = 10000000000ULL;
        signal->bandwidth_hz = 1000;
        signal->power_dbm = -40.0;
        signal->start_time_s = 0.0;
        signal->repeat_interval_s = 1.0;

        cached_asset_t *asset = &cache->assets[index];
        snprintf(asset->source_id, sizeof(asset->source_id), "%s", ids[index]);
        asset->sample_count = 8;
        asset->samples = assets[index];
    }
}

static receiver_config_t fixed_center_receiver(void)
{
    return window_receiver(9960000000ULL, 10040000000ULL, SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 0.0, 1.0, -40.0);
}

static void setup_constant_signal(scenario_t *scenario, asset_cache_t *cache, iq_ci16_t *asset_samples, uint64_t signal_center_hz)
{
    memset(scenario, 0, sizeof(*scenario));
    memset(cache, 0, sizeof(*cache));
    scenario->schema_version = 1;
    snprintf(scenario->scenario_id, sizeof(scenario->scenario_id), "%s", "unit_scan");
    scenario->source_count = 1;
    scenario->signal_count = 1;
    for (size_t sample = 0; sample < 8; sample++) {
        asset_samples[sample] = (iq_ci16_t){.i = 1000, .q = 0};
    }

    scenario_source_t *source = &scenario->sources[0];
    snprintf(source->id, sizeof(source->id), "%s", "scan_src");
    snprintf(source->source_type, sizeof(source->source_type), "%s", "iq_file");
    snprintf(source->format, sizeof(source->format), "%s", "ci16");
    snprintf(source->byte_order, sizeof(source->byte_order), "%s", "little_endian");
    snprintf(source->iq_layout, sizeof(source->iq_layout), "%s", "interleaved_iq");
    source->sample_rate_hz = 1000;
    source->bandwidth_hz = 1000;
    source->sample_count = 8;
    source->nominal_level_dbfs = 0.0;

    scenario_signal_t *signal = &scenario->signals[0];
    snprintf(signal->signal_id, sizeof(signal->signal_id), "%s", "scan_sig");
    snprintf(signal->source_reference, sizeof(signal->source_reference), "%s", "scan_src");
    signal->center_frequency_hz = signal_center_hz;
    signal->bandwidth_hz = 1000;
    signal->power_dbm = -40.0;
    signal->start_time_s = 0.0;
    signal->repeat_interval_s = 1.0;

    cache->asset_count = 1;
    snprintf(cache->assets[0].source_id, sizeof(cache->assets[0].source_id), "%s", "scan_src");
    cache->assets[0].sample_count = 8;
    cache->assets[0].samples = asset_samples;
}

static receiver_config_t scanner_receiver(void)
{
    return window_receiver(9960000000ULL, 10060000000ULL, SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 100000000000.0, 1.0, -40.0);
}

START_TEST(renders_nonzero_visible_signal)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("simulator/configs/receiver_scanner.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    iq_ci16_t out[128];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 128, &stats));
    ck_assert_uint_eq(stats.samples_rendered, 128);
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_ne(out[0].i, 0);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renderer_80mhz_sinc_resamples_24576_source_to_98304_output)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("simulator/configs/receiver_scanner.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);
    config.receivers[0].frequency_min_hz = 9965000000ULL;
    config.receivers[0].frequency_max_hz = 10045000000ULL;

    iq_ci16_t out[8];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 8, &stats));
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    const double gain = 1.0;
    const size_t offset = 11059;
    /* Playback now starts at the exact fractional sample: 450000 ns * 24576000 Hz = 11059.2,
     * so the resampler samples at fraction + i*0.25 instead of snapping to sample 11059. */
    const double base_s = 450000.0 / 1000000000.0;
    const double exact = base_s * 24576000.0;
    const double fraction = exact - floor(exact);
    const double source_per_output = 24576000.0 / 98304000.0;
    /* The renderer now filters with RESAMPLER_RADIUS samples of history before the block's
     * first source sample (interior kernel), so mirror that in the reference. */
    for (size_t i = 0; i < 5; i++) {
        const double pos = 4.0 + fraction + (double)i * source_per_output;
        ck_assert_int_le(abs(out[i].i - sinc_scaled_i16(&asset->samples[offset - 4], 16, pos, false, gain)), 2);
        ck_assert_int_le(abs(out[i].q - sinc_scaled_i16(&asset->samples[offset - 4], 16, pos, true, gain)), 2);
    }
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renderer_low_rate_upsample_uses_linear_path)
{
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_samples[4] = {
        {.i = 0, .q = 1600},
        {.i = 1600, .q = 0},
        {.i = 3200, .q = -1600},
        {.i = 4800, .q = -3200},
    };
    memset(&scenario, 0, sizeof(scenario));
    memset(&cache, 0, sizeof(cache));

    scenario.schema_version = 1;
    snprintf(scenario.scenario_id, sizeof(scenario.scenario_id), "%s", "unit_low_rate_linear");
    scenario.source_count = 1;
    scenario.signal_count = 1;
    snprintf(scenario.sources[0].id, sizeof(scenario.sources[0].id), "%s", "low_src");
    snprintf(scenario.sources[0].source_type, sizeof(scenario.sources[0].source_type), "%s", "iq_file");
    snprintf(scenario.sources[0].format, sizeof(scenario.sources[0].format), "%s", "ci16");
    snprintf(scenario.sources[0].byte_order, sizeof(scenario.sources[0].byte_order), "%s", "little_endian");
    snprintf(scenario.sources[0].iq_layout, sizeof(scenario.sources[0].iq_layout), "%s", "interleaved_iq");
    scenario.sources[0].sample_rate_hz = 1000;
    /* Occupied bandwidth well under source_rate / LINEAR_MIN_SOURCE_OVERSAMPLE, so this source
     * qualifies for the linear upsampling shortcut this test exercises. */
    scenario.sources[0].bandwidth_hz = 100;
    scenario.sources[0].sample_count = 4;
    snprintf(scenario.signals[0].signal_id, sizeof(scenario.signals[0].signal_id), "%s", "low_sig");
    snprintf(scenario.signals[0].source_reference, sizeof(scenario.signals[0].source_reference), "%s", "low_src");
    scenario.signals[0].center_frequency_hz = 10000000000ULL;
    scenario.signals[0].bandwidth_hz = 100;
    scenario.signals[0].power_dbm = -40.0;
    scenario.signals[0].start_time_s = 0.0;
    scenario.signals[0].repeat_interval_s = 1.0;

    cache.asset_count = 1;
    snprintf(cache.assets[0].source_id, sizeof(cache.assets[0].source_id), "%s", "low_src");
    cache.assets[0].sample_count = 4;
    cache.assets[0].samples = asset_samples;

    const receiver_config_t receiver = window_receiver(9999999500ULL, 10000000500ULL, 1000, 16000, 0.0, 1.0, -40.0);
    iq_ci16_t out[16];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 16, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    for (size_t i = 0; i < 16; i++) {
        const double fraction = (double)i / 16.0;
        ck_assert_int_eq(out[i].i, interpolate_scaled_i16(asset_samples[0].i, asset_samples[1].i, fraction, 1.0));
        ck_assert_int_eq(out[i].q, interpolate_scaled_i16(asset_samples[0].q, asset_samples[1].q, fraction, 1.0));
    }
}
END_TEST

START_TEST(renderer_applies_rf_power_relative_to_reference)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("simulator/configs/receiver_scanner.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    scenario.signals[0].power_dbm = -49.0;
    config.receivers[0].channels[0].rf_reference_power_dbm = -55.0;
    config.receivers[0].frequency_min_hz = 9965000000ULL;
    config.receivers[0].frequency_max_hz = 10045000000ULL;

    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);
    iq_ci16_t out[4];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 4, &stats));
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    const double gain = pow(10.0, 6.0 / 20.0);
    /* +6 dB relative to the reference. Playback starts at the fractional sample 11059.2. */
    const double base_s = 450000.0 / 1000000000.0;
    const double exact = base_s * 24576000.0;
    const double fraction = exact - floor(exact);
    ck_assert_int_le(abs(out[0].i - sinc_scaled_i16(&asset->samples[11055], 16, 4.0 + fraction, false, gain)), 2);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renderer_mixes_two_signals_without_clipping)
{
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_a[8];
    iq_ci16_t asset_b[8];
    setup_two_signal_mix(&scenario, &cache, asset_a, asset_b, 1000, 100, 2000, -400);

    const receiver_config_t receiver = fixed_center_receiver();
    iq_ci16_t out[4];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 4, &stats));

    ck_assert_uint_eq(stats.samples_rendered, 4);
    ck_assert_uint_eq(stats.active_signals, 2);
    for (size_t index = 0; index < 4; index++) {
        ck_assert_int_eq(out[index].i, 3000);
        ck_assert_int_eq(out[index].q, -300);
    }
}
END_TEST

START_TEST(renderer_clips_after_mixing_two_signals)
{
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_a[8];
    iq_ci16_t asset_b[8];
    setup_two_signal_mix(&scenario, &cache, asset_a, asset_b, 30000, -30000, 30000, -30000);

    const receiver_config_t receiver = fixed_center_receiver();
    iq_ci16_t out[4];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 4, &stats));

    ck_assert_uint_eq(stats.samples_rendered, 4);
    ck_assert_uint_eq(stats.active_signals, 2);
    for (size_t index = 0; index < 4; index++) {
        ck_assert_int_eq(out[index].i, 32767);
        ck_assert_int_eq(out[index].q, -32768);
    }
}
END_TEST

START_TEST(renderer_mix_bus_clips_once_not_per_signal)
{
    /* Three co-located signals summing to +30000 I / -30000 Q. The running sum crosses full
     * scale mid-way (+25000 +25000 = +50000), which the old per-signal clipping would have
     * saturated to 32767 before subtracting the third signal, giving an order-dependent 12767.
     * With a float mix bus clipped once, the result is the true sum, saturated only at the end. */
    scenario_t scenario;
    asset_cache_t cache;
    memset(&scenario, 0, sizeof(scenario));
    memset(&cache, 0, sizeof(cache));
    scenario.schema_version = 1;
    snprintf(scenario.scenario_id, sizeof(scenario.scenario_id), "%s", "unit_mix3");
    scenario.source_count = 3;
    scenario.signal_count = 3;
    cache.asset_count = 3;
    static iq_ci16_t assets[3][8];
    const int16_t iv[3] = {25000, 25000, -20000};
    const int16_t qv[3] = {-25000, -25000, 20000};
    for (size_t n = 0; n < 3; n++) {
        for (size_t s = 0; s < 8; s++) {
            assets[n][s] = (iq_ci16_t){.i = iv[n], .q = qv[n]};
        }
        scenario_source_t *src = &scenario.sources[n];
        snprintf(src->id, sizeof(src->id), "s%zu", n);
        snprintf(src->source_type, sizeof(src->source_type), "%s", "iq_file");
        snprintf(src->format, sizeof(src->format), "%s", "ci16");
        snprintf(src->byte_order, sizeof(src->byte_order), "%s", "little_endian");
        snprintf(src->iq_layout, sizeof(src->iq_layout), "%s", "interleaved_iq");
        src->sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ;
        src->bandwidth_hz = 1000;
        src->sample_count = 8;
        scenario_signal_t *sig = &scenario.signals[n];
        snprintf(sig->signal_id, sizeof(sig->signal_id), "sig%zu", n);
        snprintf(sig->source_reference, sizeof(sig->source_reference), "s%zu", n);
        sig->center_frequency_hz = 10000000000ULL;
        sig->bandwidth_hz = 1000;
        sig->power_dbm = -40.0;
        sig->repeat_interval_s = 1.0;
        snprintf(cache.assets[n].source_id, sizeof(cache.assets[n].source_id), "s%zu", n);
        cache.assets[n].sample_count = 8;
        cache.assets[n].samples = assets[n];
    }

    const receiver_config_t receiver = fixed_center_receiver();
    iq_ci16_t out[4];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 3);
    for (size_t i = 0; i < 4; i++) {
        ck_assert_int_eq(out[i].i, 30000);
        ck_assert_int_eq(out[i].q, -30000);
    }
}
END_TEST

START_TEST(scanner_moves_fixed_rf_signal_through_baseband)
{
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_samples[8];
    setup_constant_signal(&scenario, &cache, asset_samples, 9980000000ULL);

    const receiver_config_t receiver = scanner_receiver();
    iq_ci16_t positive_offset[4];
    iq_ci16_t zero_offset[4];
    iq_ci16_t negative_offset[4];
    render_stats_t stats;

    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, positive_offset, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 200000ULL, zero_offset, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 400000ULL, negative_offset, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);

    /* The mixer phase now derives from absolute time, so a single sample's sign depends on the
     * (nonzero) starting phase. The rotation *direction* is what encodes the offset sign, and it
     * is phase-independent: for a tone A*exp(j(phi0 + i*w)), the discriminant
     * out[0].i*out[1].q - out[0].q*out[1].i == A^2*sin(w) has the sign of the frequency offset. */
    const long pos_disc = (long)positive_offset[0].i * positive_offset[1].q - (long)positive_offset[0].q * positive_offset[1].i;
    const long neg_disc = (long)negative_offset[0].i * negative_offset[1].q - (long)negative_offset[0].q * negative_offset[1].i;
    ck_assert_int_gt(pos_disc, 0);        /* signal above centre -> positive offset */
    ck_assert_int_eq(zero_offset[1].q, 0); /* centre passes over the signal -> no rotation */
    ck_assert_int_lt(neg_disc, 0);        /* signal below centre -> negative offset */
}
END_TEST

START_TEST(scanner_same_time_is_deterministic_across_instances)
{
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_samples[8];
    setup_constant_signal(&scenario, &cache, asset_samples, 9980000000ULL);

    receiver_config_t receiver_a = scanner_receiver();
    receiver_config_t receiver_b = scanner_receiver();
    receiver_b.id = 7;
    receiver_b.rest_port = 9100;
    receiver_b.channels[0].udp_output.port = 55000;

    iq_ci16_t out_a[8];
    iq_ci16_t out_b[8];
    render_stats_t stats_a;
    render_stats_t stats_b;
    ck_assert(render_rx_block(&scenario, &cache, &receiver_a, 400000ULL, out_a, 8, &stats_a));
    ck_assert(render_rx_block(&scenario, &cache, &receiver_b, 400000ULL, out_b, 8, &stats_b));

    ck_assert_uint_eq(stats_a.active_signals, 1);
    ck_assert_uint_eq(stats_b.active_signals, 1);
    ck_assert_mem_eq(out_a, out_b, sizeof(out_a));
}
END_TEST

START_TEST(renders_ddc_nonzero_visible_signal)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    const receiver_config_t rx = fixed_channel_receiver(10005000000ULL, SIM_DDC_BANDWIDTH_HZ, SIM_DDC_SAMPLE_RATE_HZ, 1.0, -55.0);
    iq_ci16_t out[128];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &rx, 450000ULL, out, 128, &stats));
    ck_assert_uint_eq(stats.samples_rendered, 128);
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_ne(out[0].i, 0);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renders_ddc_4096_sample_block_for_waterfall)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    const receiver_config_t rx = fixed_channel_receiver(10005000000ULL, SIM_DDC_BANDWIDTH_HZ, SIM_DDC_SAMPLE_RATE_HZ, 1.0, -55.0);
    iq_ci16_t out[4096];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &rx, 450000ULL, out, 4096, &stats));
    ck_assert_uint_eq(stats.samples_rendered, 4096);
    ck_assert_uint_eq(stats.active_signals, 1);

    bool any_nonzero = false;
    for (size_t i = 0; i < 4096; i++) {
        if (out[i].i != 0 || out[i].q != 0) {
            any_nonzero = true;
            break;
        }
    }
    ck_assert(any_nonzero);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renderer_adds_deterministic_noise_floor_without_active_signals)
{
    scenario_t scenario;
    memset(&scenario, 0, sizeof(scenario));
    scenario.schema_version = 1;
    snprintf(scenario.scenario_id, sizeof(scenario.scenario_id), "%s", "unit_noise_floor");
    scenario.noise_floor.enabled = true;
    /* Density chosen so the total in the 80 MHz window (density + 10*log10(bw)) is ~-85 dBm,
     * matching the receiver reference below -- present, deterministic, not fully saturated. */
    scenario.noise_floor.power_dbm_per_hz = -164.0;
    scenario.noise_floor.seed = 1234;

    asset_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    receiver_config_t receiver = fixed_center_receiver();
    receiver.channels[0].rf_reference_power_dbm = -85.0;

    iq_ci16_t out_a[32];
    iq_ci16_t out_b[32];
    iq_ci16_t out_c[32];
    render_stats_t stats;

    ck_assert(render_rx_block(&scenario, &cache, &receiver, 1000000ULL, out_a, 32, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert_uint_eq(stats.samples_rendered, 32);
    ck_assert(samples_have_energy(out_a, 32));

    ck_assert(render_rx_block(&scenario, &cache, &receiver, 1000000ULL, out_b, 32, &stats));
    ck_assert_mem_eq(out_a, out_b, sizeof(out_a));

    ck_assert(render_rx_block(&scenario, &cache, &receiver, 2000000ULL, out_c, 32, &stats));
    ck_assert(memcmp(out_a, out_c, sizeof(out_a)) != 0);
}
END_TEST

static double measure_noise_rms(uint64_t window_bw_hz, double density_dbm_per_hz, double ref_dbm)
{
    scenario_t scenario;
    memset(&scenario, 0, sizeof(scenario));
    scenario.schema_version = 1;
    scenario.noise_floor.enabled = true;
    scenario.noise_floor.power_dbm_per_hz = density_dbm_per_hz;
    scenario.noise_floor.seed = 42;
    asset_cache_t cache;
    memset(&cache, 0, sizeof(cache));

    const receiver_config_t rx = fixed_channel_receiver(10000000000ULL, (uint32_t)window_bw_hz, 4000000U, 1.0, ref_dbm);
    /* Average power over several blocks (different pool slices) to shrink statistical error. */
    double power = 0.0;
    size_t total = 0;
    iq_ci16_t out[2048];
    render_stats_t stats;
    for (uint64_t b = 0; b < 16; b++) {
        const uint64_t t = 1000000ULL + b * 700000ULL;
        ck_assert(render_rx_block(&scenario, &cache, &rx, t, out, 2048, &stats));
        for (size_t i = 0; i < 2048; i++) {
            power += (double)out[i].i * out[i].i + (double)out[i].q * out[i].q;
            total++;
        }
    }
    return sqrt(power / (double)total / 2.0); /* per-component RMS */
}

START_TEST(renderer_noise_scales_with_window_bandwidth_as_density)
{
    /* Noise specified as a density: total power = density + 10*log10(bandwidth), so the RMS must
     * track the window bandwidth. A 1 MHz window carries 10*log10(1e6/25e3) ~= 16 dB more noise
     * power (a factor sqrt(40) ~= 6.32 in amplitude) than a 25 kHz window. */
    const double ref = -70.0;
    const double density = -140.0;
    const double rms_wide = measure_noise_rms(1000000ULL, density, ref);
    const double rms_narrow = measure_noise_rms(25000ULL, density, ref);

    const double expected_amp_wide = 32767.0 * pow(10.0, (density + 10.0 * log10(1000000.0) - ref) / 20.0);
    const double expected_amp_narrow = 32767.0 * pow(10.0, (density + 10.0 * log10(25000.0) - ref) / 20.0);

    /* Absolute level within 0.3 dB, and the wide/narrow ratio matches sqrt(bw ratio). */
    ck_assert(fabs(20.0 * log10(rms_wide / expected_amp_wide)) < 0.3);
    ck_assert(fabs(20.0 * log10(rms_narrow / expected_amp_narrow)) < 0.3);
    ck_assert(fabs(20.0 * log10((rms_wide / rms_narrow) / sqrt(1000000.0 / 25000.0))) < 0.3);
}
END_TEST

START_TEST(renderer_renders_audio_modulation_modes)
{
    /* Audio signals are pre-rendered to complex baseband at load (T3) and rendered through the
     * IQ dispatch (T4). Build the pre-render for each modulation via the real engine and confirm
     * the renderer emits energy for all four modes through the new path. */
    scenario_t scenario;
    asset_cache_t cache;
    float audio[512];
    memset(&scenario, 0, sizeof(scenario));
    memset(&cache, 0, sizeof(cache));

    scenario.schema_version = 1;
    snprintf(scenario.scenario_id, sizeof(scenario.scenario_id), "%s", "unit_audio_mods");
    scenario.source_count = 1;
    scenario.signal_count = 1;
    snprintf(scenario.sources[0].id, sizeof(scenario.sources[0].id), "%s", "audio");
    snprintf(scenario.sources[0].source_type, sizeof(scenario.sources[0].source_type), "%s", "audio_file");
    scenario.sources[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    snprintf(scenario.sources[0].format, sizeof(scenario.sources[0].format), "%s", "wav");
    scenario.sources[0].sample_rate_hz = 48000;
    scenario.sources[0].bandwidth_hz = 200000;
    scenario.sources[0].sample_count = 512;

    snprintf(scenario.signals[0].signal_id, sizeof(scenario.signals[0].signal_id), "%s", "audio_sig");
    snprintf(scenario.signals[0].source_reference, sizeof(scenario.signals[0].source_reference), "%s", "audio");
    scenario.signals[0].center_frequency_hz = 10000000000ULL;
    scenario.signals[0].bandwidth_hz = 200000;
    scenario.signals[0].power_dbm = -80.0;
    scenario.signals[0].fm_deviation_hz = 75000.0;
    scenario.signals[0].am_depth = 0.8;
    scenario.signals[0].start_time_s = 0.0;
    scenario.signals[0].repeat_interval_s = 1.0;

    for (size_t i = 0; i < 512; i++) {
        audio[i] = (float)sin(2.0 * M_PI * (double)i / 32.0);
    }
    cache.asset_count = 1;
    snprintf(cache.assets[0].source_id, sizeof(cache.assets[0].source_id), "%s", "audio");
    cache.assets[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    cache.assets[0].sample_count = 512;
    cache.assets[0].audio_samples = audio;

    receiver_config_t receiver = fixed_center_receiver();
    receiver.channels[0].rf_reference_power_dbm = -40.0;
    iq_ci16_t out[64];
    render_stats_t stats;
    char error[128];
    const scenario_modulation_t modes[] = {
        SCENARIO_MODULATION_WBFM,
        SCENARIO_MODULATION_AM,
        SCENARIO_MODULATION_USB,
        SCENARIO_MODULATION_LSB,
    };
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        memset(out, 0, sizeof(out));
        scenario.signals[0].modulation = modes[m];
        cached_prerender_t pr;
        ck_assert_msg(asset_cache_prerender_from_audio(&pr, audio, 512, 48000, &scenario.signals[0], NULL, error, sizeof(error)), "%s", error);
        cache.prerenders[0] = pr;
        ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 64, &stats));
        ck_assert_uint_eq(stats.active_signals, 1);
        ck_assert(samples_have_energy(out, 64));
        free(pr.samples);
        cache.prerenders[0].valid = false;
    }
}
END_TEST

/* Build a DC-audio AM/SSB signal and its pre-render (via the real engine) so the renderer sees it
 * exactly as it would after asset_cache_load. audio_rate sets the source rate; the pre-render rate
 * is then derived from it and stored in cache->prerenders[0]. Caller frees cache->prerenders[0].samples. */
static void setup_dc_audio_signal(scenario_t *scenario, asset_cache_t *cache, float *audio, size_t n,
                                  float dc, scenario_modulation_t mod, uint32_t audio_rate, uint32_t signal_bw)
{
    memset(scenario, 0, sizeof(*scenario));
    memset(cache, 0, sizeof(*cache));
    for (size_t i = 0; i < n; i++) {
        audio[i] = dc;
    }
    scenario->schema_version = 1;
    scenario->source_count = 1;
    scenario->signal_count = 1;
    scenario_source_t *src = &scenario->sources[0];
    snprintf(src->id, sizeof(src->id), "%s", "audio");
    snprintf(src->source_type, sizeof(src->source_type), "%s", "audio_file");
    src->source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    snprintf(src->format, sizeof(src->format), "%s", "wav");
    src->sample_rate_hz = audio_rate;
    src->bandwidth_hz = signal_bw;
    src->sample_count = n;
    scenario_signal_t *sig = &scenario->signals[0];
    snprintf(sig->signal_id, sizeof(sig->signal_id), "%s", "audio_sig");
    snprintf(sig->source_reference, sizeof(sig->source_reference), "%s", "audio");
    sig->center_frequency_hz = 10000000000ULL; /* offset 0 for the fixed-centre receiver */
    sig->bandwidth_hz = signal_bw;
    sig->power_dbm = -40.0;
    sig->am_depth = 0.8;
    sig->modulation = mod;
    sig->repeat_interval_s = 1.0;
    cache->asset_count = 1;
    snprintf(cache->assets[0].source_id, sizeof(cache->assets[0].source_id), "%s", "audio");
    cache->assets[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    cache->assets[0].sample_count = n;
    cache->assets[0].audio_samples = audio;

    cached_prerender_t pr;
    char error[128];
    ck_assert_msg(asset_cache_prerender_from_audio(&pr, audio, n, audio_rate, sig, NULL, error, sizeof(error)), "%s", error);
    cache->prerenders[0] = pr;
}

START_TEST(renderer_am_envelope_is_normalised_and_does_not_over_saturate)
{
    /* DC audio 0.5 at depth 0.8: normalised envelope = (1 + 0.8*0.5)/(1+0.8) = 0.7778, so with
     * unity gain the output is ~25485, not the old (1 + 0.4) = 1.4x that clipped to 32767. The
     * pre-render peak-normalises the constant envelope to 32767 and folds 0.7778 into the gain,
     * so the calibrated output is unchanged. */
    scenario_t scenario;
    asset_cache_t cache;
    float audio[64];
    setup_dc_audio_signal(&scenario, &cache, audio, 64, 0.5f, SCENARIO_MODULATION_AM, 48000, 10000);
    const receiver_config_t receiver = fixed_center_receiver();
    iq_ci16_t out[8];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 8, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    const int expected = (int)lrint(32767.0 * (1.0 + 0.8 * 0.5) / (1.0 + 0.8));
    ck_assert_int_ne(out[0].i, 32767);
    ck_assert_int_le(abs(out[0].i - expected), 2);
    free(cache.prerenders[0].samples);
}
END_TEST

START_TEST(renderer_audio_stops_at_end_of_asset_within_block)
{
    /* Pre-render rate == output rate (AM at 48 kHz audio -> 96 kHz pre-render, rendered at 96 kHz):
     * a 2-sample DC clip pre-renders to 4 samples, so a block of 8 renders samples 0..3 and leaves
     * 4..7 at zero instead of holding a dead carrier. */
    scenario_t scenario;
    asset_cache_t cache;
    float audio[2];
    setup_dc_audio_signal(&scenario, &cache, audio, 2, 1.0f, SCENARIO_MODULATION_AM, 48000, 10000);
    ck_assert_uint_eq(cache.prerenders[0].sample_rate_hz, 96000U);
    ck_assert_uint_eq(cache.prerenders[0].sample_count, 4U);
    const receiver_config_t receiver = window_receiver(9999900000ULL, 10000100000ULL, 200000, 96000, 0.0, 1.0, -40.0); /* rate == pre-render rate: integer-aligned direct path */
    iq_ci16_t out[8];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 8, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    for (size_t i = 0; i < 4; i++) {
        ck_assert_int_ne(out[i].i, 0);
    }
    for (size_t i = 4; i < 8; i++) {
        ck_assert_int_eq(out[i].i, 0);
        ck_assert_int_eq(out[i].q, 0);
    }
    free(cache.prerenders[0].samples);
}
END_TEST

START_TEST(renders_burst_scenario_only_during_active_second)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/burst_1s_every_5s.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    const receiver_config_t rx = fixed_channel_receiver(10005000000ULL, SIM_DDC_BANDWIDTH_HZ, SIM_DDC_SAMPLE_RATE_HZ, 1.0, -55.0);
    iq_ci16_t active_a[1024];
    iq_ci16_t inactive[1024];
    iq_ci16_t active_b[1024];
    render_stats_t stats;

    ck_assert(render_rx_block(&scenario, &cache, &rx, 500000000ULL, active_a, 1024, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 1500000000ULL, inactive, 1024, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 5500000000ULL, active_b, 1024, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);

    ck_assert(samples_have_energy(active_a, 1024));
    ck_assert(samples_have_energy(inactive, 1024));
    ck_assert(samples_have_energy(active_b, 1024));
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renderer_applies_window_passband_gain)
{
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_samples[8];
    setup_constant_signal(&scenario, &cache, asset_samples, 10000000000ULL);
    scenario.signals[0].bandwidth_hz = 2000000;
    scenario.sources[0].sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ;

    const receiver_config_t receiver = fixed_center_receiver();
    render_stats_t stats;
    iq_ci16_t out[4];

    scenario.signals[0].center_frequency_hz = 10039000000ULL;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_eq(out[0].i, 1000);

    scenario.signals[0].center_frequency_hz = 10040500000ULL;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_eq(out[0].i, 500);

    scenario.signals[0].center_frequency_hz = 10041500000ULL;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert_int_eq(out[0].i, 0);
    ck_assert_int_eq(out[0].q, 0);
}
END_TEST

START_TEST(nco_phase_is_continuous_across_block_boundaries)
{
    /* Render a constant IQ source (same rate, nonzero offset -> pure frequency shift) as two
     * consecutive grid-aligned blocks, and as one double-length block. The second block must
     * continue the phase of the first with no boundary jump: block1[j] == combined[64 + j]. */
    scenario_t scenario;
    asset_cache_t cache;
    static iq_ci16_t asset_samples[256];
    for (size_t s = 0; s < 256; s++) {
        asset_samples[s] = (iq_ci16_t){.i = 1000, .q = 0};
    }
    setup_constant_signal(&scenario, &cache, asset_samples, 10000000000ULL);
    /* Longer constant source so the signal stays active across both blocks (same rate,
     * nonzero offset -> pure frequency shift), isolating the mixer phase. */
    scenario.sources[0].sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ;
    scenario.sources[0].sample_count = 256;
    scenario.signals[0].center_frequency_hz = 10001234500ULL; /* non-block-periodic offset */
    scenario.signals[0].bandwidth_hz = 200000;
    cache.assets[0].sample_count = 256;

    const receiver_config_t receiver = fixed_center_receiver();
    const uint32_t rate = receiver.channels[0].sample_rate_hz;
    const uint64_t t0 = streamer_block_start_ns(0, 64, rate);
    const uint64_t t1 = streamer_block_start_ns(1, 64, rate);

    iq_ci16_t block0[64];
    iq_ci16_t block1[64];
    iq_ci16_t combined[128];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, t0, block0, 64, &stats));
    ck_assert(render_rx_block(&scenario, &cache, &receiver, t1, block1, 64, &stats));
    ck_assert(render_rx_block(&scenario, &cache, &receiver, t0, combined, 128, &stats));

    for (size_t j = 0; j < 64; j++) {
        ck_assert_int_eq(block0[j].i, combined[j].i);
        ck_assert_int_eq(block0[j].q, combined[j].q);
    }
    for (size_t j = 0; j < 64; j++) {
        /* Constant source, so amplitude is identical; only the continuous phase carries over.
         * Allow +/-1 LSB for the independent float rounding of the two renders. */
        ck_assert_int_le(abs(block1[j].i - combined[64 + j].i), 1);
        ck_assert_int_le(abs(block1[j].q - combined[64 + j].q), 1);
    }
}
END_TEST

static double render_decimated_tone_power(double tone_cycles_per_source_sample)
{
    /* Source at 4x the output rate carrying a complex tone; render at offset 0 (decimate by 4)
     * and return the mean output power. A tone below the output Nyquist should pass; one above
     * it must be attenuated by the anti-alias kernel instead of folding back at full power. */
    enum { SRC = 2048, OUT = 256 };
    static iq_ci16_t src[SRC];
    for (size_t n = 0; n < SRC; n++) {
        const double phase = 2.0 * M_PI * tone_cycles_per_source_sample * (double)n;
        src[n].i = (int16_t)lrint(10000.0 * cos(phase));
        src[n].q = (int16_t)lrint(10000.0 * sin(phase));
    }
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t dummy[8];
    setup_constant_signal(&scenario, &cache, dummy, 10000000000ULL);
    scenario.sources[0].sample_rate_hz = 4000000;
    scenario.sources[0].sample_count = SRC;
    scenario.signals[0].bandwidth_hz = 2000000;
    cache.assets[0].sample_count = SRC;
    cache.assets[0].samples = src;

    const receiver_config_t receiver = window_receiver(9999500000ULL, 10000500000ULL, 1000000, 1000000, 0.0, 1.0, -40.0); /* source_per_output = 4 */
    iq_ci16_t out[OUT];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &receiver, 0, out, OUT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    double power = 0.0;
    for (size_t i = 0; i < OUT; i++) {
        power += (double)out[i].i * out[i].i + (double)out[i].q * out[i].q;
    }
    return power / (double)OUT;
}

START_TEST(renderer_decimation_attenuates_out_of_band_tone)
{
    /* Output Nyquist is 0.125 cycles/source-sample. 0.03 is in band; 0.4 is far out of band. */
    const double in_band = render_decimated_tone_power(0.03);
    const double out_of_band = render_decimated_tone_power(0.4);
    ck_assert(in_band > 1.0e6); /* the passband tone survives with substantial power */
    /* Without the decimation-aware cutoff the out-of-band tone would alias back at nearly full
     * power (ratio ~1). The band-limited kernel must push it well down. */
    ck_assert(out_of_band < in_band * 0.05);
}
END_TEST

START_TEST(renderer_skips_signal_beyond_output_nyquist)
{
    /* Wide window (fits the signal, so passband gain > 0) but a low output rate: a signal
     * 10 MHz off centre is well inside the 80 MHz window yet far beyond the +/-1 MHz output
     * Nyquist, so it must be skipped entirely rather than aliased into the band. */
    scenario_t scenario;
    asset_cache_t cache;
    iq_ci16_t asset_samples[8];
    setup_constant_signal(&scenario, &cache, asset_samples, 10010000000ULL);
    scenario.sources[0].sample_rate_hz = 2000000;
    scenario.signals[0].bandwidth_hz = 100000;

    const receiver_config_t rx = fixed_channel_receiver(10000000000ULL, 80000000U /* window wider than the output rate */, 2000000U /* Nyquist = 1 MHz */, 1.0, -40.0);
    iq_ci16_t out[16];
    render_stats_t stats;
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, 16, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    for (size_t i = 0; i < 16; i++) {
        ck_assert_int_eq(out[i].i, 0);
        ck_assert_int_eq(out[i].q, 0);
    }

    /* A signal only 0.3 MHz off centre is within Nyquist and still rendered. */
    scenario.signals[0].center_frequency_hz = 10000300000ULL;
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, 16, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
}
END_TEST

/* Largest magnitude in the spectrum outside +/-1.5 bins of any listed signal frequency, in dBc
 * relative to peak_ref. Used to bound resampler images/quantisation spurs in the parity tests. */
static double max_spur_dbc(const iq_ci16_t *x, size_t n, uint32_t fs, const double *sig_freqs, size_t nsig, double peak_ref)
{
    const double bin_hz = (double)fs / (double)n;
    double worst = 0.0;
    for (size_t k = 0; k < n; k++) {
        double f = (double)k * bin_hz;
        if (f > (double)fs / 2.0) {
            f -= (double)fs;
        }
        bool is_signal = false;
        for (size_t j = 0; j < nsig; j++) {
            if (fabs(f - sig_freqs[j]) < 1.5 * bin_hz) {
                is_signal = true;
                break;
            }
        }
        if (is_signal) {
            continue;
        }
        const double m = dft_bin_mag(x, n, f, fs);
        if (m > worst) {
            worst = m;
        }
    }
    return 20.0 * log10(worst / peak_ref);
}

START_TEST(renderer_prerender_am_matches_reference_synthesis)
{
    /* AT2 parity: render an AM tone via the low-rate pre-render (oversample 2) and via a
     * pre-render forced to the output rate (== full-rate reference synthesis), then compare
     * spectra. Carrier and both sidebands within 0.5 dB; no spur above -38 dBc (the 2x-oversample
     * resampler image floor). */
    enum { OUT = 480000, AR = 48000, AN = 4096, N = 1200 };
    const double tone = 4000.0;
    float audio[AN];
    for (size_t i = 0; i < AN; i++) {
        audio[i] = (float)(0.9 * sin(2.0 * M_PI * tone * (double)i / (double)AR));
    }
    const prerender_params_t ref_pp = {.oversample = 1e9, .max_rate_hz = OUT}; /* rate == OUT */
    const prerender_params_t low_pp = {.oversample = 2.0, .max_rate_hz = 4000000U};
    iq_ci16_t ref[N];
    iq_ci16_t test[N];
    render_audio_once(SCENARIO_MODULATION_AM, AR, audio, AN, 12000, 0.0, &ref_pp, OUT, ref, N);
    render_audio_once(SCENARIO_MODULATION_AM, AR, audio, AN, 12000, 0.0, &low_pp, OUT, test, N);

    const double car_ref = dft_bin_mag(ref, N, 0.0, OUT);
    const double car_test = dft_bin_mag(test, N, 0.0, OUT);
    const double sb_ref = dft_bin_mag(ref, N, tone, OUT);
    const double sb_test = dft_bin_mag(test, N, tone, OUT);
    ck_assert(fabs(20.0 * log10(car_test / car_ref)) < 0.5);
    ck_assert(fabs(20.0 * log10(sb_test / sb_ref)) < 0.5);

    const double sig[] = {0.0, tone, -tone};
    ck_assert(max_spur_dbc(test, N, OUT, sig, 3, car_ref) < -38.0);
}
END_TEST

START_TEST(renderer_prerender_ssb_matches_reference_and_rejects_image)
{
    /* AT2 + AQ2: SSB tone level within 0.5 dB of the full-rate reference, opposite sideband
     * rejected (Hilbert-limited ~46 dB), spurs below -38 dBc, for both USB and LSB. */
    enum { OUT = 480000, AR = 48000, AN = 4096, N = 1200 };
    const double tone = 4000.0;
    float audio[AN];
    for (size_t i = 0; i < AN; i++) {
        audio[i] = (float)(0.9 * sin(2.0 * M_PI * tone * (double)i / (double)AR));
    }
    const prerender_params_t ref_pp = {.oversample = 1e9, .max_rate_hz = OUT};
    const prerender_params_t low_pp = {.oversample = 2.0, .max_rate_hz = 4000000U};
    const scenario_modulation_t modes[] = {SCENARIO_MODULATION_USB, SCENARIO_MODULATION_LSB};
    for (size_t m = 0; m < 2; m++) {
        const double tone_f = modes[m] == SCENARIO_MODULATION_USB ? tone : -tone;
        iq_ci16_t ref[N];
        iq_ci16_t test[N];
        render_audio_once(modes[m], AR, audio, AN, 6000, 0.0, &ref_pp, OUT, ref, N);
        render_audio_once(modes[m], AR, audio, AN, 6000, 0.0, &low_pp, OUT, test, N);

        const double tone_ref = dft_bin_mag(ref, N, tone_f, OUT);
        const double tone_test = dft_bin_mag(test, N, tone_f, OUT);
        const double image = dft_bin_mag(test, N, -tone_f, OUT);
        ck_assert(fabs(20.0 * log10(tone_test / tone_ref)) < 0.5);
        ck_assert(20.0 * log10(tone_test / image) >= 44.0);
    }
}
END_TEST

START_TEST(renderer_wbfm_prerender_demod_recovers_tone)
{
    /* AQ1 -- the end-to-end gate proving the rearchitecture preserves FM fidelity: pre-render a
     * 1 kHz tone as WBFM, render >=16 consecutive grid blocks, channel-filter and FM-demodulate,
     * and require the recovered tone at >=40 dB SNR with no spectral line at the block rate. */
    const uint32_t OUT = 3072000U;
    const uint32_t AR = 192000U; /* tone well-sampled so the audio-integration floor is not the limit */
    const double TONE = 1000.0;
    const size_t BLK = 4096;
    const size_t NBLK = 40; /* >= 16 consecutive grid blocks; longer window tightens the SNR estimate */
    const size_t N = BLK * NBLK;
    const size_t AN = (size_t)((double)N * AR / OUT) + 512;
    float *audio = malloc(AN * sizeof(*audio));
    ck_assert_ptr_nonnull(audio);
    for (size_t i = 0; i < AN; i++) {
        audio[i] = (float)(0.9 * sin(2.0 * M_PI * TONE * (double)i / (double)AR));
    }

    scenario_t sc;
    asset_cache_t cache;
    memset(&sc, 0, sizeof(sc));
    memset(&cache, 0, sizeof(cache));
    sc.schema_version = 1;
    sc.source_count = 1;
    sc.signal_count = 1;
    snprintf(sc.sources[0].id, sizeof(sc.sources[0].id), "%s", "audio");
    sc.sources[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    sc.sources[0].sample_rate_hz = AR;
    sc.sources[0].bandwidth_hz = 200000;
    sc.sources[0].sample_count = AN;
    snprintf(sc.signals[0].signal_id, sizeof(sc.signals[0].signal_id), "%s", "sig");
    snprintf(sc.signals[0].source_reference, sizeof(sc.signals[0].source_reference), "%s", "audio");
    sc.signals[0].center_frequency_hz = 10000000000ULL;
    sc.signals[0].bandwidth_hz = 200000;
    sc.signals[0].power_dbm = -40.0;
    sc.signals[0].modulation = SCENARIO_MODULATION_WBFM;
    sc.signals[0].fm_deviation_hz = 75000.0;
    sc.signals[0].repeat_interval_s = 1000.0;
    cache.asset_count = 1;
    snprintf(cache.assets[0].source_id, sizeof(cache.assets[0].source_id), "%s", "audio");
    cache.assets[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    cache.assets[0].sample_count = AN;
    cached_prerender_t pr;
    char err[128];
    const prerender_params_t low_pp = {.oversample = 2.0, .max_rate_hz = 4000000U};
    ck_assert_msg(asset_cache_prerender_from_audio(&pr, audio, AN, AR, &sc.signals[0], &low_pp, err, sizeof(err)), "%s", err);
    cache.prerenders[0] = pr;

    const receiver_config_t rx = window_receiver(10000000000ULL - OUT / 2, 10000000000ULL + OUT / 2, OUT, OUT, 0.0, 1.0, -40.0);
    double *re = malloc(N * sizeof(*re));
    double *im = malloc(N * sizeof(*im));
    ck_assert(re != NULL && im != NULL);
    render_stats_t st;
    for (size_t b = 0; b < NBLK; b++) {
        iq_ci16_t blk[4096];
        const uint64_t t = streamer_block_start_ns(b, BLK, OUT);
        ck_assert(render_rx_block(&sc, &cache, &rx, t, blk, BLK, &st));
        for (size_t i = 0; i < BLK; i++) {
            re[b * BLK + i] = (double)blk[i].i;
            im[b * BLK + i] = (double)blk[i].q;
        }
    }

    /* Channel LPF a real receiver applies before demod: 127-tap windowed sinc, cutoff 120 kHz. */
    enum { TAPS = 127 };
    double h[TAPS];
    double hsum = 0.0;
    const double fc = 120000.0 / (double)OUT;
    for (int k = 0; k < TAPS; k++) {
        const int mm = k - TAPS / 2;
        const double sinc = (mm == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * mm) / (M_PI * mm);
        const double win = 0.54 - 0.46 * cos(2.0 * M_PI * k / (TAPS - 1));
        h[k] = sinc * win;
        hsum += h[k];
    }
    for (int k = 0; k < TAPS; k++) {
        h[k] /= hsum;
    }

    double *demod = malloc(N * sizeof(*demod));
    ck_assert_ptr_nonnull(demod);
    size_t dn = 0;
    double prev_re = 0.0;
    double prev_im = 0.0;
    bool have_prev = false;
    for (size_t i = TAPS; i < N - TAPS; i++) {
        double fr = 0.0;
        double fi = 0.0;
        for (int k = 0; k < TAPS; k++) {
            const size_t j = i - (size_t)(k - TAPS / 2);
            fr += h[k] * re[j];
            fi += h[k] * im[j];
        }
        if (have_prev && hypot(fr, fi) > 1.0 && hypot(prev_re, prev_im) > 1.0) {
            /* angle(z * conj(prev)) */
            const double cr = fr * prev_re + fi * prev_im;
            const double ci = fi * prev_re - fr * prev_im;
            demod[dn++] = atan2(ci, cr);
        }
        prev_re = fr;
        prev_im = fi;
        have_prev = true;
    }

    double mean = 0.0;
    for (size_t i = 0; i < dn; i++) {
        mean += demod[i];
    }
    mean /= (double)dn;
    double cr = 0.0;
    double ci = 0.0;
    for (size_t i = 0; i < dn; i++) {
        demod[i] -= mean;
        const double th = 2.0 * M_PI * TONE * (double)i / (double)OUT;
        cr += demod[i] * cos(th);
        ci += demod[i] * sin(th);
    }
    cr *= 2.0 / (double)dn;
    ci *= 2.0 / (double)dn;
    const double tone_power = 0.5 * (cr * cr + ci * ci);
    double resid = 0.0;
    double br_re = 0.0;
    double br_im = 0.0;
    const double block_rate = (double)OUT / (double)BLK;
    for (size_t i = 0; i < dn; i++) {
        const double th = 2.0 * M_PI * TONE * (double)i / (double)OUT;
        const double r = demod[i] - (cr * cos(th) + ci * sin(th));
        resid += r * r;
        const double thb = 2.0 * M_PI * block_rate * (double)i / (double)OUT;
        br_re += r * cos(thb);
        br_im += r * sin(thb);
    }
    resid /= (double)dn;
    const double snr_db = 10.0 * log10(tone_power / resid);
    const double br_amp = (2.0 / (double)dn) * sqrt(br_re * br_re + br_im * br_im);
    const double block_line_power = 0.5 * br_amp * br_amp;
    const double block_line_dbc = 10.0 * log10(block_line_power / tone_power);
    ck_assert_msg(snr_db >= 40.0, "WBFM demod SNR %.1f dB < 40 dB", snr_db);
    ck_assert_msg(block_line_dbc <= -40.0, "block-rate line %.1f dBc > -40 dBc", block_line_dbc);

    free(pr.samples);
    free(audio);
    free(re);
    free(im);
    free(demod);
}
END_TEST

START_TEST(renderer_prerender_audio_is_deterministic)
{
    /* AQ4 extended to a pre-rendered audio signal: two independent instances (fresh caches, fresh
     * pre-render) render the same grid block byte-for-byte identically. */
    enum { AR = 48000, AN = 2048, N = 512 };
    float audio[AN];
    for (size_t i = 0; i < AN; i++) {
        audio[i] = (float)(0.8 * sin(2.0 * M_PI * 1500.0 * (double)i / (double)AR));
    }
    const prerender_params_t pp = {.oversample = 2.0, .max_rate_hz = 4000000U};
    iq_ci16_t a[N];
    iq_ci16_t b[N];
    render_audio_once(SCENARIO_MODULATION_WBFM, AR, audio, AN, 200000, 75000.0, &pp, 3072000U, a, N);
    render_audio_once(SCENARIO_MODULATION_WBFM, AR, audio, AN, 200000, 75000.0, &pp, 3072000U, b, N);
    ck_assert_int_eq(memcmp(a, b, sizeof(a)), 0);
}
END_TEST

/* One in-memory replay signal over a caller-filled asset. power_dbm == ref and scale 1, so the
 * end-to-end gain is exactly 1.0 and direct-path output reproduces the file samples verbatim. */
static void setup_replay_signal(scenario_t *scenario, asset_cache_t *cache, iq_ci16_t *asset_samples, size_t n,
                                uint32_t source_rate_hz, scenario_replay_mode_t mode, bool loop,
                                uint64_t f0_hz, uint64_t range_start_hz, uint64_t range_stop_hz)
{
    memset(scenario, 0, sizeof(*scenario));
    memset(cache, 0, sizeof(*cache));
    scenario->schema_version = 1;
    snprintf(scenario->scenario_id, sizeof(scenario->scenario_id), "%s", "unit_replay");
    scenario->source_count = 1;
    scenario->signal_count = 1;

    scenario_source_t *source = &scenario->sources[0];
    snprintf(source->id, sizeof(source->id), "%s", "replay_src");
    snprintf(source->source_type, sizeof(source->source_type), "%s", "iq_file");
    source->source_kind = SCENARIO_SOURCE_IQ_FILE;
    source->sample_rate_hz = source_rate_hz;
    source->bandwidth_hz = source_rate_hz;
    source->sample_count = n;

    scenario_signal_t *signal = &scenario->signals[0];
    snprintf(signal->signal_id, sizeof(signal->signal_id), "%s", "replay_sig");
    snprintf(signal->source_reference, sizeof(signal->source_reference), "%s", "replay_src");
    signal->replay_mode = mode;
    signal->loop = loop;
    signal->center_frequency_hz = f0_hz;
    signal->replay_range_start_hz = range_start_hz;
    signal->replay_range_stop_hz = range_stop_hz;
    signal->bandwidth_hz = source_rate_hz;
    signal->power_dbm = -40.0;
    signal->start_time_s = 0.0;
    signal->repeat_interval_s = loop ? 0.0 : 1.0;

    cache->asset_count = 1;
    snprintf(cache->assets[0].source_id, sizeof(cache->assets[0].source_id), "%s", "replay_src");
    cache->assets[0].sample_count = n;
    cache->assets[0].samples = asset_samples;
}

/* Build a passthrough scenario+cache with one or two rate variants. Pass v1 == NULL for a single
 * variant. The renderer selects the variant matching the channel rate and streams it verbatim. */
static void setup_passthrough_signal(scenario_t *scenario, asset_cache_t *cache,
                                     scenario_replay_mode_t mode, uint64_t f0_hz,
                                     uint64_t range_start_hz, uint64_t range_stop_hz,
                                     iq_ci16_t *v0, size_t n0, uint32_t r0,
                                     iq_ci16_t *v1, size_t n1, uint32_t r1)
{
    memset(scenario, 0, sizeof(*scenario));
    memset(cache, 0, sizeof(*cache));
    scenario->schema_version = 1;
    snprintf(scenario->scenario_id, sizeof(scenario->scenario_id), "%s", "unit_passthrough");
    scenario->source_count = 1;
    scenario->signal_count = 1;

    scenario_source_t *source = &scenario->sources[0];
    snprintf(source->id, sizeof(source->id), "%s", "pt_src");
    snprintf(source->source_type, sizeof(source->source_type), "%s", "iq_file");
    source->source_kind = SCENARIO_SOURCE_IQ_FILE;
    source->passthrough_variant_count = v1 != NULL ? 2 : 1;
    source->passthrough_variants[0].sample_rate_hz = r0;
    source->passthrough_variants[0].bandwidth_hz = r0;
    source->passthrough_variants[0].sample_count = n0;
    if (v1 != NULL) {
        source->passthrough_variants[1].sample_rate_hz = r1;
        source->passthrough_variants[1].bandwidth_hz = r1;
        source->passthrough_variants[1].sample_count = n1;
    }

    scenario_signal_t *signal = &scenario->signals[0];
    snprintf(signal->signal_id, sizeof(signal->signal_id), "%s", "pt_sig");
    snprintf(signal->source_reference, sizeof(signal->source_reference), "%s", "pt_src");
    signal->replay_mode = mode;
    signal->loop = true;
    signal->passthrough = true;
    signal->center_frequency_hz = f0_hz;
    signal->replay_range_start_hz = range_start_hz;
    signal->replay_range_stop_hz = range_stop_hz;
    signal->bandwidth_hz = r0;
    signal->power_dbm = -40.0;

    cache->asset_count = 1;
    snprintf(cache->assets[0].source_id, sizeof(cache->assets[0].source_id), "%s", "pt_src");
    cache->assets[0].variant_count = v1 != NULL ? 2 : 1;
    cache->assets[0].variants[0].samples = v0;
    cache->assets[0].variants[0].sample_count = n0;
    cache->assets[0].variants[0].sample_rate_hz = r0;
    if (v1 != NULL) {
        cache->assets[0].variants[1].samples = v1;
        cache->assets[0].variants[1].sample_count = n1;
        cache->assets[0].variants[1].sample_rate_hz = r1;
    }
}

/* Two loop-periodic tones around f0: one lands in the narrow channel, the other must fold
 * onto an in-band frequency and be rejected by the DDC cascade. */
static void fill_two_tone_asset(iq_ci16_t *asset, size_t n, uint32_t rate_hz,
                                double tone_a_hz, double tone_b_hz, double amplitude)
{
    for (size_t p = 0; p < n; p++) {
        const double phase_a = 2.0 * M_PI * tone_a_hz * (double)p / (double)rate_hz;
        const double phase_b = 2.0 * M_PI * tone_b_hz * (double)p / (double)rate_hz;
        asset[p].i = (int16_t)lrint(amplitude * (cos(phase_a) + cos(phase_b)));
        asset[p].q = (int16_t)lrint(amplitude * (sin(phase_a) + sin(phase_b)));
    }
}

START_TEST(ddc_cascade_extracts_subband_from_wideband_loop)
{
    /* 1.024 MS/s shift-mode loop into a 16 kS/s / 12 kHz channel: ratio 64 goes through the
     * cascade via a cached 256 kS/s intermediate (tail ratio 16, front 4). A +5 kHz tone with
     * the channel tuned +3 kHz off f0 must appear at exactly 2 kHz; a +200 kHz neighbor tone
     * (folding onto 5 kHz) must vanish. */
    enum { N = 8192, COUNT = 512 };
    static iq_ci16_t asset[N];
    fill_two_tone_asset(asset, N, 1024000U, 5000.0, 200000.0, 8000.0);
    scenario_t scenario;
    asset_cache_t cache;
    setup_replay_signal(&scenario, &cache, asset, N, 1024000U,
                        SCENARIO_REPLAY_SHIFT, true, 100000000ULL, 99500000ULL, 100500000ULL);

    iq_ci16_t out[2 * COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(100003000ULL, 12000U, 16000U, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);

    const double wanted = dft_bin_mag(out, COUNT, 2000.0, 16000.0);
    ck_assert_double_ge(wanted, 7500.0);
    ck_assert_double_le(wanted, 8500.0);
    /* The neighbor's fold target (200k - 3k = 197 kHz = 12*16 kHz + 5 kHz). */
    ck_assert_double_le(dft_bin_mag(out, COUNT, 5000.0, 16000.0), 8.0);

    /* Determinism: the same block renders bit-identically (second render is a cache hit,
     * and entry content is a pure function of its key). */
    iq_ci16_t again[COUNT];
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, again, COUNT, &stats));
    ck_assert_int_eq(memcmp(out, again, COUNT * sizeof(out[0])), 0);

    /* Statelessness across blocks: block 2 rendered on its own matches the second half of a
     * contiguous double-length render (only oscillator re-anchor rounding may differ). */
    iq_ci16_t both[2 * COUNT];
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, both, 2 * COUNT, &stats));
    iq_ci16_t second[COUNT];
    const uint64_t t1_ns = (uint64_t)COUNT * 1000000000ULL / 16000ULL;
    ck_assert(render_rx_block(&scenario, &cache, &rx, t1_ns, second, COUNT, &stats));
    for (size_t k = 0; k < COUNT; k++) {
        ck_assert(abs((int)second[k].i - (int)both[COUNT + k].i) <= 2);
        ck_assert(abs((int)second[k].q - (int)both[COUNT + k].q) <= 2);
    }

    /* A detector re-tuning a hair off (+125 Hz) reuses the cached intermediate (same tune
     * grid point); the residual NCO moves the tone to 1875 Hz exactly. */
    rx = fixed_channel_receiver(100003125ULL, 12000U, 16000U, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    const double retuned = dft_bin_mag(out, COUNT, 1875.0, 16000.0);
    ck_assert_double_ge(retuned, 7500.0);
    ck_assert_double_le(retuned, 8500.0);
}
END_TEST

START_TEST(ddc_direct_cascade_when_intermediate_not_available)
{
    /* Ratio 20 has no split with tail >= 16 and front >= 4, so the block runs the full
     * cascade from the source rate every time -- same extraction, no cache. */
    enum { N = 8192, COUNT = 512 };
    static iq_ci16_t asset[N];
    fill_two_tone_asset(asset, N, 320000U, 5000.0, 100000.0, 8000.0);
    scenario_t scenario;
    asset_cache_t cache;
    setup_replay_signal(&scenario, &cache, asset, N, 320000U,
                        SCENARIO_REPLAY_SHIFT, true, 100000000ULL, 99500000ULL, 100500000ULL);

    iq_ci16_t out[COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(100003000ULL, 12000U, 16000U, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);

    const double wanted = dft_bin_mag(out, COUNT, 2000.0, 16000.0);
    ck_assert_double_ge(wanted, 7500.0);
    ck_assert_double_le(wanted, 8500.0);
    /* 100 kHz neighbor folds onto 4 kHz (100k - 3k = 97 kHz = 6*16 kHz + 1 kHz). */
    ck_assert_double_le(dft_bin_mag(out, COUNT, 1000.0, 16000.0), 8.0);
}
END_TEST

START_TEST(replay_range_mode_follows_tune_and_loops)
{
    /* Ramp asset at the full 98.304 MS/s output rate: equal rates keep the direct (no-resample)
     * path, so the output must reproduce the file bytes exactly -- including across the loop
     * seam inside one block. */
    enum { N = 1000, COUNT = 2048 };
    static iq_ci16_t asset[N];
    for (size_t i = 0; i < N; i++) {
        asset[i] = (iq_ci16_t){.i = (int16_t)i, .q = (int16_t)(N - i)};
    }
    scenario_t scenario;
    asset_cache_t cache;
    setup_replay_signal(&scenario, &cache, asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ,
                        SCENARIO_REPLAY_RANGE, true, 0, 99900000ULL, 100100000ULL);

    /* 1 ms on the 98.304 MS/s grid is exactly output sample 98304 -> file offset 304. */
    const uint64_t t_ns = 1000000ULL;
    iq_ci16_t low[COUNT];
    iq_ci16_t high[COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(99950000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, t_ns, low, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    for (size_t k = 0; k < COUNT; k++) {
        const size_t pos = (98304U + k) % N;
        ck_assert_int_eq(low[k].i, asset[pos].i);
        ck_assert_int_eq(low[k].q, asset[pos].q);
    }

    /* Anywhere else inside the range: bit-identical output. */
    rx = fixed_channel_receiver(100050000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, t_ns, high, COUNT, &stats));
    ck_assert_int_eq(memcmp(low, high, sizeof(low)), 0);

    /* Outside the range: silent. */
    rx = fixed_channel_receiver(100200000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, t_ns, high, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(!samples_have_energy(high, COUNT));
}
END_TEST

START_TEST(replay_shift_mode_pins_absolute_frequency)
{
    /* DC-only file nominally at 100 MHz. Tuned to 110 MHz the content must stay at 100 MHz
     * absolute, i.e. appear as a -10 MHz tone in the channel: s(t) * e^{j*2*pi*(f0-f_tune)*t}. */
    enum { N = 1000, COUNT = 4096 };
    static iq_ci16_t asset[N];
    for (size_t i = 0; i < N; i++) {
        asset[i] = (iq_ci16_t){.i = 10000, .q = 0};
    }
    scenario_t scenario;
    asset_cache_t cache;
    setup_replay_signal(&scenario, &cache, asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ,
                        SCENARIO_REPLAY_SHIFT, true, 100000000ULL, 80000000ULL, 120000000ULL);

    iq_ci16_t out[COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(110000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    const double at_minus_10m = dft_bin_mag(out, COUNT, -10000000.0, (double)SIM_RECEIVER_SAMPLE_RATE_HZ);
    const double at_dc = dft_bin_mag(out, COUNT, 0.0, (double)SIM_RECEIVER_SAMPLE_RATE_HZ);
    ck_assert(at_minus_10m > 5000.0);
    ck_assert(at_dc < at_minus_10m / 100.0);

    /* Tuned to f0 itself the rotation is zero: pure DC. */
    rx = fixed_channel_receiver(100000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert(dft_bin_mag(out, COUNT, 0.0, (double)SIM_RECEIVER_SAMPLE_RATE_HZ) > 5000.0);

    /* Outside the shift range: silent. */
    rx = fixed_channel_receiver(125000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(!samples_have_energy(out, COUNT));
}
END_TEST

START_TEST(fixed_mode_with_loop_uses_epoch_position)
{
    /* loop=true on a fixed-placement signal: normal passband/offset machinery, but the
     * playback position comes from the continuous epoch grid instead of start/repeat. */
    enum { N = 500, COUNT = 1024 };
    static iq_ci16_t asset[N];
    for (size_t i = 0; i < N; i++) {
        asset[i] = (iq_ci16_t){.i = (int16_t)(i + 1), .q = 0};
    }
    scenario_t scenario;
    asset_cache_t cache;
    setup_replay_signal(&scenario, &cache, asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ,
                        SCENARIO_REPLAY_FIXED, true, 100000000ULL, 0, 0);
    /* Keep the declared band inside the window so the fractional-overlap gain is exactly 1. */
    scenario.signals[0].bandwidth_hz = (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ;

    iq_ci16_t out[COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(100000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    const uint64_t t_ns = 1000000ULL; /* output sample 98304 -> offset 98304 % 500 = 304 */
    ck_assert(render_rx_block(&scenario, &cache, &rx, t_ns, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    for (size_t k = 0; k < COUNT; k++) {
        const size_t pos = (98304U + k) % N;
        ck_assert_int_eq(out[k].i, asset[pos].i);
    }
}
END_TEST

START_TEST(passthrough_bypasses_mixer_for_matching_rate_range_signal)
{
    /* Unit gain (power_dbm == ref) so the fast path takes the literal memcpy branch: output
     * must be byte-identical to the file, and a second unrelated fixed-mode signal in the same
     * scenario must be invisible (the passthrough channel never enters the general mixer). */
    enum { N = 2000, COUNT = 4096 };
    static iq_ci16_t asset[N];
    for (size_t i = 0; i < N; i++) {
        asset[i] = (iq_ci16_t){.i = (int16_t)(1000 + (int)i), .q = (int16_t)(2000 - (int)i)};
    }
    scenario_t scenario;
    asset_cache_t cache;
    setup_passthrough_signal(&scenario, &cache, SCENARIO_REPLAY_RANGE, 0, 99900000ULL, 100100000ULL,
                             asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ, NULL, 0, 0);

    iq_ci16_t out[COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(100000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    for (size_t k = 0; k < COUNT; k++) {
        ck_assert_int_eq(out[k].i, asset[k % N].i);
        ck_assert_int_eq(out[k].q, asset[k % N].q);
    }
}
END_TEST

START_TEST(passthrough_selects_variant_by_channel_rate)
{
    /* Two variants at different rates carry distinguishable constants. The channel rate selects
     * which file streams; a rate with no variant is silent (passthrough never resamples). */
    enum { NA = 1500, NB = 900 };
    static iq_ci16_t va[NA];
    static iq_ci16_t vb[NB];
    for (size_t i = 0; i < NA; i++) va[i] = (iq_ci16_t){.i = 111, .q = -111};
    for (size_t i = 0; i < NB; i++) vb[i] = (iq_ci16_t){.i = 777, .q = -777};
    scenario_t scenario;
    asset_cache_t cache;
    setup_passthrough_signal(&scenario, &cache, SCENARIO_REPLAY_RANGE, 0, 99900000ULL, 100100000ULL,
                             va, NA, SIM_RECEIVER_SAMPLE_RATE_HZ, vb, NB, SIM_DDC_SAMPLE_RATE_HZ);

    iq_ci16_t out[512];
    render_stats_t stats;
    /* Channel at variant A's rate -> variant A content (unit gain memcpy). */
    receiver_config_t rx = fixed_channel_receiver(100000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, 512, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_eq(out[0].i, 111);
    ck_assert_int_eq(out[300].i, 111);
    /* Channel at variant B's rate -> variant B content. */
    rx = fixed_channel_receiver(100000000ULL, SIM_DDC_BANDWIDTH_HZ, SIM_DDC_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, 512, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_eq(out[0].i, 777);
    ck_assert_int_eq(out[300].i, 777);
    /* Channel at a rate with no variant -> silence, still handled (not passed to the mixer). */
    rx = fixed_channel_receiver(100000000ULL, 1000000U, 1000000U, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, 512, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(!samples_have_energy(out, 512));
}
END_TEST

START_TEST(passthrough_rotates_for_shift_without_mixer)
{
    /* Shift mode still needs the frequency rotation (unavoidable DSP), but must skip the float
     * mix bus: the output should match the general renderer's own shift-mode result exactly
     * (same math, different code path), confirming passthrough doesn't silently change the
     * signal's placement. */
    enum { N = 2000, COUNT = 4096 };
    static iq_ci16_t asset[N];
    for (size_t i = 0; i < N; i++) {
        asset[i] = (iq_ci16_t){.i = 10000, .q = 0};
    }
    scenario_t scenario_fast, scenario_general;
    asset_cache_t cache_fast, cache_general;
    setup_passthrough_signal(&scenario_fast, &cache_fast, SCENARIO_REPLAY_SHIFT, 100000000ULL, 80000000ULL, 120000000ULL,
                             asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ, NULL, 0, 0);
    setup_replay_signal(&scenario_general, &cache_general, asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ,
                        SCENARIO_REPLAY_SHIFT, true, 100000000ULL, 80000000ULL, 120000000ULL);

    iq_ci16_t out_fast[COUNT];
    iq_ci16_t out_general[COUNT];
    render_stats_t stats;
    receiver_config_t rx = fixed_channel_receiver(110000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario_fast, &cache_fast, &rx, 0, out_fast, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(render_rx_block(&scenario_general, &cache_general, &rx, 0, out_general, COUNT, &stats));
    /* Independent NCO implementations (VOLK rotator vs scalar mixer path) can differ by a
     * rounding count -- allow a small per-sample tolerance rather than requiring bit-exactness. */
    for (size_t k = 0; k < COUNT; k++) {
        ck_assert(abs((int)out_fast[k].i - (int)out_general[k].i) <= 2);
        ck_assert(abs((int)out_fast[k].q - (int)out_general[k].q) <= 2);
    }
}
END_TEST

START_TEST(passthrough_is_silent_out_of_range_and_at_unsupported_rate)
{
    /* Passthrough never resamples: an in-range channel whose rate has no variant renders silence,
     * and an out-of-range tune renders silence too (no signal reaches the mixer). */
    enum { N = 2000, COUNT = 1024 };
    static iq_ci16_t asset[N];
    for (size_t i = 0; i < N; i++) {
        asset[i] = (iq_ci16_t){.i = 5000, .q = 0};
    }
    scenario_t scenario;
    asset_cache_t cache;
    /* One variant at 98.304 MS/s only. */
    setup_passthrough_signal(&scenario, &cache, SCENARIO_REPLAY_RANGE, 0, 99900000ULL, 100100000ULL,
                             asset, N, SIM_RECEIVER_SAMPLE_RATE_HZ, NULL, 0, 0);

    iq_ci16_t out[COUNT];
    render_stats_t stats;
    /* In range, but the channel rate (1 MS/s) has no matching variant -> silence, no resample. */
    receiver_config_t rx = fixed_channel_receiver(100000000ULL, 1000000U, 1000000U, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(!samples_have_energy(out, COUNT));

    /* Matching rate but out of range -> silence via the mixer path (passthrough not active). */
    rx = fixed_channel_receiver(130000000ULL, (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ, SIM_RECEIVER_SAMPLE_RATE_HZ, 1.0, -40.0);
    ck_assert(render_rx_block(&scenario, &cache, &rx, 0, out, COUNT, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(!samples_have_energy(out, COUNT));
}
END_TEST

Suite *renderer_suite(void)
{
    Suite *suite = suite_create("renderer");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, nco_phase_is_continuous_across_block_boundaries);
    tcase_add_test(tc, renderer_skips_signal_beyond_output_nyquist);
    tcase_add_test(tc, renderer_mix_bus_clips_once_not_per_signal);
    tcase_add_test(tc, renderer_decimation_attenuates_out_of_band_tone);
    tcase_add_test(tc, renders_nonzero_visible_signal);
    tcase_add_test(tc, renderer_80mhz_sinc_resamples_24576_source_to_98304_output);
    tcase_add_test(tc, renderer_low_rate_upsample_uses_linear_path);
    tcase_add_test(tc, renderer_applies_rf_power_relative_to_reference);
    tcase_add_test(tc, renderer_mixes_two_signals_without_clipping);
    tcase_add_test(tc, renderer_clips_after_mixing_two_signals);
    tcase_add_test(tc, scanner_moves_fixed_rf_signal_through_baseband);
    tcase_add_test(tc, scanner_same_time_is_deterministic_across_instances);
    tcase_add_test(tc, renders_ddc_nonzero_visible_signal);
    tcase_add_test(tc, renders_ddc_4096_sample_block_for_waterfall);
    tcase_add_test(tc, renderer_adds_deterministic_noise_floor_without_active_signals);
    tcase_add_test(tc, renderer_noise_scales_with_window_bandwidth_as_density);
    tcase_add_test(tc, renderer_renders_audio_modulation_modes);
    tcase_add_test(tc, renderer_am_envelope_is_normalised_and_does_not_over_saturate);
    tcase_add_test(tc, renderer_audio_stops_at_end_of_asset_within_block);
    tcase_add_test(tc, renderer_prerender_am_matches_reference_synthesis);
    tcase_add_test(tc, renderer_prerender_ssb_matches_reference_and_rejects_image);
    tcase_add_test(tc, renderer_wbfm_prerender_demod_recovers_tone);
    tcase_add_test(tc, renderer_prerender_audio_is_deterministic);
    tcase_add_test(tc, renders_burst_scenario_only_during_active_second);
    tcase_add_test(tc, renderer_applies_window_passband_gain);
    tcase_add_test(tc, replay_range_mode_follows_tune_and_loops);
    tcase_add_test(tc, replay_shift_mode_pins_absolute_frequency);
    tcase_add_test(tc, fixed_mode_with_loop_uses_epoch_position);
    tcase_add_test(tc, passthrough_bypasses_mixer_for_matching_rate_range_signal);
    tcase_add_test(tc, passthrough_selects_variant_by_channel_rate);
    tcase_add_test(tc, passthrough_rotates_for_shift_without_mixer);
    tcase_add_test(tc, passthrough_is_silent_out_of_range_and_at_unsupported_rate);
    tcase_add_test(tc, ddc_cascade_extracts_subband_from_wideband_loop);
    tcase_add_test(tc, ddc_direct_cascade_when_intermediate_not_available);
    suite_add_tcase(suite, tc);
    return suite;
}
