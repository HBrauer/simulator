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
    return (receiver_config_t){
        .id = 0,
        .frequency_start_hz = 9960000000ULL,
        .frequency_stop_hz = 10040000000ULL,
        .bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -40.0,
    };
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
    return (receiver_config_t){
        .id = 0,
        .frequency_start_hz = 9960000000ULL,
        .frequency_stop_hz = 10060000000ULL,
        .bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ,
        .scan_rate_hz_per_s = 100000000000.0,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -40.0,
    };
}

START_TEST(renders_nonzero_visible_signal)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("simulator/configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    iq_ci16_t out[128];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 128, &stats));
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
    ck_assert_msg(config_load_yaml("simulator/configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);
    config.receivers[0].frequency_start_hz = 9965000000ULL;
    config.receivers[0].frequency_stop_hz = 10045000000ULL;

    iq_ci16_t out[8];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 8, &stats));
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
    for (size_t i = 0; i < 5; i++) {
        const double pos = fraction + (double)i * source_per_output;
        ck_assert_int_le(abs(out[i].i - sinc_scaled_i16(&asset->samples[offset], 8, pos, false, gain)), 2);
        ck_assert_int_le(abs(out[i].q - sinc_scaled_i16(&asset->samples[offset], 8, pos, true, gain)), 2);
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
    scenario.sources[0].bandwidth_hz = 1000;
    scenario.sources[0].sample_count = 4;
    snprintf(scenario.signals[0].signal_id, sizeof(scenario.signals[0].signal_id), "%s", "low_sig");
    snprintf(scenario.signals[0].source_reference, sizeof(scenario.signals[0].source_reference), "%s", "low_src");
    scenario.signals[0].center_frequency_hz = 10000000000ULL;
    scenario.signals[0].bandwidth_hz = 1000;
    scenario.signals[0].power_dbm = -40.0;
    scenario.signals[0].start_time_s = 0.0;
    scenario.signals[0].repeat_interval_s = 1.0;

    cache.asset_count = 1;
    snprintf(cache.assets[0].source_id, sizeof(cache.assets[0].source_id), "%s", "low_src");
    cache.assets[0].sample_count = 4;
    cache.assets[0].samples = asset_samples;

    const receiver_config_t receiver = {
        .id = 0,
        .frequency_start_hz = 9999999500ULL,
        .frequency_stop_hz = 10000000500ULL,
        .bandwidth_hz = 1000,
        .sample_rate_hz = 16000,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -40.0,
    };
    iq_ci16_t out[16];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 16, &stats));
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
    ck_assert_msg(config_load_yaml("simulator/configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    scenario.signals[0].power_dbm = -49.0;
    config.receivers[0].rf_reference_power_dbm = -55.0;
    config.receivers[0].frequency_start_hz = 9965000000ULL;
    config.receivers[0].frequency_stop_hz = 10045000000ULL;

    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);
    iq_ci16_t out[4];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 4, &stats));
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    const double gain = pow(10.0, 6.0 / 20.0);
    /* +6 dB relative to the reference. Playback starts at the fractional sample 11059.2. */
    const double base_s = 450000.0 / 1000000000.0;
    const double exact = base_s * 24576000.0;
    const double fraction = exact - floor(exact);
    ck_assert_int_le(abs(out[0].i - sinc_scaled_i16(&asset->samples[11059], 8, fraction, false, gain)), 2);
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
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 4, &stats));

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
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 4, &stats));

    ck_assert_uint_eq(stats.samples_rendered, 4);
    ck_assert_uint_eq(stats.active_signals, 2);
    for (size_t index = 0; index < 4; index++) {
        ck_assert_int_eq(out[index].i, 32767);
        ck_assert_int_eq(out[index].q, -32768);
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

    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, positive_offset, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 200000ULL, zero_offset, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 400000ULL, negative_offset, 4, &stats));
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
    receiver_b.udp_80mhz_output.port = 55000;

    iq_ci16_t out_a[8];
    iq_ci16_t out_b[8];
    render_stats_t stats_a;
    render_stats_t stats_b;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver_a, 400000ULL, out_a, 8, &stats_a));
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver_b, 400000ULL, out_b, 8, &stats_b));

    ck_assert_uint_eq(stats_a.active_signals, 1);
    ck_assert_uint_eq(stats_b.active_signals, 1);
    ck_assert_mem_eq(out_a, out_b, sizeof(out_a));
}
END_TEST

START_TEST(renders_ddc_nonzero_visible_signal)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    ddc_config_t ddc = {
        .id = 0,
        .center_frequency_hz = 10005000000ULL,
        .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -55.0,
    };
    iq_ci16_t out[128];
    render_stats_t stats;
    ck_assert(renderer_render_ddc_block(&scenario, &cache, &ddc, 450000ULL, out, 128, &stats));
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
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    ddc_config_t ddc = {
        .id = 0,
        .center_frequency_hz = 10005000000ULL,
        .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -55.0,
    };
    iq_ci16_t out[4096];
    render_stats_t stats;
    ck_assert(renderer_render_ddc_block(&scenario, &cache, &ddc, 450000ULL, out, 4096, &stats));
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
    scenario.noise_floor.power_dbm = -85.0;
    scenario.noise_floor.seed = 1234;

    asset_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    receiver_config_t receiver = fixed_center_receiver();
    receiver.rf_reference_power_dbm = -85.0;

    iq_ci16_t out_a[32];
    iq_ci16_t out_b[32];
    iq_ci16_t out_c[32];
    render_stats_t stats;

    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 1000000ULL, out_a, 32, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert_uint_eq(stats.samples_rendered, 32);
    ck_assert(samples_have_energy(out_a, 32));

    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 1000000ULL, out_b, 32, &stats));
    ck_assert_mem_eq(out_a, out_b, sizeof(out_a));

    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 2000000ULL, out_c, 32, &stats));
    ck_assert(memcmp(out_a, out_c, sizeof(out_a)) != 0);
}
END_TEST

START_TEST(renderer_renders_audio_modulation_modes)
{
    scenario_t scenario;
    asset_cache_t cache;
    float audio[32];
    float hilbert[32];
    double integral[33];
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
    scenario.sources[0].sample_count = 32;

    snprintf(scenario.signals[0].signal_id, sizeof(scenario.signals[0].signal_id), "%s", "audio_sig");
    snprintf(scenario.signals[0].source_reference, sizeof(scenario.signals[0].source_reference), "%s", "audio");
    scenario.signals[0].center_frequency_hz = 10000000000ULL;
    scenario.signals[0].bandwidth_hz = 200000;
    scenario.signals[0].power_dbm = -80.0;
    scenario.signals[0].fm_deviation_hz = 75000.0;
    scenario.signals[0].am_depth = 0.8;
    scenario.signals[0].start_time_s = 0.0;
    scenario.signals[0].repeat_interval_s = 1.0;

    integral[0] = 0.0;
    for (size_t i = 0; i < 32; i++) {
        audio[i] = (float)sin(2.0 * M_PI * (double)i / 32.0);
        hilbert[i] = (float)cos(2.0 * M_PI * (double)i / 32.0);
        integral[i + 1U] = integral[i] + (double)audio[i];
    }
    cache.asset_count = 1;
    snprintf(cache.assets[0].source_id, sizeof(cache.assets[0].source_id), "%s", "audio");
    cache.assets[0].source_kind = SCENARIO_SOURCE_AUDIO_FILE;
    cache.assets[0].sample_count = 32;
    cache.assets[0].audio_samples = audio;
    cache.assets[0].audio_hilbert = hilbert;
    cache.assets[0].audio_integral = integral;

    receiver_config_t receiver = fixed_center_receiver();
    receiver.rf_reference_power_dbm = -40.0;
    iq_ci16_t out[64];
    render_stats_t stats;
    const scenario_modulation_t modes[] = {
        SCENARIO_MODULATION_WBFM,
        SCENARIO_MODULATION_AM,
        SCENARIO_MODULATION_USB,
        SCENARIO_MODULATION_LSB,
    };
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        memset(out, 0, sizeof(out));
        scenario.signals[0].modulation = modes[m];
        ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 64, &stats));
        ck_assert_uint_eq(stats.active_signals, 1);
        ck_assert(samples_have_energy(out, 64));
    }
}
END_TEST

START_TEST(renders_burst_scenario_only_during_active_second)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/burst_1s_every_5s.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    ddc_config_t ddc = {
        .id = 0,
        .center_frequency_hz = 10005000000ULL,
        .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -55.0,
    };
    iq_ci16_t active_a[1024];
    iq_ci16_t inactive[1024];
    iq_ci16_t active_b[1024];
    render_stats_t stats;

    ck_assert(renderer_render_ddc_block(&scenario, &cache, &ddc, 500000000ULL, active_a, 1024, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert(renderer_render_ddc_block(&scenario, &cache, &ddc, 1500000000ULL, inactive, 1024, &stats));
    ck_assert_uint_eq(stats.active_signals, 0);
    ck_assert(renderer_render_ddc_block(&scenario, &cache, &ddc, 5500000000ULL, active_b, 1024, &stats));
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
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_eq(out[0].i, 1000);

    scenario.signals[0].center_frequency_hz = 10040500000ULL;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_eq(out[0].i, 500);

    scenario.signals[0].center_frequency_hz = 10041500000ULL;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, 0, out, 4, &stats));
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
    const uint32_t rate = receiver.sample_rate_hz;
    const uint64_t t0 = streamer_block_start_ns(0, 64, rate);
    const uint64_t t1 = streamer_block_start_ns(1, 64, rate);

    iq_ci16_t block0[64];
    iq_ci16_t block1[64];
    iq_ci16_t combined[128];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, t0, block0, 64, &stats));
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, t1, block1, 64, &stats));
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &receiver, t0, combined, 128, &stats));

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

Suite *renderer_suite(void)
{
    Suite *suite = suite_create("renderer");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, nco_phase_is_continuous_across_block_boundaries);
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
    tcase_add_test(tc, renderer_renders_audio_modulation_modes);
    tcase_add_test(tc, renders_burst_scenario_only_during_active_second);
    tcase_add_test(tc, renderer_applies_window_passband_gain);
    suite_add_tcase(suite, tc);
    return suite;
}
