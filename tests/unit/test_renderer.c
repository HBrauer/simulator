#include "config.h"
#include "asset_cache.h"
#include "renderer.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>
#include <stdio.h>
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
    };
}

START_TEST(renders_nonzero_visible_signal)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
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

START_TEST(renderer_80mhz_linearly_interpolates_24576_source_to_98304_output)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
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
    const double gain = pow(10.0, -scenario.sources[0].nominal_level_dbfs / 20.0);
    const size_t offset = 11059;
    ck_assert_int_eq(out[0].i, interpolate_scaled_i16(asset->samples[offset].i, asset->samples[offset].i, 0.0, gain));
    ck_assert_int_eq(out[0].q, interpolate_scaled_i16(asset->samples[offset].q, asset->samples[offset].q, 0.0, gain));
    ck_assert_int_eq(out[1].i, interpolate_scaled_i16(asset->samples[offset].i, asset->samples[offset + 1].i, 0.25, gain));
    ck_assert_int_eq(out[2].i, interpolate_scaled_i16(asset->samples[offset].i, asset->samples[offset + 1].i, 0.50, gain));
    ck_assert_int_eq(out[3].i, interpolate_scaled_i16(asset->samples[offset].i, asset->samples[offset + 1].i, 0.75, gain));
    ck_assert_int_eq(out[4].i, interpolate_scaled_i16(asset->samples[offset + 1].i, asset->samples[offset + 1].i, 0.0, gain));
    asset_cache_free(&cache);
}
END_TEST

START_TEST(renderer_clips_after_nominal_level_gain)
{
    simulator_config_t config;
    scenario_t scenario;
    char error[128];
    ck_assert_msg(config_load_yaml("configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    scenario.sources[0].nominal_level_dbfs = -120.0;
    config.receivers[0].frequency_start_hz = 9965000000ULL;
    config.receivers[0].frequency_stop_hz = 10045000000ULL;

    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);
    iq_ci16_t out[4];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &cache, &config.receivers[0], 450000ULL, out, 4, &stats));
    ck_assert(out[0].i == 32767 || out[0].i == -32768);
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

    ck_assert_int_gt(positive_offset[1].q, 900);
    ck_assert_int_eq(zero_offset[1].q, 0);
    ck_assert_int_lt(negative_offset[1].q, -900);
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
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    asset_cache_t cache;
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    ddc_config_t ddc = {
        .id = 0,
        .center_frequency_hz = 10005000000ULL,
        .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
        .output_scale = 1.0,
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

Suite *renderer_suite(void)
{
    Suite *suite = suite_create("renderer");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, renders_nonzero_visible_signal);
    tcase_add_test(tc, renderer_80mhz_linearly_interpolates_24576_source_to_98304_output);
    tcase_add_test(tc, renderer_clips_after_nominal_level_gain);
    tcase_add_test(tc, renderer_mixes_two_signals_without_clipping);
    tcase_add_test(tc, renderer_clips_after_mixing_two_signals);
    tcase_add_test(tc, scanner_moves_fixed_rf_signal_through_baseband);
    tcase_add_test(tc, scanner_same_time_is_deterministic_across_instances);
    tcase_add_test(tc, renders_ddc_nonzero_visible_signal);
    suite_add_tcase(suite, tc);
    return suite;
}
