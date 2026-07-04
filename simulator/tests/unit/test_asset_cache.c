#include "asset_cache.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>

START_TEST(loads_scenario_assets_into_memory)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
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

START_TEST(loads_assets_in_small_batches)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load_limited(&cache, &scenario, 0, 17, error, sizeof(error)), "%s", error);
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    ck_assert_uint_eq(asset->sample_count, 24576);
    ck_assert_int_ne(asset->samples[0].i, 0);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(rejects_asset_cache_memory_limit)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert(!asset_cache_load_limited(&cache, &scenario, 16, 4096, error, sizeof(error)));
    ck_assert_str_eq(error, "asset_cache_limit_exceeded");
}
END_TEST

START_TEST(normalizes_audio_asset_to_target_rms)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/audio_radio_demo.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    const cached_asset_t *asset = asset_cache_find(&cache, "radio_clip_wav");
    ck_assert_ptr_nonnull(asset);
    ck_assert_ptr_nonnull(asset->audio_samples);
    double sum_sq = 0.0;
    for (uint64_t i = 0; i < asset->sample_count; i++) {
        sum_sq += (double)asset->audio_samples[i] * (double)asset->audio_samples[i];
    }
    const double rms = sqrt(sum_sq / (double)asset->sample_count);
    /* Normalised to 1/sqrt(2) so power_dbm means the same thing regardless of source level. */
    ck_assert(fabs(rms - 0.70710678) < 1e-3);
    asset_cache_free(&cache);
}
END_TEST

Suite *asset_cache_suite(void)
{
    Suite *suite = suite_create("asset_cache");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_scenario_assets_into_memory);
    tcase_add_test(tc, loads_assets_in_small_batches);
    tcase_add_test(tc, rejects_asset_cache_memory_limit);
    tcase_add_test(tc, normalizes_audio_asset_to_target_rms);
    suite_add_tcase(suite, tc);
    return suite;
}
