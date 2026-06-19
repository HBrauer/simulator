#include "asset_cache.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>

START_TEST(loads_scenario_assets_into_memory)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(cache.asset_count, 1);
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    ck_assert_uint_eq(asset->sample_count, 24576);
    ck_assert_int_ne(asset->samples[0].i, 0);
    asset_cache_free(&cache);
}
END_TEST

Suite *asset_cache_suite(void)
{
    Suite *suite = suite_create("asset_cache");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_scenario_assets_into_memory);
    suite_add_tcase(suite, tc);
    return suite;
}
