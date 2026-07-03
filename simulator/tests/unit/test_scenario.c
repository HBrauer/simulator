#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

START_TEST(loads_and_validates_scenario)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_str_eq(scenario.scenario_id, "test_scenario_001");
    ck_assert_uint_eq(scenario.source_count, 1);
    ck_assert_uint_eq(scenario.signal_count, 1);
}
END_TEST

START_TEST(loads_optional_noise_floor)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/burst_1s_every_5s.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert(scenario.noise_floor.enabled);
    ck_assert_double_eq_tol(scenario.noise_floor.power_dbm, -125.0, 0.001);
    ck_assert_uint_eq(scenario.noise_floor.seed, 49152);
}
END_TEST

START_TEST(derives_source_sample_count_from_file_size)
{
    char path[] = "/tmp/sdr_scenario_without_sample_count_XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    FILE *file = fdopen(fd, "w");
    ck_assert_ptr_nonnull(file);
    fprintf(file,
        "{"
        "\"schema_version\":1,"
        "\"scenario_id\":\"derive_sample_count\","
        "\"sources\":[{"
        "\"id\":\"asset_fsk_001\","
        "\"source_type\":\"iq_file\","
        "\"file\":\"simulator/assets/fsk_20mhz.c16\","
        "\"format\":\"ci16\","
        "\"byte_order\":\"little_endian\","
        "\"iq_layout\":\"interleaved_iq\","
        "\"sample_rate_hz\":24576000,"
        "\"bandwidth_hz\":20000000,"
        "\"center_frequency_hz\":0,"
        "\"nominal_level_dbfs\":-12.0"
        "}],"
        "\"signals\":[{"
        "\"signal_id\":\"sig_fsk_asset\","
        "\"source_reference\":\"asset_fsk_001\","
        "\"center_frequency_hz\":10005000000,"
        "\"bandwidth_hz\":20000000,"
        "\"power_dbm\":-55.0,"
        "\"start_time_s\":0.0,"
        "\"repeat_interval_s\":1.0"
        "}]"
        "}");
    fclose(file);

    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json(path, &scenario, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.sources[0].sample_count, 0);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.sources[0].sample_count, 24576);
    unlink(path);
}
END_TEST

START_TEST(rejects_duplicate_source_ids)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.source_count = 2;
    scenario.sources[1] = scenario.sources[0];
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_source_id");
}
END_TEST

START_TEST(rejects_duplicate_signal_ids)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.signal_count = 2;
    scenario.signals[1] = scenario.signals[0];
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_signal_id");
}
END_TEST

START_TEST(rejects_missing_source_reference)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    snprintf(scenario.signals[0].source_reference, sizeof(scenario.signals[0].source_reference), "%s", "does_not_exist");
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "missing_source_reference");
}
END_TEST

Suite *scenario_suite(void)
{
    Suite *suite = suite_create("scenario");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_and_validates_scenario);
    tcase_add_test(tc, loads_optional_noise_floor);
    tcase_add_test(tc, derives_source_sample_count_from_file_size);
    tcase_add_test(tc, rejects_duplicate_source_ids);
    tcase_add_test(tc, rejects_duplicate_signal_ids);
    tcase_add_test(tc, rejects_missing_source_reference);
    suite_add_tcase(suite, tc);
    return suite;
}
