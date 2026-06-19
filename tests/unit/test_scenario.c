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
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_str_eq(scenario.scenario_id, "test_scenario_001");
    ck_assert_uint_eq(scenario.source_count, 1);
    ck_assert_uint_eq(scenario.signal_count, 1);
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
        "\"file\":\"assets/fsk_20mhz.c16\","
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

Suite *scenario_suite(void)
{
    Suite *suite = suite_create("scenario");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_and_validates_scenario);
    tcase_add_test(tc, derives_source_sample_count_from_file_size);
    suite_add_tcase(suite, tc);
    return suite;
}
