#include "test_suites.h"
#include "vita49_packet.h"

#include <check.h>
#include <string.h>

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) | ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static uint64_t read_be64(const uint8_t *data)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        value = (value << 8U) | (uint64_t)data[i];
    }
    return value;
}

START_TEST(writes_if_data_packet_header_and_payload)
{
    const iq_ci16_t samples[] = {
        {.i = 1, .q = -2},
        {.i = 300, .q = -400},
    };
    uint8_t out[64];
    size_t written = 0;
    const vita49_if_data_packet_t packet = {
        .stream_id = vita49_stream_id(3, 3),
        .sequence = 9,
        .timestamp_ns = 1234567890ULL,
        .payload = samples,
        .payload_samples = 2,
    };

    ck_assert(vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, VITA49_IF_DATA_HEADER_BYTES + sizeof(samples));
    ck_assert_uint_eq(read_be32(out) >> 28U, VITA49_PACKET_TYPE_IF_DATA);
    ck_assert_uint_eq((read_be32(out) >> 22U) & 0x03U, VITA49_TSI_UTC);
    ck_assert_uint_eq((read_be32(out) >> 20U) & 0x03U, VITA49_TSF_REAL_TIME);
    ck_assert_uint_eq((read_be32(out) >> 25U) & 0x01U, 1U);
    ck_assert_uint_eq((read_be32(out) >> 16U) & 0x0fU, 9U);
    ck_assert_uint_eq(read_be32(out) & 0xffffU, written / 4U);
    ck_assert_uint_eq(read_be32(out + 4), 0x53440303U);
    ck_assert_uint_eq(read_be32(out + 8), 1U);
    ck_assert_uint_eq(read_be64(out + 12), 234567890000ULL);
    ck_assert(memcmp(out + VITA49_IF_DATA_HEADER_BYTES, samples, sizeof(samples)) == 0);
}
END_TEST

START_TEST(rejects_too_small_output_buffer)
{
    const iq_ci16_t sample = {.i = 1, .q = 2};
    uint8_t out[8];
    size_t written = 99;
    const vita49_if_data_packet_t packet = {
        .stream_id = vita49_stream_id(0, 0),
        .sequence = 0,
        .timestamp_ns = 0,
        .payload = &sample,
        .payload_samples = 1,
    };

    ck_assert(!vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, 0);
}
END_TEST

START_TEST(writes_context_packet_fields)
{
    uint8_t out[64];
    size_t written = 0;
    const vita49_context_packet_t packet = {
        .stream_id = vita49_stream_id(1, 2),
        .sequence = 5,
        .timestamp_ns = 1234567890ULL,
        .changed = true,
        .rf_reference_frequency_hz = 10005000000ULL,
        .bandwidth_hz = 20000000ULL,
        .sample_rate_hz = 24576000ULL,
    };

    ck_assert(vita49_write_context_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, vita49_context_packet_size());
    const uint32_t header = read_be32(out);
    ck_assert_uint_eq(header >> 28U, VITA49_PACKET_TYPE_CONTEXT);
    ck_assert_uint_eq((header >> 22U) & 0x03U, VITA49_TSI_UTC);
    ck_assert_uint_eq((header >> 20U) & 0x03U, VITA49_TSF_REAL_TIME);
    ck_assert_uint_eq((header >> 16U) & 0x0fU, 5U);
    ck_assert_uint_eq(header & 0xffffU, written / 4U);
    ck_assert_uint_eq(read_be32(out + 4), 0x53440102U);
    ck_assert_uint_eq(read_be32(out + 8), 1U);
    ck_assert_uint_eq(read_be64(out + 12), 234567890000ULL);
    const uint32_t cif0 = read_be32(out + 20);
    ck_assert_uint_eq(cif0 >> 31U, 1U);              /* change indicator */
    ck_assert_uint_eq((cif0 >> 29U) & 1U, 1U);       /* bandwidth */
    ck_assert_uint_eq((cif0 >> 27U) & 1U, 1U);       /* rf reference frequency */
    ck_assert_uint_eq((cif0 >> 21U) & 1U, 1U);       /* sample rate */
    /* 64-bit fixed point, radix point after bit 20: value_hz << 20. */
    ck_assert_uint_eq(read_be64(out + 24) >> 20U, 20000000ULL);
    ck_assert_uint_eq(read_be64(out + 32) >> 20U, 10005000000ULL);
    ck_assert_uint_eq(read_be64(out + 40) >> 20U, 24576000ULL);
}
END_TEST

START_TEST(context_packet_rejects_small_buffer)
{
    uint8_t out[16];
    size_t written = 99;
    const vita49_context_packet_t packet = {.stream_id = 1};
    ck_assert(!vita49_write_context_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, 0);
}
END_TEST

Suite *vita49_packet_suite(void)
{
    Suite *suite = suite_create("vita49_packet");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, writes_if_data_packet_header_and_payload);
    tcase_add_test(tc, rejects_too_small_output_buffer);
    tcase_add_test(tc, writes_context_packet_fields);
    tcase_add_test(tc, context_packet_rejects_small_buffer);
    suite_add_tcase(suite, tc);
    return suite;
}
