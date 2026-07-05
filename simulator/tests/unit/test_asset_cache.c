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
    ck_assert_msg(asset_cache_load_limited(&cache, &scenario, 0, 17, NULL, error, sizeof(error)), "%s", error);
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
    ck_assert(!asset_cache_load_limited(&cache, &scenario, 16, 4096, NULL, error, sizeof(error)));
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

START_TEST(prerenders_audio_signals_at_content_bandwidth_rate)
{
    /* audio_radio_demo: one 48 kHz WAV modulated as WBFM/AM/USB/LSB. Oversample defaults to 2.0,
     * so the intermediate rates are 2x the content bandwidth (Carson for FM, audio_rate for AM,
     * audio_rate/2 for SSB), floored to the declared signal bandwidth. */
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/audio_radio_demo.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    const cached_asset_t *asset = asset_cache_find(&cache, "radio_clip_wav");
    ck_assert_ptr_nonnull(asset);
    const uint64_t frames = asset->sample_count;

    const uint32_t expected_rate[4] = {396000U, 96000U, 48000U, 48000U}; /* wbfm, am, usb, lsb */
    for (size_t s = 0; s < 4; s++) {
        const cached_prerender_t *pr = asset_cache_prerender(&cache, s);
        ck_assert_ptr_nonnull(pr);
        ck_assert_uint_eq(pr->sample_rate_hz, expected_rate[s]);
        const uint64_t expected_count = (uint64_t)llround((double)frames * (double)expected_rate[s] / 48000.0);
        ck_assert_uint_eq(pr->sample_count, expected_count);
        ck_assert_ptr_nonnull(pr->samples);
        ck_assert(pr->gain > 0.0);
    }
    /* FM is constant-modulus, so its recorded gain is exactly 1.0 (scale exactly 32767). */
    ck_assert(fabs(asset_cache_prerender(&cache, 0)->gain - 1.0) < 1e-12);
    asset_cache_free(&cache);
}
END_TEST

static double audio_linear_probe(const float *samples, uint64_t count, double position)
{
    uint64_t index = (uint64_t)position;
    if (index + 1ULL >= count) {
        return index < count ? (double)samples[index] : 0.0;
    }
    const double fraction = position - (double)index;
    return (double)samples[index] + ((double)samples[index + 1ULL] - (double)samples[index]) * fraction;
}

START_TEST(prerender_gain_reconstructs_direct_am_amplitude)
{
    /* The recorded gain folds the peak normalisation back out: stored*gain/32767 must reproduce
     * the AM envelope the direct full-rate synthesis would have produced, to within quantisation. */
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/audio_radio_demo.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load(&cache, &scenario, error, sizeof(error)), "%s", error);

    const cached_asset_t *asset = asset_cache_find(&cache, "radio_clip_wav");
    const cached_prerender_t *pr = asset_cache_prerender(&cache, 1); /* AM station */
    const double depth = scenario.signals[1].am_depth;
    const double audio_rate = 48000.0;
    for (size_t k = 0; k < 5; k++) {
        const uint64_t i = pr->sample_count / 6U * (k + 1U);
        const double position = (double)i * audio_rate / (double)pr->sample_rate_hz;
        const double audio = audio_linear_probe(asset->audio_samples, asset->sample_count, position);
        const double direct = (1.0 + depth * audio) / (1.0 + depth);
        const double reconstructed = (double)pr->samples[i].i * pr->gain / 32767.0;
        ck_assert(fabs(reconstructed - direct) < 2.0e-3);
        ck_assert_int_eq(pr->samples[i].q, 0);
    }
    asset_cache_free(&cache);
}
END_TEST

START_TEST(prerender_is_deterministic_across_loads)
{
    scenario_t scenario;
    asset_cache_t cache_a;
    asset_cache_t cache_b;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/audio_radio_demo.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load(&cache_a, &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load(&cache_b, &scenario, error, sizeof(error)), "%s", error);
    for (size_t s = 0; s < 4; s++) {
        const cached_prerender_t *a = asset_cache_prerender(&cache_a, s);
        const cached_prerender_t *b = asset_cache_prerender(&cache_b, s);
        ck_assert_uint_eq(a->sample_count, b->sample_count);
        ck_assert_int_eq(memcmp(a->samples, b->samples, (size_t)a->sample_count * sizeof(*a->samples)), 0);
    }
    asset_cache_free(&cache_a);
    asset_cache_free(&cache_b);
}
END_TEST

START_TEST(rejects_prerender_memory_limit)
{
    /* 8 MB fits the ~5.9 MB audio asset + helpers but not the ~12 MB WBFM pre-render, so the
     * limit is enforced against the pre-render (not just the raw source). */
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/audio_radio_demo.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert(!asset_cache_load_limited(&cache, &scenario, 8000000, 4096, NULL, error, sizeof(error)));
    ck_assert_str_eq(error, "asset_cache_limit_exceeded");
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
    tcase_add_test(tc, prerenders_audio_signals_at_content_bandwidth_rate);
    tcase_add_test(tc, prerender_gain_reconstructs_direct_am_amplitude);
    tcase_add_test(tc, prerender_is_deterministic_across_loads);
    tcase_add_test(tc, rejects_prerender_memory_limit);
    suite_add_tcase(suite, tc);
    return suite;
}
