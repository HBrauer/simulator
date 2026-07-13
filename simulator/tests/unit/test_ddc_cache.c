#include "ddc_cache.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Shared fixture: a 1.024 MS/s source loop of 8192 samples (8 ms, so shifts snap to
 * multiples of 125 Hz) carrying a complex tone at +10 kHz, decimated 8:1 to a 128 kS/s
 * intermediate. All tone/shift frequencies are chosen to be loop-periodic so DFT bins are
 * exact. */
#define TEST_SOURCE_RATE 1024000U
#define TEST_INTERMEDIATE_RATE 128000U
#define TEST_LOOP_SAMPLES 8192U
#define TEST_TONE_HZ 10000.0
#define TEST_TONE_AMPLITUDE 16000.0

static iq_ci16_t g_source[TEST_LOOP_SAMPLES];
static ddc_plan_t g_front_plan;

static void fill_source_tone(void)
{
    for (size_t p = 0; p < TEST_LOOP_SAMPLES; p++) {
        const double phase = 2.0 * M_PI * TEST_TONE_HZ * (double)p / (double)TEST_SOURCE_RATE;
        g_source[p].i = (int16_t)lrint(TEST_TONE_AMPLITUDE * cos(phase));
        g_source[p].q = (int16_t)lrint(TEST_TONE_AMPLITUDE * sin(phase));
    }
}

static const ddc_plan_t *front_plan(void)
{
    if (g_front_plan.ratio == 0) {
        ck_assert(ddc_plan_design(&g_front_plan, TEST_SOURCE_RATE, TEST_INTERMEDIATE_RATE,
                                  ddc_front_bandwidth_hz(TEST_INTERMEDIATE_RATE),
                                  DDC_STOPBAND_DB, NULL, 0));
        ck_assert_uint_eq(g_front_plan.ratio, 8);
    }
    return &g_front_plan;
}

START_TEST(build_shifts_and_decimates_one_seamless_loop)
{
    fill_source_tone();
    ddc_cache_t *cache = ddc_cache_create(1U << 20);
    ck_assert_ptr_nonnull(cache);

    const ddc_cache_entry_t *entry = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        3000.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_nonnull(entry);
    ck_assert_uint_eq(entry->sample_count, TEST_LOOP_SAMPLES / 8U);
    ck_assert_double_eq_tol(entry->applied_shift_hz, 3000.0, 1e-9);

    /* The +10 kHz tone shifted by +3 kHz must sit at 13 kHz in the intermediate, at half
     * the source amplitude (DDC_CACHE_SAMPLE_SCALE), and nowhere else. */
    const size_t count = (size_t)entry->sample_count;
    const size_t tone_bin = 13000U * count / TEST_INTERMEDIATE_RATE;
    double tone_power = 0.0;
    double other_peak = 0.0;
    for (size_t bin = 0; bin < count; bin++) {
        double re = 0.0;
        double im = 0.0;
        for (size_t q = 0; q < count; q++) {
            const double angle = -2.0 * M_PI * (double)bin * (double)q / (double)count;
            const double sample_i = (double)entry->samples[q].i;
            const double sample_q = (double)entry->samples[q].q;
            re += sample_i * cos(angle) - sample_q * sin(angle);
            im += sample_i * sin(angle) + sample_q * cos(angle);
        }
        const double magnitude = hypot(re, im) / (double)count;
        if (bin == tone_bin) {
            tone_power = magnitude;
        } else if (magnitude > other_peak) {
            other_peak = magnitude;
        }
    }
    ck_assert_double_eq_tol(tone_power, DDC_CACHE_SAMPLE_SCALE * TEST_TONE_AMPLITUDE,
                            0.01 * TEST_TONE_AMPLITUDE);
    /* Everything else (images, seam artifacts) at least 60 dB down. */
    ck_assert_double_le(other_peak, tone_power * 1e-3);

    ddc_cache_release(cache, entry);
    ddc_cache_destroy(cache);
}
END_TEST

START_TEST(build_matches_direct_circular_convolution_at_the_seam)
{
    fill_source_tone();
    ddc_cache_t *cache = ddc_cache_create(1U << 20);
    const ddc_plan_t *plan = front_plan();
    const ddc_cache_entry_t *entry = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        3000.0, TEST_INTERMEDIATE_RATE, plan);
    ck_assert_ptr_nonnull(entry);
    ck_assert_uint_eq(plan->stage_count, 1); /* keeps the reference a single dot product */

    const ddc_stage_t *stage = &plan->stages[0];
    const int64_t half = (int64_t)((stage->tap_count - 1U) / 2U);
    const int64_t loop = TEST_LOOP_SAMPLES;
    /* Outputs whose windows straddle the loop seam, plus one interior. */
    const uint64_t probes[] = {0, 5, entry->sample_count - 1};
    for (size_t t = 0; t < sizeof(probes) / sizeof(probes[0]); t++) {
        const int64_t q = (int64_t)probes[t];
        double acc_i = 0.0;
        double acc_q = 0.0;
        for (uint32_t j = 0; j < stage->tap_count; j++) {
            const int64_t p_raw = q * 8 - half + (int64_t)j;
            const int64_t p = ((p_raw % loop) + loop) % loop;
            const double phase =
                2.0 * M_PI * entry->applied_shift_hz * (double)p / (double)TEST_SOURCE_RATE;
            const double rotated_i =
                (double)g_source[p].i * cos(phase) - (double)g_source[p].q * sin(phase);
            const double rotated_q =
                (double)g_source[p].i * sin(phase) + (double)g_source[p].q * cos(phase);
            acc_i += (double)stage->taps[j] * rotated_i;
            acc_q += (double)stage->taps[j] * rotated_q;
        }
        ck_assert_double_le(fabs((double)entry->samples[q].i - DDC_CACHE_SAMPLE_SCALE * acc_i), 2.0);
        ck_assert_double_le(fabs((double)entry->samples[q].q - DDC_CACHE_SAMPLE_SCALE * acc_q), 2.0);
    }

    ddc_cache_release(cache, entry);
    ddc_cache_destroy(cache);
}
END_TEST

START_TEST(nearby_shifts_snap_to_the_same_entry)
{
    fill_source_tone();
    ddc_cache_t *cache = ddc_cache_create(1U << 20);
    const ddc_cache_entry_t *first = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        3000.0, TEST_INTERMEDIATE_RATE, front_plan());
    /* 3010 Hz rounds to the same 24 cycles/loop (granularity 125 Hz): a detector coming
     * back "a few Hz off" reuses the entry instead of rebuilding. */
    const ddc_cache_entry_t *second = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        3010.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_nonnull(first);
    ck_assert_ptr_eq(first, second);
    ck_assert_double_eq_tol(second->applied_shift_hz, 3000.0, 1e-9);
    /* A genuinely different shift builds its own entry. */
    const ddc_cache_entry_t *third = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        6000.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_nonnull(third);
    ck_assert_ptr_ne(third, first);
    ddc_cache_release(cache, first);
    ddc_cache_release(cache, second);
    ddc_cache_release(cache, third);
    ddc_cache_destroy(cache);
}
END_TEST

START_TEST(lru_eviction_keeps_budget_and_rebuilds_on_demand)
{
    fill_source_tone();
    const size_t entry_bytes = (TEST_LOOP_SAMPLES / 8U) * sizeof(iq_ci16_t);
    ddc_cache_t *cache = ddc_cache_create(2U * entry_bytes);

    const ddc_cache_entry_t *entry_a = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        1000.0, TEST_INTERMEDIATE_RATE, front_plan());
    const ddc_cache_entry_t *entry_b = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        2000.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_nonnull(entry_a);
    ck_assert_ptr_nonnull(entry_b);
    ddc_cache_release(cache, entry_a);
    ddc_cache_release(cache, entry_b);
    ck_assert_uint_eq(ddc_cache_used_bytes(cache), 2U * entry_bytes);

    /* Touch A so B is the LRU victim, then insert C: budget must hold. */
    const ddc_cache_entry_t *entry_a_again = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        1000.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_eq(entry_a_again, entry_a);
    ddc_cache_release(cache, entry_a_again);
    const ddc_cache_entry_t *entry_c = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        3000.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_nonnull(entry_c);
    ddc_cache_release(cache, entry_c);
    ck_assert_uint_le(ddc_cache_used_bytes(cache), 2U * entry_bytes);

    /* The evicted key just rebuilds (a bounded retune stall, not an error). */
    const ddc_cache_entry_t *entry_b_rebuilt = ddc_cache_acquire(
        cache, "src", g_source, TEST_LOOP_SAMPLES, TEST_SOURCE_RATE,
        2000.0, TEST_INTERMEDIATE_RATE, front_plan());
    ck_assert_ptr_nonnull(entry_b_rebuilt);
    ddc_cache_release(cache, entry_b_rebuilt);
    ck_assert_uint_le(ddc_cache_used_bytes(cache), 2U * entry_bytes);
    ddc_cache_destroy(cache);
}
END_TEST

START_TEST(acquire_rejects_impossible_requests)
{
    fill_source_tone();
    ddc_cache_t *cache = ddc_cache_create(1U << 20);
    /* Loop length not divisible by the front decimation: a circular build would smear the
     * seam, so the caller must fall back to the direct path. */
    ck_assert_ptr_null(ddc_cache_acquire(cache, "src", g_source, TEST_LOOP_SAMPLES - 1U,
                                         TEST_SOURCE_RATE, 3000.0, TEST_INTERMEDIATE_RATE,
                                         front_plan()));
    ddc_cache_destroy(cache);
    /* Budget smaller than a single entry. */
    cache = ddc_cache_create(16);
    ck_assert_ptr_null(ddc_cache_acquire(cache, "src", g_source, TEST_LOOP_SAMPLES,
                                         TEST_SOURCE_RATE, 3000.0, TEST_INTERMEDIATE_RATE,
                                         front_plan()));
    ck_assert_uint_eq(ddc_cache_used_bytes(cache), 0);
    ddc_cache_destroy(cache);
}
END_TEST

Suite *ddc_cache_suite(void)
{
    Suite *suite = suite_create("ddc_cache");
    TCase *tc = tcase_create("core");
    tcase_set_timeout(tc, 60);
    tcase_add_test(tc, build_shifts_and_decimates_one_seamless_loop);
    tcase_add_test(tc, build_matches_direct_circular_convolution_at_the_seam);
    tcase_add_test(tc, nearby_shifts_snap_to_the_same_entry);
    tcase_add_test(tc, lru_eviction_keeps_budget_and_rebuilds_on_demand);
    tcase_add_test(tc, acquire_rejects_impossible_requests);
    suite_add_tcase(suite, tc);
    return suite;
}
