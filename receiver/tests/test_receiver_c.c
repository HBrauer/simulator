#include "test_suites.h"
#include "history.h"
#include "vita49_rx.h"
#include "waterfall.h"

#include <check.h>
#include <math.h>
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

START_TEST(parses_simulator_vita49_context_packet)
{
    /* Mirror of the simulator's vita49_write_context_packet layout: bandwidth (bit 29), RF
     * reference frequency (27), reference level (24, a single 32-bit word), sample rate (21),
     * and the two-word Data Packet Payload Format field (15) -- 15 words total. */
    uint8_t packet[60];
    memset(packet, 0, sizeof(packet));
    const uint32_t header = (4U << 28U) | (1U << 22U) | (2U << 20U) | (5U << 16U) | 15U;
    write_be32(packet, header);
    write_be32(packet + 4, 0x53440001U);
    write_be32(packet + 8, 1U);
    write_be64(packet + 12, 234567890000ULL);
    const uint32_t cif0 =
        (1U << 31U) | (1U << 29U) | (1U << 27U) | (1U << 24U) | (1U << 21U) | (1U << 15U);
    write_be32(packet + 20, cif0);
    write_be64(packet + 24, 20000000ULL << 20U);
    write_be64(packet + 32, 10005000000ULL << 20U);
    write_be32(packet + 40, (uint32_t)((uint16_t)(int16_t)(-55 * 128))); /* -55 dBm, dBm*2^7 */
    write_be64(packet + 44, 24576000ULL << 20U);
    write_be32(packet + 52, 0x2E0007DFU); /* CF32: IEEE-754 single, item packing 32, data item 32 */
    write_be32(packet + 56, 0U);

    ck_assert_uint_eq(vita49_rx_packet_type(packet, sizeof(packet)), VITA49_RX_PACKET_TYPE_CONTEXT);
    vita49_rx_context_t context;
    ck_assert(vita49_rx_parse_context(packet, sizeof(packet), &context));
    ck_assert_uint_eq(context.sequence, 5);
    ck_assert(context.changed);
    ck_assert_uint_eq(context.stream_id, 0x53440001U);
    ck_assert_uint_eq(context.timestamp_ns, 1234567890ULL);
    ck_assert_uint_eq(context.bandwidth_hz, 20000000ULL);
    ck_assert_uint_eq(context.rf_reference_frequency_hz, 10005000000ULL);
    ck_assert_uint_eq(context.sample_rate_hz, 24576000ULL);
    ck_assert(context.has_reference_level);
    ck_assert_double_eq_tol(context.reference_level_dbm, -55.0, 0.01);
    ck_assert(context.has_format);
    ck_assert_int_eq(context.format, VITA49_RX_FORMAT_CF32);

    /* The CI24 and CI16 payload-format words decode to their formats too. */
    write_be32(packet + 52, 0x200007D7U);
    ck_assert(vita49_rx_parse_context(packet, sizeof(packet), &context));
    ck_assert_int_eq(context.format, VITA49_RX_FORMAT_CI24);
    write_be32(packet + 52, 0x200003CFU);
    ck_assert(vita49_rx_parse_context(packet, sizeof(packet), &context));
    ck_assert_int_eq(context.format, VITA49_RX_FORMAT_CI16);

    /* Genuinely unknown CIF0 bits shift the field layout, so parsing must refuse. */
    write_be32(packet + 20, cif0 | (1U << 30U));
    ck_assert(!vita49_rx_parse_context(packet, sizeof(packet), &context));
}
END_TEST

/* A context packet without the Reference Level field (older simulators) must still parse; the
 * receiver simply has no absolute-power reference and stays on a dBFS axis. */
START_TEST(parses_context_packet_without_reference_level)
{
    uint8_t packet[48];
    memset(packet, 0, sizeof(packet));
    const uint32_t header = (4U << 28U) | (1U << 22U) | (2U << 20U) | (5U << 16U) | 12U;
    write_be32(packet, header);
    write_be32(packet + 4, 0x53440001U);
    write_be32(packet + 8, 1U);
    write_be64(packet + 12, 234567890000ULL);
    const uint32_t cif0 = (1U << 31U) | (1U << 29U) | (1U << 27U) | (1U << 21U);
    write_be32(packet + 20, cif0);
    write_be64(packet + 24, 20000000ULL << 20U);
    write_be64(packet + 32, 10005000000ULL << 20U);
    write_be64(packet + 40, 24576000ULL << 20U);

    vita49_rx_context_t context;
    ck_assert(vita49_rx_parse_context(packet, sizeof(packet), &context));
    ck_assert_uint_eq(context.sample_rate_hz, 24576000ULL);
    ck_assert(!context.has_reference_level);
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
    for (size_t i = 0; i < 4U * 16U; i++) {
        any = any || wf.history[i] > -120.0f;
    }
    ck_assert(any);
    waterfall_free(&wf);
}
END_TEST

/* The spectrum is calibrated so a full-scale on-bin complex tone reads 0 dBFS, and level scales
 * 1:1 in dB. This is what lets the axis read absolute dBm once the VITA reference level (RF power
 * at 0 dBFS) is added. */
START_TEST(full_scale_tone_reads_zero_dbfs)
{
    const size_t n = 1024;
    const size_t bin = 64; /* on-bin: frequency = bin * fs / n */
    waterfall_t wf;
    ck_assert(waterfall_init(&wf, n, 4));

    int16_t iq[2U * 1024];
    for (size_t i = 0; i < n; i++) {
        const double phase = 2.0 * M_PI * (double)bin * (double)i / (double)n;
        iq[2U * i] = (int16_t)lround(32767.0 * cos(phase));
        iq[2U * i + 1U] = (int16_t)lround(32767.0 * sin(phase));
    }
    ck_assert(waterfall_push_ci16(&wf, iq, n));
    /* Full scale -> 0 dBFS at the tone bin (small negative from the 32767/32768 scale). */
    ck_assert_double_eq_tol(wf.last_max_db, 0.0, 0.2);

    /* Half amplitude (-6 dB) must move the peak down by ~6 dB: 1:1 dB scaling. */
    for (size_t i = 0; i < n; i++) {
        const double phase = 2.0 * M_PI * (double)bin * (double)i / (double)n;
        iq[2U * i] = (int16_t)lround(16384.0 * cos(phase));
        iq[2U * i + 1U] = (int16_t)lround(16384.0 * sin(phase));
    }
    ck_assert(waterfall_push_ci16(&wf, iq, n));
    ck_assert_double_eq_tol(wf.last_max_db, -6.0, 0.2);

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
    tcase_add_test(tc, parses_simulator_vita49_context_packet);
    tcase_add_test(tc, parses_context_packet_without_reference_level);
    tcase_add_test(tc, pushes_ci16_into_waterfall);
    tcase_add_test(tc, full_scale_tone_reads_zero_dbfs);
    tcase_add_test(tc, calculates_history_stride_from_duration);
    suite_add_tcase(suite, tc);
    return suite;
}
