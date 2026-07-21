#include "streamer.h"
#include "test_suites.h"

#include <check.h>
#include <stdint.h>

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

START_TEST(block_grid_contains_time_and_is_monotonic_without_drift)
{
    /* 96 MS/s: 1e9 is not divisible by the sample rate, so any ns-accumulating scheme drifts.
     * The exact 128-bit grid must not: block starts are strictly increasing and the block that
     * index_from_time() selects for a time t must actually contain t: start(k) <= t < start(k+1). */
    const size_t block_samples = 1024;
    const uint32_t rate = 96000000U;
    uint64_t prev_start = 0;
    for (uint64_t k = 1; k < 2000000ULL; k += 4099ULL) {
        const uint64_t start_ns = streamer_block_start_ns(k, block_samples, rate);
        ck_assert_uint_gt(start_ns, prev_start);
        prev_start = start_ns;
        /* Round-trip is exact to within the 1 ns quantisation of the block boundary (block_start
         * rounds down, so it can land in the tail of block k-1). Crucially the error does not
         * accumulate with k — no drift. */
        const uint64_t idx = streamer_block_index_from_time_ns(start_ns, block_samples, rate);
        ck_assert_uint_le(k - idx, 1ULL);
        ck_assert_uint_le(idx, k);
    }
}
END_TEST

START_TEST(block_grid_start_matches_exact_sample_math)
{
    const size_t block_samples = 1024;
    const uint32_t rate = 98304000U;
    for (uint64_t k = 0; k < 100000ULL; k += 777ULL) {
        const __uint128_t samples = (__uint128_t)k * block_samples;
        const uint64_t expected = (uint64_t)((samples * 1000000000ULL) / rate);
        ck_assert_uint_eq(streamer_block_start_ns(k, block_samples, rate), expected);
    }
}
END_TEST

START_TEST(block_grid_is_independent_of_query_phase)
{
    /* Two "instances" querying the grid at different wall-clock instants within the same
     * block must resolve to the same block index (and therefore the same rendered content). */
    const size_t block_samples = 1024;
    const uint32_t rate = 96000000U;
    const uint64_t k = 12345ULL;
    const uint64_t start = streamer_block_start_ns(k, block_samples, rate);
    const uint64_t mid = start + streamer_block_duration_ns(block_samples, rate) / 2ULL;
    ck_assert_uint_eq(streamer_block_index_from_time_ns(start, block_samples, rate), k);
    ck_assert_uint_eq(streamer_block_index_from_time_ns(mid, block_samples, rate), k);
}
END_TEST

START_TEST(low_rate_channels_shrink_block_size)
{
    /* High rates keep the configured size... */
    ck_assert_uint_eq(streamer_block_samples_for_rate(1536, 98304000U), 1536);
    ck_assert_uint_eq(streamer_block_samples_for_rate(1536, 128000U), 1536);
    /* ...low rates cap at the largest power of two keeping >= ~4 blocks/s... */
    ck_assert_uint_eq(streamer_block_samples_for_rate(1536, 16000U), 1536);
    ck_assert_uint_eq(streamer_block_samples_for_rate(1536, 6000U), 1024);
    ck_assert_uint_eq(streamer_block_samples_for_rate(1536, 2000U), 256);
    /* ...never below the 64-sample floor, and degenerate inputs pass through. */
    ck_assert_uint_eq(streamer_block_samples_for_rate(4096, 100U), 64);
    ck_assert_uint_eq(streamer_block_samples_for_rate(1536, 0U), 1536);
    ck_assert_uint_eq(streamer_block_samples_for_rate(0, 2000U), 0);
    /* A 2 kS/s DDC channel now emits 128 ms blocks instead of 768 ms ones. */
    ck_assert_uint_eq(streamer_block_duration_ns(256, 2000U), 128000000ULL);
}
END_TEST

START_TEST(block_grid_works_at_ddc_low_rates)
{
    /* The shrunken grid at a 2 kS/s DDC rate: exact round trip, no drift, block contains
     * its own start time. */
    const uint32_t rate = 2000U;
    const size_t block_samples = streamer_block_samples_for_rate(1536, rate);
    uint64_t prev_start = 0;
    for (uint64_t k = 1; k < 600000ULL; k += 997ULL) { /* spans a full day of 128 ms blocks */
        const uint64_t start_ns = streamer_block_start_ns(k, block_samples, rate);
        ck_assert_uint_gt(start_ns, prev_start);
        prev_start = start_ns;
        ck_assert_uint_eq(streamer_block_index_from_time_ns(start_ns, block_samples, rate), k);
    }
}
END_TEST

START_TEST(batch_latency_cap_bounds_low_rate_bursts)
{
    const uint64_t budget_ns = 25000000ULL; /* 25 ms */
    /* High rate: 16 blocks of 1536 samples at 98.304 MS/s span ~250 us, far under 25 ms, so the
     * time cap is effectively unbounded and the fixed count cap alone binds. */
    ck_assert_uint_gt(streamer_batch_blocks_for_latency(1536, 98304000U, budget_ns), 16U);
    /* Low rate: at 100 kS/s a 1536-sample block is ~15.36 ms, so only one fits in 25 ms. */
    ck_assert_uint_eq(streamer_batch_blocks_for_latency(1536, 100000U, budget_ns), 1U);
    /* A rate where a couple of blocks fit: 400 kS/s -> 3.84 ms/block -> floor(25/3.84) = 6. */
    ck_assert_uint_eq(streamer_batch_blocks_for_latency(1536, 400000U, budget_ns), 6U);
    /* Never returns zero even when one block already exceeds the budget. */
    ck_assert_uint_eq(streamer_batch_blocks_for_latency(1536, 10000U, budget_ns), 1U);
    /* A zero budget (or degenerate block duration) means "no time limit". */
    ck_assert_uint_eq(streamer_batch_blocks_for_latency(1536, 100000U, 0U), SIZE_MAX);
    ck_assert_uint_eq(streamer_batch_blocks_for_latency(0, 100000U, budget_ns), SIZE_MAX);
    ck_assert_uint_eq(streamer_batch_blocks_for_latency(1536, 0U, budget_ns), SIZE_MAX);
}
END_TEST

Suite *streamer_suite(void)
{
    Suite *suite = suite_create("streamer");
    TCase *tc = tcase_create("pacing");
    tcase_add_test(tc, calculates_80mhz_packet_pacing_duration);
    tcase_add_test(tc, calculates_ddc_packet_pacing_duration);
    tcase_add_test(tc, rejects_empty_pacing_inputs);
    tcase_add_test(tc, block_grid_contains_time_and_is_monotonic_without_drift);
    tcase_add_test(tc, block_grid_start_matches_exact_sample_math);
    tcase_add_test(tc, block_grid_is_independent_of_query_phase);
    tcase_add_test(tc, low_rate_channels_shrink_block_size);
    tcase_add_test(tc, batch_latency_cap_bounds_low_rate_bursts);
    tcase_add_test(tc, block_grid_works_at_ddc_low_rates);
    suite_add_tcase(suite, tc);
    return suite;
}
