#include "config.h"
#include "asset_cache.h"
#include "renderer.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>

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
    tcase_add_test(tc, renders_ddc_nonzero_visible_signal);
    suite_add_tcase(suite, tc);
    return suite;
}
