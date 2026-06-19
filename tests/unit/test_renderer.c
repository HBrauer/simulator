#include "config.h"
#include "asset_cache.h"
#include "renderer.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>

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

START_TEST(renderer_80mhz_maps_24576_source_to_98304_output)
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
    ck_assert_int_eq(out[0].i, out[1].i);
    ck_assert_int_eq(out[1].i, out[2].i);
    ck_assert_int_eq(out[2].i, out[3].i);
    ck_assert_int_ne(out[3].i, out[4].i);
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
    tcase_add_test(tc, renderer_80mhz_maps_24576_source_to_98304_output);
    tcase_add_test(tc, renders_ddc_nonzero_visible_signal);
    suite_add_tcase(suite, tc);
    return suite;
}
