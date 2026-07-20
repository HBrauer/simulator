#include "asset_cache.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

START_TEST(loads_scenario_assets_into_memory)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
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
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_msg(asset_cache_load_limited(&cache, &scenario, 0, 17, NULL, error, sizeof(error)), "%s", error);
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    ck_assert_uint_eq(asset->sample_count, 24576);
    ck_assert_int_ne(asset->samples[0].i, 0);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(mmaps_iq_asset_over_memory_limit)
{
    scenario_t scenario;
    asset_cache_t cache;
    asset_cache_t reference;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/scanner_fsk.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    /* An IQ file over the budget is memory-mapped instead of rejected... */
    ck_assert_msg(asset_cache_load_limited(&cache, &scenario, 16, 4096, NULL, error, sizeof(error)), "%s", error);
    const cached_asset_t *asset = asset_cache_find(&cache, "asset_fsk_001");
    ck_assert_ptr_nonnull(asset);
    ck_assert(asset->mmapped);
    ck_assert_uint_eq(asset->sample_count, 24576);
    /* ...and the mapping is byte-identical to the RAM-loaded copy. */
    ck_assert_msg(asset_cache_load(&reference, &scenario, error, sizeof(error)), "%s", error);
    const cached_asset_t *ram = asset_cache_find(&reference, "asset_fsk_001");
    ck_assert_ptr_nonnull(ram);
    ck_assert(!ram->mmapped);
    ck_assert_int_eq(memcmp(asset->samples, ram->samples, (size_t)asset->sample_count * sizeof(*asset->samples)), 0);
    asset_cache_free(&reference);
    asset_cache_free(&cache);
}
END_TEST

START_TEST(rejects_audio_asset_over_memory_limit)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/audio_radio_demo.yaml", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    /* Audio has no mmap fallback (buffers are rewritten in place at load). */
    ck_assert(!asset_cache_load_limited(&cache, &scenario, 16, 4096, NULL, error, sizeof(error)));
    ck_assert_str_eq(error, "asset_cache_limit_exceeded");
}
END_TEST

START_TEST(normalizes_audio_asset_to_target_rms)
{
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/audio_radio_demo.yaml", &scenario, error, sizeof(error)), "%s", error);
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
    ck_assert_msg(scenario_load("simulator/scenarios/audio_radio_demo.yaml", &scenario, error, sizeof(error)), "%s", error);
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
    ck_assert_msg(scenario_load("simulator/scenarios/audio_radio_demo.yaml", &scenario, error, sizeof(error)), "%s", error);
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
    ck_assert_msg(scenario_load("simulator/scenarios/audio_radio_demo.yaml", &scenario, error, sizeof(error)), "%s", error);
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

/* Largest adjacent-sample jump in the interior of a prerendered IQ buffer, and the jump across
 * the wrap seam (last sample -> first). A loop-clean buffer's seam must look like any other
 * adjacent-sample step. */
static void prerender_seam_jumps(const cached_prerender_t *pr, double *interior_max, double *seam)
{
    *interior_max = 0.0;
    for (uint64_t i = 0; i + 1U < pr->sample_count; i++) {
        const double di = (double)pr->samples[i + 1U].i - (double)pr->samples[i].i;
        const double dq = (double)pr->samples[i + 1U].q - (double)pr->samples[i].q;
        const double d = sqrt(di * di + dq * dq);
        if (d > *interior_max) {
            *interior_max = d;
        }
    }
    const uint64_t last = pr->sample_count - 1U;
    const double di = (double)pr->samples[0].i - (double)pr->samples[last].i;
    const double dq = (double)pr->samples[0].q - (double)pr->samples[last].q;
    *seam = sqrt(di * di + dq * dq);
}

START_TEST(loop_conditioned_prerender_wraps_continuously)
{
    /* 1 s of 48 kHz audio with a strong DC offset chosen so the raw FM phase integral ends half
     * a carrier cycle off (75000 * 0.2531 = 18982.5 cycles): the unconditioned seam is a near
     *-maximal phase jump, the loop-conditioned one must be an ordinary adjacent-sample step.
     * The 217 Hz tone completes whole cycles over the clip so only the DC drives the residual. */
    enum { AUDIO_COUNT = 48000 };
    static float audio[AUDIO_COUNT];
    for (size_t i = 0; i < AUDIO_COUNT; i++) {
        audio[i] = (float)(0.2531 + 0.5 * sin(2.0 * M_PI * 217.0 * (double)i / 48000.0));
    }
    scenario_signal_t signal;
    memset(&signal, 0, sizeof(signal));
    signal.modulation = SCENARIO_MODULATION_WBFM;
    signal.fm_deviation_hz = 75000.0;
    signal.bandwidth_hz = 200000;
    char error[128];

    /* Burst reference: full-length buffer, discontinuous seam (it never wraps, so that's fine). */
    signal.loop = false;
    signal.repeat_interval_s = 10.0;
    cached_prerender_t burst;
    ck_assert_msg(asset_cache_prerender_from_audio(&burst, audio, AUDIO_COUNT, 48000, &signal, NULL, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(burst.sample_count, (uint64_t)llround((double)AUDIO_COUNT * (double)burst.sample_rate_hz / 48000.0));

    /* Continuous: the loop is shorter by the seam crossfade, and wraps cleanly. */
    signal.loop = true;
    signal.repeat_interval_s = 0.0;
    cached_prerender_t looped;
    ck_assert_msg(asset_cache_prerender_from_audio(&looped, audio, AUDIO_COUNT, 48000, &signal, NULL, error, sizeof(error)), "%s", error);
    const uint64_t fade = (uint64_t)llround(AUDIO_LOOP_CROSSFADE_S * 48000.0);
    ck_assert_uint_eq(looped.sample_count, (uint64_t)llround((double)(AUDIO_COUNT - fade) * (double)looped.sample_rate_hz / 48000.0));

    double interior = 0.0;
    double seam = 0.0;
    prerender_seam_jumps(&burst, &interior, &seam);
    ck_assert_msg(seam > 1.3 * interior, "burst seam %.0f vs interior %.0f: DC offset should break the raw seam", seam, interior);
    prerender_seam_jumps(&looped, &interior, &seam);
    ck_assert_msg(seam <= interior + 4.0, "loop seam %.0f vs interior %.0f: conditioned seam must be an ordinary step", seam, interior);

    free(burst.samples);
    free(looped.samples);
}
END_TEST

START_TEST(loop_conditioned_am_envelope_is_seam_continuous)
{
    /* Same construction for AM: the envelope (I channel; Q stays 0) must wrap without a step. */
    enum { AUDIO_COUNT = 48000 };
    static float audio[AUDIO_COUNT];
    for (size_t i = 0; i < AUDIO_COUNT; i++) {
        audio[i] = (float)(0.2531 + 0.5 * sin(2.0 * M_PI * 217.0 * (double)i / 48000.0));
    }
    scenario_signal_t signal;
    memset(&signal, 0, sizeof(signal));
    signal.modulation = SCENARIO_MODULATION_AM;
    signal.am_depth = 0.8;
    signal.bandwidth_hz = 10000;
    signal.loop = true;
    char error[128];
    cached_prerender_t pr;
    ck_assert_msg(asset_cache_prerender_from_audio(&pr, audio, AUDIO_COUNT, 48000, &signal, NULL, error, sizeof(error)), "%s", error);
    double interior = 0.0;
    double seam = 0.0;
    prerender_seam_jumps(&pr, &interior, &seam);
    /* The last pre-render sample sits half an audio sample before the clip end and the linear
     * interpolator clamps there, so the seam step spans up to ~1.5 audio samples vs the 0.5 of
     * interior steps: allow 3x. A genuine envelope discontinuity would be orders larger. */
    ck_assert_msg(seam <= 3.0 * interior + 4.0, "AM seam %.0f vs interior %.0f", seam, interior);
    ck_assert_int_eq(pr.samples[0].q, 0);
    free(pr.samples);
}
END_TEST

START_TEST(rejects_prerender_memory_limit)
{
    /* 8 MB fits the ~5.9 MB audio asset + helpers but not the ~12 MB WBFM pre-render, so the
     * limit is enforced against the pre-render (not just the raw source). */
    scenario_t scenario;
    asset_cache_t cache;
    char error[128];
    ck_assert_msg(scenario_load("simulator/scenarios/audio_radio_demo.yaml", &scenario, error, sizeof(error)), "%s", error);
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
    tcase_add_test(tc, mmaps_iq_asset_over_memory_limit);
    tcase_add_test(tc, rejects_audio_asset_over_memory_limit);
    tcase_add_test(tc, normalizes_audio_asset_to_target_rms);
    tcase_add_test(tc, prerenders_audio_signals_at_content_bandwidth_rate);
    tcase_add_test(tc, prerender_gain_reconstructs_direct_am_amplitude);
    tcase_add_test(tc, prerender_is_deterministic_across_loads);
    tcase_add_test(tc, loop_conditioned_prerender_wraps_continuously);
    tcase_add_test(tc, loop_conditioned_am_envelope_is_seam_continuous);
    tcase_add_test(tc, rejects_prerender_memory_limit);
    suite_add_tcase(suite, tc);
    return suite;
}
