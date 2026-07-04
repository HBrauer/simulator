#include "ringbuffer.h"
#include "test_suites.h"

#include <check.h>
#include <pthread.h>
#include <stdint.h>

START_TEST(pushes_and_pops_records_with_timestamps_in_order)
{
    ringbuffer_t rb;
    ck_assert(ringbuffer_init(&rb, sizeof(uint32_t), 4));
    ck_assert_uint_eq(ringbuffer_available(&rb), 4);

    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t payload = 100U + i;
        ck_assert(ringbuffer_try_push(&rb, 1000ULL + i, &payload));
    }
    ck_assert_uint_eq(ringbuffer_fill(&rb), 3);

    for (uint32_t i = 0; i < 3; i++) {
        uint64_t ts = 0;
        uint32_t payload = 0;
        ck_assert(ringbuffer_try_pop(&rb, &ts, &payload));
        ck_assert_uint_eq(ts, 1000ULL + i);
        ck_assert_uint_eq(payload, 100U + i);
    }
    ck_assert_uint_eq(ringbuffer_fill(&rb), 0);
    ringbuffer_free(&rb);
}
END_TEST

START_TEST(push_fails_when_full_pop_fails_when_empty)
{
    ringbuffer_t rb;
    ck_assert(ringbuffer_init(&rb, sizeof(uint32_t), 2));
    uint32_t payload = 7;
    uint64_t ts = 0;

    ck_assert(!ringbuffer_try_pop(&rb, &ts, &payload));
    ck_assert(ringbuffer_try_push(&rb, 1, &payload));
    ck_assert(ringbuffer_try_push(&rb, 2, &payload));
    ck_assert(!ringbuffer_try_push(&rb, 3, &payload));
    ck_assert_uint_eq(ringbuffer_available(&rb), 0);

    ck_assert(ringbuffer_try_pop(&rb, &ts, &payload));
    ck_assert(ringbuffer_try_push(&rb, 4, &payload));
    ringbuffer_free(&rb);
}
END_TEST

START_TEST(drain_discards_buffered_records)
{
    ringbuffer_t rb;
    ck_assert(ringbuffer_init(&rb, sizeof(uint32_t), 4));
    uint32_t payload = 1;
    ck_assert(ringbuffer_try_push(&rb, 1, &payload));
    ck_assert(ringbuffer_try_push(&rb, 2, &payload));
    ringbuffer_drain(&rb);
    ck_assert_uint_eq(ringbuffer_fill(&rb), 0);
    ck_assert_uint_eq(ringbuffer_available(&rb), 4);
    uint64_t ts = 0;
    ck_assert(!ringbuffer_try_pop(&rb, &ts, &payload));
    ringbuffer_free(&rb);
}
END_TEST

#define SPSC_TOTAL 300000ULL

typedef struct {
    ringbuffer_t *rb;
    uint64_t count;
} spsc_producer_args_t;

static void *spsc_producer(void *arg)
{
    spsc_producer_args_t *a = arg;
    uint64_t sent = 0;
    while (sent < a->count) {
        /* Payload is a sequence number so the consumer can verify ordering and integrity. */
        if (ringbuffer_try_push(a->rb, sent, &sent)) {
            sent++;
        }
    }
    return NULL;
}

START_TEST(spsc_stress_preserves_order_and_integrity)
{
    ringbuffer_t rb;
    ck_assert(ringbuffer_init(&rb, sizeof(uint64_t), 8));
    spsc_producer_args_t args = {.rb = &rb, .count = SPSC_TOTAL};
    pthread_t producer;
    ck_assert_int_eq(pthread_create(&producer, NULL, spsc_producer, &args), 0);

    uint64_t received = 0;
    while (received < SPSC_TOTAL) {
        uint64_t ts = 0;
        uint64_t payload = 0;
        if (ringbuffer_try_pop(&rb, &ts, &payload)) {
            ck_assert_uint_eq(payload, received); /* strictly in order, no gaps or tears */
            ck_assert_uint_eq(ts, received);
            received++;
        }
    }
    pthread_join(producer, NULL);
    ck_assert_uint_eq(received, SPSC_TOTAL);
    ringbuffer_free(&rb);
}
END_TEST

Suite *ringbuffer_suite(void)
{
    Suite *suite = suite_create("ringbuffer");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, pushes_and_pops_records_with_timestamps_in_order);
    tcase_add_test(tc, push_fails_when_full_pop_fails_when_empty);
    tcase_add_test(tc, drain_discards_buffered_records);
    tcase_set_timeout(tc, 60); /* the SPSC stress test busy-polls; generous under sanitizers */
    tcase_add_test(tc, spsc_stress_preserves_order_and_integrity);
    suite_add_tcase(suite, tc);
    return suite;
}
