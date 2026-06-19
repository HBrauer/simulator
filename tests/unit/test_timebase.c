#include "receiver.h"
#include "test_suites.h"
#include "timebase.h"

#include <check.h>

START_TEST(override_returns_fixed_scenario_time)
{
    timebase_t tb;
    timebase_init(&tb);
    timebase_set_override(&tb, 123456789ULL);
    ck_assert_uint_eq(timebase_now_ns(&tb), 123456789ULL);
}
END_TEST

START_TEST(scanner_is_deterministic)
{
    receiver_config_t r = {
        .frequency_start_hz = 9960000000ULL,
        .frequency_stop_hz = 10060000000ULL,
        .scan_rate_hz_per_s = 100000000000.0,
    };
    ck_assert_uint_eq(receiver_center_frequency_hz(&r, 500000ULL), 10010000000ULL);
}
END_TEST

Suite *timebase_suite(void)
{
    Suite *suite = suite_create("timebase");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, override_returns_fixed_scenario_time);
    tcase_add_test(tc, scanner_is_deterministic);
    suite_add_tcase(suite, tc);
    return suite;
}
