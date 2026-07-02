#include "streamer.h"
#include "test_suites.h"

#include <check.h>

START_TEST(calculates_80mhz_packet_pacing_duration)
{
    const uint64_t duration_ns = streamer_block_duration_ns(1024, SIM_RECEIVER_SAMPLE_RATE_HZ);

    ck_assert_uint_ge(duration_ns, 10416);
    ck_assert_uint_le(duration_ns, 10417);
}
END_TEST

START_TEST(calculates_ddc_packet_pacing_duration)
{
    const uint64_t duration_ns = streamer_block_duration_ns(256, SIM_DDC_SAMPLE_RATE_HZ);

    ck_assert_uint_ge(duration_ns, 10416);
    ck_assert_uint_le(duration_ns, 10417);
}
END_TEST

START_TEST(rejects_empty_pacing_inputs)
{
    ck_assert_uint_eq(streamer_block_duration_ns(0, SIM_RECEIVER_SAMPLE_RATE_HZ), 0);
    ck_assert_uint_eq(streamer_block_duration_ns(1024, 0), 0);
}
END_TEST

Suite *streamer_suite(void)
{
    Suite *suite = suite_create("streamer");
    TCase *tc = tcase_create("pacing");
    tcase_add_test(tc, calculates_80mhz_packet_pacing_duration);
    tcase_add_test(tc, calculates_ddc_packet_pacing_duration);
    tcase_add_test(tc, rejects_empty_pacing_inputs);
    suite_add_tcase(suite, tc);
    return suite;
}
