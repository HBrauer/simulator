#include "test_suites.h"
#include "history.h"
#include "vita49_rx.h"
#include "waterfall.h"

#include <check.h>
#include <stdint.h>
#include <string.h>

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) {
        out[i] = (uint8_t)(value >> (56U - 8U * i));
    }
}

START_TEST(parses_simulator_vita49_if_data_packet)
{
    uint8_t packet[28];
    const uint32_t header = (1U << 28U) | (1U << 25U) | (1U << 22U) | (2U << 20U) | (7U << 16U) | 7U;
    write_be32(packet, header);
    write_be32(packet + 4, 0x53440000U);
    write_be32(packet + 8, 1U);
    write_be64(packet + 12, 234567890000ULL);
    packet[20] = 1;
    packet[21] = 0;
    packet[22] = 2;
    packet[23] = 0;
    packet[24] = 3;
    packet[25] = 0;
    packet[26] = 4;
    packet[27] = 0;

    vita49_rx_packet_t parsed;
    ck_assert(vita49_rx_parse_if_data(packet, sizeof(packet), &parsed));
    ck_assert_uint_eq(parsed.sequence, 7);
    ck_assert(parsed.vita49_2);
    ck_assert_uint_eq(parsed.stream_id, 0x53440000U);
    ck_assert_uint_eq(parsed.timestamp_ns, 1234567890ULL);
    ck_assert_uint_eq(parsed.payload_bytes, 8);
    ck_assert_ptr_eq(parsed.payload, packet + 20);
}
END_TEST

START_TEST(rejects_malformed_vita49_packet_size)
{
    uint8_t packet[24];
    memset(packet, 0, sizeof(packet));
    write_be32(packet, (1U << 28U) | (1U << 25U) | (1U << 22U) | (2U << 20U) | 99U);
    vita49_rx_packet_t parsed;
    ck_assert(!vita49_rx_parse_if_data(packet, sizeof(packet), &parsed));
}
END_TEST

START_TEST(pushes_ci16_into_waterfall)
{
    waterfall_t wf;
    ck_assert(waterfall_init(&wf, 16, 4));
    int16_t iq[32];
    for (size_t i = 0; i < 16; i++) {
        iq[2U * i] = (int16_t)(1000 * (int)i);
        iq[2U * i + 1U] = 0;
    }
    ck_assert(waterfall_push_ci16(&wf, iq, 16));
    ck_assert(wf.last_max_db > wf.last_min_db);
    bool any = false;
    for (size_t i = 0; i < 16; i++) {
        if (wf.history[3U * 16U + i] > -120.0f) {
            any = true;
        }
    }
    ck_assert(any);
    waterfall_free(&wf);
}
END_TEST

START_TEST(calculates_history_stride_from_duration)
{
    ck_assert(waterfall_history_seconds_valid(1.0));
    ck_assert(waterfall_history_seconds_valid(WATERFALL_HISTORY_DEFAULT_SECONDS));
    ck_assert(waterfall_history_seconds_valid(60.0));
    ck_assert(!waterfall_history_seconds_valid(0.5));
    ck_assert(!waterfall_history_seconds_valid(61.0));

    const size_t stride_1s = waterfall_stride_for_history(40, 24576000U, 1.0, 1024);
    const size_t stride_30s = waterfall_stride_for_history(40, 24576000U, WATERFALL_HISTORY_DEFAULT_SECONDS, 1024);
    const size_t stride_60s = waterfall_stride_for_history(40, 24576000U, 60.0, 1024);
    ck_assert_uint_eq(stride_1s, 614400U);
    ck_assert_uint_eq(stride_30s, 18432000U);
    ck_assert_uint_eq(stride_60s, 36864000U);
    ck_assert_double_eq_tol(waterfall_history_for_stride(40, 24576000U, stride_1s), 1.0, 0.0001);
    ck_assert_double_eq_tol(waterfall_history_for_stride(40, 24576000U, stride_30s), WATERFALL_HISTORY_DEFAULT_SECONDS, 0.0001);
    ck_assert_double_eq_tol(waterfall_history_for_stride(40, 24576000U, stride_60s), 60.0, 0.0001);
}
END_TEST

Suite *receiver_c_suite(void)
{
    Suite *suite = suite_create("receiver_c");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, parses_simulator_vita49_if_data_packet);
    tcase_add_test(tc, rejects_malformed_vita49_packet_size);
    tcase_add_test(tc, pushes_ci16_into_waterfall);
    tcase_add_test(tc, calculates_history_stride_from_duration);
    suite_add_tcase(suite, tc);
    return suite;
}
