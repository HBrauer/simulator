#include "config.h"
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

    iq_ci16_t out[128];
    render_stats_t stats;
    ck_assert(renderer_render_80mhz_block(&scenario, &config.receivers[0], 450000ULL, out, 128, &stats));
    ck_assert_uint_eq(stats.samples_rendered, 128);
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_ne(out[0].i, 0);
}
END_TEST

START_TEST(renders_ddc_nonzero_visible_signal)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);

    ddc_config_t ddc = {
        .id = 0,
        .center_frequency_hz = 10005000000ULL,
        .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
    };
    iq_ci16_t out[128];
    render_stats_t stats;
    ck_assert(renderer_render_ddc_block(&scenario, &ddc, 450000ULL, out, 128, &stats));
    ck_assert_uint_eq(stats.samples_rendered, 128);
    ck_assert_uint_eq(stats.active_signals, 1);
    ck_assert_int_ne(out[0].i, 0);
}
END_TEST

Suite *renderer_suite(void)
{
    Suite *suite = suite_create("renderer");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, renders_nonzero_visible_signal);
    tcase_add_test(tc, renders_ddc_nonzero_visible_signal);
    suite_add_tcase(suite, tc);
    return suite;
}
