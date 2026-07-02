#include "ringbuffer.h"
#include "test_suites.h"

#include <check.h>

START_TEST(wraps_and_preserves_order)
{
    ringbuffer_t rb;
    ck_assert(ringbuffer_init(&rb, 4));
    const uint8_t in1[] = {1, 2, 3};
    const uint8_t in2[] = {4, 5};
    uint8_t out[4] = {0};
    ck_assert_uint_eq(ringbuffer_write(&rb, in1, sizeof(in1)), 3);
    ck_assert_uint_eq(ringbuffer_available(&rb), 1);
    ck_assert_uint_eq(ringbuffer_read(&rb, out, 2), 2);
    ck_assert_uint_eq(ringbuffer_fill(&rb), 1);
    ck_assert_uint_eq(out[0], 1);
    ck_assert_uint_eq(out[1], 2);
    ck_assert_uint_eq(ringbuffer_write(&rb, in2, sizeof(in2)), 2);
    ck_assert_uint_eq(ringbuffer_read(&rb, out, 3), 3);
    ck_assert_uint_eq(out[0], 3);
    ck_assert_uint_eq(out[1], 4);
    ck_assert_uint_eq(out[2], 5);
    ringbuffer_free(&rb);
}
END_TEST

START_TEST(clear_discards_buffered_data)
{
    ringbuffer_t rb;
    ck_assert(ringbuffer_init(&rb, 4));
    const uint8_t in[] = {1, 2, 3};
    uint8_t out[2] = {0};

    ck_assert_uint_eq(ringbuffer_write(&rb, in, sizeof(in)), 3);
    ringbuffer_clear(&rb);
    ck_assert_uint_eq(ringbuffer_fill(&rb), 0);
    ck_assert_uint_eq(ringbuffer_available(&rb), 4);
    ck_assert_uint_eq(ringbuffer_read(&rb, out, sizeof(out)), 0);

    ringbuffer_free(&rb);
}
END_TEST

Suite *ringbuffer_suite(void)
{
    Suite *suite = suite_create("ringbuffer");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, wraps_and_preserves_order);
    tcase_add_test(tc, clear_discards_buffered_data);
    suite_add_tcase(suite, tc);
    return suite;
}
