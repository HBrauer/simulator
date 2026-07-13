#include "ddc.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* Composite cascade magnitude response at an absolute input frequency, in dB. Each stage's
 * DTFT is periodic in its own input rate, so evaluating every stage at the raw frequency
 * automatically accounts for how decimation folds that frequency down the cascade -- the
 * product is exactly the gain with which `frequency_hz` lands in the final output. */
static double plan_response_db(const ddc_plan_t *plan, double frequency_hz)
{
    double rate_hz = (double)plan->source_rate_hz;
    double magnitude = 1.0;
    for (size_t k = 0; k < plan->stage_count; k++) {
        const ddc_stage_t *stage = &plan->stages[k];
        double re = 0.0;
        double im = 0.0;
        for (uint32_t j = 0; j < stage->tap_count; j++) {
            const double angle = -2.0 * M_PI * frequency_hz * (double)j / rate_hz;
            re += (double)stage->taps[j] * cos(angle);
            im += (double)stage->taps[j] * sin(angle);
        }
        magnitude *= hypot(re, im);
        rate_hz /= (double)stage->decim;
    }
    return 20.0 * log10(magnitude + 1e-300);
}

static void assert_plan_structure(const ddc_plan_t *plan)
{
    ck_assert_uint_ge(plan->stage_count, 1);
    ck_assert_uint_le(plan->stage_count, DDC_MAX_STAGES);
    uint64_t product = 1;
    for (size_t k = 0; k < plan->stage_count; k++) {
        const ddc_stage_t *stage = &plan->stages[k];
        const bool is_final = k + 1 == plan->stage_count;
        ck_assert_uint_ge(stage->decim, 2);
        ck_assert_uint_eq(stage->tap_count & 1U, 1U); /* odd, so the center tap is exact */
        ck_assert_uint_le(stage->tap_count, is_final ? DDC_MAX_STAGE_TAPS : DDC_NONFINAL_STAGE_TAPS);
        product *= stage->decim;
        double sum = 0.0;
        for (uint32_t j = 0; j < stage->tap_count; j++) {
            /* Symmetric (linear phase), so decimated output stays aligned to the center tap. */
            ck_assert_double_eq_tol((double)stage->taps[j],
                                    (double)stage->taps[stage->tap_count - 1U - j], 1e-9);
            sum += (double)stage->taps[j];
        }
        ck_assert_double_eq_tol(sum, 1.0, 1e-5); /* unity DC gain */
    }
    ck_assert_uint_eq(product, plan->ratio);
    ck_assert_uint_gt(plan->history_source_samples, 0);
}

START_TEST(design_768_ratio_has_valid_structure)
{
    ddc_plan_t plan;
    char error[32] = "";
    ck_assert(ddc_plan_design(&plan, 98304000U, 128000U, 100000U, DDC_STOPBAND_DB, error, sizeof(error)));
    ck_assert_uint_eq(plan.ratio, 768);
    assert_plan_structure(&plan);
    /* The whole point of the cascade: composite reach far beyond the old 256-tap resampler,
     * yet cheap because most of it sits behind the early decimations. */
    ck_assert_uint_gt(plan.history_source_samples, 256);
}
END_TEST

START_TEST(design_768_ratio_passband_flat_and_images_rejected)
{
    ddc_plan_t plan;
    ck_assert(ddc_plan_design(&plan, 98304000U, 128000U, 100000U, DDC_STOPBAND_DB, NULL, 0));

    /* Passband: flat to well under 0.5 dB out to 90% of the half-bandwidth. */
    ck_assert_double_eq_tol(plan_response_db(&plan, 0.0), 0.0, 0.01);
    ck_assert_double_le(fabs(plan_response_db(&plan, 20000.0)), 0.5);
    ck_assert_double_le(fabs(plan_response_db(&plan, 45000.0)), 0.5);

    /* Every image of every in-band frequency must be rejected: input m*output_rate + g folds
     * onto g. Sweep all image bands up to the source Nyquist at the band edges and center. */
    const double offsets[] = {-45000.0, 0.0, 45000.0};
    const double nyquist = 98304000.0 / 2.0;
    for (uint32_t m = 1; ; m++) {
        const double image_hz = (double)m * 128000.0;
        if (image_hz + 45000.0 > nyquist) {
            break;
        }
        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            const double response_db = plan_response_db(&plan, image_hz + offsets[i]);
            ck_assert_msg(response_db <= -70.0,
                          "image at %.0f Hz only %.1f dB down", image_hz + offsets[i], response_db);
        }
    }
}
END_TEST

START_TEST(design_handles_huge_ratio)
{
    /* 98.304 MS/s -> 1.28 kS/s (ratio 76800): the "1 kHz channel from an 80 MHz recording"
     * end of the use case. */
    ddc_plan_t plan;
    char error[32] = "";
    ck_assert(ddc_plan_design(&plan, 98304000U, 1280U, 1000U, DDC_STOPBAND_DB, error, sizeof(error)));
    ck_assert_uint_eq(plan.ratio, 76800);
    assert_plan_structure(&plan);

    ck_assert_double_eq_tol(plan_response_db(&plan, 0.0), 0.0, 0.01);
    ck_assert_double_le(fabs(plan_response_db(&plan, 450.0)), 0.5);
    /* Spot-check image bands across the whole decimation range (a full sweep would be
     * 38400 bands). */
    const uint32_t image_bands[] = {1, 2, 3, 7, 100, 1537, 19200, 38399};
    for (size_t i = 0; i < sizeof(image_bands) / sizeof(image_bands[0]); i++) {
        const double image_hz = (double)image_bands[i] * 1280.0;
        for (int offset = -450; offset <= 450; offset += 450) {
            const double response_db = plan_response_db(&plan, image_hz + (double)offset);
            ck_assert_msg(response_db <= -70.0,
                          "image at %.0f Hz only %.1f dB down", image_hz + (double)offset, response_db);
        }
    }
}
END_TEST

START_TEST(design_covers_cached_intermediate_front_and_tail)
{
    /* The two-hop split for 98.304 MS/s -> 128 kS/s: front to the cached intermediate rate
     * protecting its full usable band, tail from the intermediate to the channel. */
    const uint32_t intermediate_hz = ddc_intermediate_rate_hz(98304000U, 128000U);
    ck_assert_uint_eq(intermediate_hz, 2048000U);

    ddc_plan_t front;
    ck_assert(ddc_plan_design(&front, 98304000U, intermediate_hz,
                              ddc_front_bandwidth_hz(intermediate_hz), DDC_STOPBAND_DB, NULL, 0));
    assert_plan_structure(&front);
    ddc_plan_t tail;
    ck_assert(ddc_plan_design(&tail, intermediate_hz, 128000U, 100000U, DDC_STOPBAND_DB, NULL, 0));
    assert_plan_structure(&tail);

    /* Front passband must be flat across everything a tail channel can ask of it: channel
     * half-bandwidth plus the worst-case grid residual. */
    const double worst_edge_hz = 50000.0 + (double)ddc_grid_step_hz(intermediate_hz) / 2.0;
    ck_assert_double_le(worst_edge_hz, (double)ddc_front_bandwidth_hz(intermediate_hz) / 2.0);
    ck_assert_double_le(fabs(plan_response_db(&front, worst_edge_hz)), 0.5);
}
END_TEST

START_TEST(design_rejects_invalid_requests)
{
    ddc_plan_t plan;
    char error[32] = "";
    /* Non-integer ratio. */
    ck_assert(!ddc_plan_design(&plan, 98304000U, 100000U, 80000U, DDC_STOPBAND_DB, error, sizeof(error)));
    ck_assert_str_eq(error, "non_integer_ratio");
    /* Bandwidth must fit under the output rate. */
    ck_assert(!ddc_plan_design(&plan, 98304000U, 128000U, 128000U, DDC_STOPBAND_DB, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_bandwidth");
    ck_assert(!ddc_plan_design(&plan, 98304000U, 128000U, 0U, DDC_STOPBAND_DB, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_bandwidth");
    /* Ratio 1 has nothing to decimate. */
    ck_assert(!ddc_plan_design(&plan, 128000U, 128000U, 100000U, DDC_STOPBAND_DB, error, sizeof(error)));
    ck_assert_str_eq(error, "ratio_too_small");
}
END_TEST

START_TEST(plan_cache_reuses_built_plans)
{
    const ddc_plan_t *first = ddc_plan_cache_get(98304000U, 128000U, 100000U);
    ck_assert_ptr_nonnull(first);
    ck_assert_ptr_eq(ddc_plan_cache_get(98304000U, 128000U, 100000U), first);
    /* A different key gets its own plan; an undesignable key gets NULL, repeatably. */
    const ddc_plan_t *other = ddc_plan_cache_get(98304000U, 768000U, 500000U);
    ck_assert_ptr_nonnull(other);
    ck_assert_ptr_ne(other, first);
    ck_assert_ptr_null(ddc_plan_cache_get(98304000U, 100000U, 80000U));
}
END_TEST

START_TEST(intermediate_rate_divides_source_and_bounds_tail)
{
    /* ratio 768 -> smallest tail ratio >= 16 is 16 itself. */
    ck_assert_uint_eq(ddc_intermediate_rate_hz(98304000U, 128000U), 2048000U);
    /* Small ratios are not worth caching. */
    ck_assert_uint_eq(ddc_intermediate_rate_hz(98304000U, 24576000U), 0U);
    /* Prime ratio 17: no split leaves both a tail >= 16 and a front >= 4. */
    ck_assert_uint_eq(ddc_intermediate_rate_hz(2176000U, 128000U), 0U);
    /* Non-integer ratio never qualifies. */
    ck_assert_uint_eq(ddc_intermediate_rate_hz(98304000U, 100000U), 0U);

    const uint32_t intermediate_hz = ddc_intermediate_rate_hz(98304000U, 1280U);
    ck_assert_uint_gt(intermediate_hz, 0U);
    ck_assert_uint_eq(98304000U % intermediate_hz, 0U);
    ck_assert_uint_eq(intermediate_hz % 1280U, 0U);
    ck_assert_uint_ge(intermediate_hz / 1280U, 16U);
    ck_assert_uint_ge(98304000U / intermediate_hz, 4U);
}
END_TEST

START_TEST(grid_center_quantizes_to_nearest_step)
{
    const uint32_t step = ddc_grid_step_hz(2048000U);
    ck_assert_uint_eq(step, 409600U);
    /* The 119.4 MHz example tune from the use case. */
    const int64_t grid = ddc_grid_center_hz(119400000, step);
    ck_assert_int_eq(grid % (int64_t)step, 0);
    ck_assert_int_le(llabs(119400000 - grid), (int64_t)step / 2);
    /* A grid point maps to itself, and nearby tunes (the detector's "few Hz off" revisit)
     * land on the same entry. */
    ck_assert_int_eq(ddc_grid_center_hz(grid, step), grid);
    ck_assert_int_eq(ddc_grid_center_hz(119400000 + 37, step), grid);
    ck_assert_int_eq(ddc_grid_center_hz(119400000 - 91, step), grid);
    /* Negative centers still round to the nearest multiple. */
    ck_assert_int_eq(ddc_grid_center_hz(-300000, step), -409600);
    ck_assert_int_eq(ddc_grid_center_hz(-1000, step), 0);
}
END_TEST

Suite *ddc_suite(void)
{
    Suite *suite = suite_create("ddc");
    TCase *tc = tcase_create("design");
    tcase_set_timeout(tc, 60); /* the full 768-band image sweep under sanitizers */
    tcase_add_test(tc, design_768_ratio_has_valid_structure);
    tcase_add_test(tc, design_768_ratio_passband_flat_and_images_rejected);
    tcase_add_test(tc, design_handles_huge_ratio);
    tcase_add_test(tc, design_covers_cached_intermediate_front_and_tail);
    tcase_add_test(tc, design_rejects_invalid_requests);
    tcase_add_test(tc, plan_cache_reuses_built_plans);
    tcase_add_test(tc, intermediate_rate_divides_source_and_bounds_tail);
    tcase_add_test(tc, grid_center_quantizes_to_nearest_step);
    suite_add_tcase(suite, tc);
    return suite;
}
