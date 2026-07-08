#include "iq_file_reader.h"
#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <math.h>

START_TEST(opens_generated_asset)
{
    iq_file_reader_t reader;
    char error[128];
    ck_assert_msg(iq_file_reader_open(&reader, "simulator/assets/fsk_20mhz.c16", 24576, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(reader.sample_count, 24576);
    iq_ci16_t samples[4];
    size_t read_count = 0;
    ck_assert(iq_file_reader_read(&reader, 0, samples, 4, &read_count));
    ck_assert_uint_eq(read_count, 4);
    ck_assert_int_ne(samples[0].i, 0);
    iq_file_reader_close(&reader);
}
END_TEST

START_TEST(signal_activity_uses_repeat_window)
{
    scenario_source_t source = {.sample_rate_hz = 10, .sample_count = 5};
    scenario_signal_t signal = {.start_time_s = 1.0, .repeat_interval_s = 10.0};
    uint64_t offset = 999;
    double fraction = -1.0;
    ck_assert(!iq_signal_active(&signal, &source, 500000000ULL, &offset, &fraction));
    ck_assert(iq_signal_active(&signal, &source, 1200000000ULL, &offset, &fraction));
    /* 0.2 s into a 10 Hz source is exactly sample 2 (the old double math rounded this to
     * sample 1 with fraction ~1.0). */
    ck_assert_uint_eq(offset, 2);
    ck_assert(fraction < 1e-9);
    ck_assert(!iq_signal_active(&signal, &source, 1700000000ULL, &offset, &fraction));
}
END_TEST

START_TEST(signal_activity_reports_fractional_sample_offset)
{
    /* 0.25 s into a 4 Hz source lands exactly on sample 1.0 (fraction ~0); 0.30 s lands on
     * sample 1.2 -> integer offset 1, fraction ~0.2. The fraction lets the resampler start at
     * the exact sub-sample position instead of snapping to an integer each block. */
    scenario_source_t source = {.sample_rate_hz = 4, .sample_count = 100};
    scenario_signal_t signal = {.start_time_s = 0.0, .repeat_interval_s = 100.0};
    uint64_t offset = 0;
    double fraction = -1.0;
    ck_assert(iq_signal_active(&signal, &source, 300000000ULL, &offset, &fraction));
    ck_assert_uint_eq(offset, 1);
    ck_assert(fabs(fraction - 0.2) < 1e-6);
    /* NULL fraction pointer is accepted. */
    ck_assert(iq_signal_active(&signal, &source, 300000000ULL, &offset, NULL));
}
END_TEST

START_TEST(signal_activity_is_exact_at_repeat_boundaries)
{
    /* A continuous asset (duration == repeat interval) restarts at offset 0 on every cycle
     * boundary, even deep into the day. The old double fmod() could land the boundary at
     * sample_count - 1 instead, silencing one block per repeat (periodic level dips on
     * decimated channels). 24.576 MS/s asset, 0.1 s cycle, boundaries at m * 1e8 ns. */
    scenario_source_t source = {.sample_rate_hz = 24576000, .sample_count = 2457600};
    scenario_signal_t signal = {.start_time_s = 0.0, .repeat_interval_s = 0.1};
    for (uint64_t m = 1; m < 864000ULL; m += 7919ULL) { /* spread over a full day */
        const uint64_t boundary_ns = m * 100000000ULL;
        uint64_t offset = 999;
        double fraction = -1.0;
        ck_assert(iq_signal_active(&signal, &source, boundary_ns, &offset, &fraction));
        ck_assert_uint_eq(offset, 0);
        ck_assert(fraction < 1e-9);
        /* One nanosecond earlier is still inside the previous cycle, near its end. */
        ck_assert(iq_signal_active(&signal, &source, boundary_ns - 1ULL, &offset, &fraction));
        ck_assert_uint_eq(offset, source.sample_count - 1ULL);
    }
}
END_TEST

START_TEST(loop_position_wraps_exactly_at_file_length)
{
    /* Equal rates: the fraction is always exactly 0 and the position is the absolute output
     * sample index modulo the file length -- including exactly at every wrap boundary. */
    scenario_source_t source = {.sample_rate_hz = 98304000, .sample_count = 4915200};
    scenario_signal_t signal = {.start_time_s = 0.0};
    for (uint64_t k = 1; k < 2000ULL; k += 37ULL) {
        const uint64_t boundary = k * source.sample_count;
        uint64_t offset = 999;
        double fraction = -1.0;
        uint64_t until_wrap = 0;
        ck_assert(iq_signal_loop_position(&signal, &source, boundary, 98304000U, &offset, &fraction, &until_wrap));
        ck_assert_uint_eq(offset, 0);
        ck_assert(fraction < 1e-12);
        ck_assert_uint_eq(until_wrap, source.sample_count);
        ck_assert(iq_signal_loop_position(&signal, &source, boundary - 1ULL, 98304000U, &offset, &fraction, &until_wrap));
        ck_assert_uint_eq(offset, source.sample_count - 1ULL);
        ck_assert_uint_eq(until_wrap, 1);
    }
}
END_TEST

START_TEST(loop_position_is_pure_function_of_grid_index)
{
    /* The cross-instance property: the position depends only on the absolute output sample
     * index, so a process started later computes the same file offset for the same index. */
    scenario_source_t source = {.sample_rate_hz = 1000000, .sample_count = 50000};
    scenario_signal_t signal = {.start_time_s = 0.0};
    const uint64_t grid_index = 123456789012ULL; /* some instant deep into the day */
    uint64_t offset_a = 0, offset_b = 999;
    double fraction_a = -1.0, fraction_b = -2.0;
    ck_assert(iq_signal_loop_position(&signal, &source, grid_index, 1536000U, &offset_a, &fraction_a, NULL));
    ck_assert(iq_signal_loop_position(&signal, &source, grid_index, 1536000U, &offset_b, &fraction_b, NULL));
    ck_assert_uint_eq(offset_a, offset_b);
    ck_assert(fabs(fraction_a - fraction_b) < 1e-15);
    /* And it matches the closed form (index * source_rate / output_rate) mod file length. */
    const uint64_t expected = (uint64_t)(((__uint128_t)grid_index * 1000000ULL) / 1536000ULL) % 50000ULL;
    ck_assert_uint_eq(offset_a, expected);
}
END_TEST

START_TEST(loop_position_honors_start_time_and_resampled_fraction)
{
    /* start_time_s shifts the origin: inactive before it, sample 0 exactly at it. With
     * unequal rates the fractional position advances by source_rate/output_rate per sample. */
    scenario_source_t source = {.sample_rate_hz = 1000000, .sample_count = 50000};
    scenario_signal_t signal = {.start_time_s = 2.0};
    const uint64_t start_out = 2ULL * 1536000ULL;
    uint64_t offset = 999;
    double fraction = -1.0;
    uint64_t until_wrap = 0;
    ck_assert(!iq_signal_loop_position(&signal, &source, start_out - 1ULL, 1536000U, &offset, &fraction, NULL));
    ck_assert(iq_signal_loop_position(&signal, &source, start_out, 1536000U, &offset, &fraction, &until_wrap));
    ck_assert_uint_eq(offset, 0);
    ck_assert(fraction < 1e-12);
    ck_assert(iq_signal_loop_position(&signal, &source, start_out + 3ULL, 1536000U, &offset, &fraction, NULL));
    /* 3 output samples * (1.0/1.536) source samples each = 1.953125 */
    ck_assert_uint_eq(offset, 1);
    ck_assert(fabs(fraction - 0.953125) < 1e-12);
}
END_TEST

Suite *iq_file_reader_suite(void)
{
    Suite *suite = suite_create("iq_file_reader");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, opens_generated_asset);
    tcase_add_test(tc, signal_activity_uses_repeat_window);
    tcase_add_test(tc, signal_activity_reports_fractional_sample_offset);
    tcase_add_test(tc, signal_activity_is_exact_at_repeat_boundaries);
    tcase_add_test(tc, loop_position_wraps_exactly_at_file_length);
    tcase_add_test(tc, loop_position_is_pure_function_of_grid_index);
    tcase_add_test(tc, loop_position_honors_start_time_and_resampled_fraction);
    suite_add_tcase(suite, tc);
    return suite;
}
