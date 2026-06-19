#include "scenario.h"
#include "test_suites.h"

#include <check.h>

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

Suite *scenario_suite(void)
{
    Suite *suite = suite_create("scenario");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_and_validates_scenario);
    suite_add_tcase(suite, tc);
    return suite;
}
