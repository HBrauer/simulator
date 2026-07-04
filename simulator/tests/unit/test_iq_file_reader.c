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
    ck_assert(!iq_signal_active(&signal, &source, 0.5, &offset, &fraction));
    ck_assert(iq_signal_active(&signal, &source, 1.2, &offset, &fraction));
    ck_assert_uint_eq(offset, 1);
    ck_assert(fraction >= 0.0 && fraction < 1.0);
    ck_assert(!iq_signal_active(&signal, &source, 1.7, &offset, &fraction));
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
    ck_assert(iq_signal_active(&signal, &source, 0.30, &offset, &fraction));
    ck_assert_uint_eq(offset, 1);
    ck_assert(fabs(fraction - 0.2) < 1e-6);
    /* NULL fraction pointer is accepted. */
    ck_assert(iq_signal_active(&signal, &source, 0.30, &offset, NULL));
}
END_TEST

Suite *iq_file_reader_suite(void)
{
    Suite *suite = suite_create("iq_file_reader");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, opens_generated_asset);
    tcase_add_test(tc, signal_activity_uses_repeat_window);
    tcase_add_test(tc, signal_activity_reports_fractional_sample_offset);
    suite_add_tcase(suite, tc);
    return suite;
}
