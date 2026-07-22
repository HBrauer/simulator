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
        .stream_id = 0x53440303U,
        .sequence = 9,
        .timestamp_ns = 1234567890ULL,
        .payload = samples,
        .payload_samples = 2,
        .format = SIM_OUTPUT_FORMAT_CI16,
    };

    ck_assert(vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, VITA49_IF_DATA_HEADER_BYTES + 2U * 4U);
    ck_assert_uint_eq(read_be32(out) >> 28U, VITA49_PACKET_TYPE_IF_DATA);
    ck_assert_uint_eq((read_be32(out) >> 27U) & 0x01U, 0U); /* class id absent */
    ck_assert_uint_eq((read_be32(out) >> 22U) & 0x03U, VITA49_TSI_UTC);
    ck_assert_uint_eq((read_be32(out) >> 20U) & 0x03U, VITA49_TSF_REAL_TIME);
    ck_assert_uint_eq((read_be32(out) >> 25U) & 0x01U, 1U);
    ck_assert_uint_eq((read_be32(out) >> 16U) & 0x0fU, 9U);
    ck_assert_uint_eq(read_be32(out) & 0xffffU, written / 4U);
    ck_assert_uint_eq(read_be32(out + 4), 0x53440303U);
    ck_assert_uint_eq(read_be32(out + 8), 1U);
    ck_assert_uint_eq(read_be64(out + 12), 234567890000ULL);
    /* CI16: one big-endian 32-bit word per sample, I in bits 31..16, Q in bits 15..0. */
    ck_assert_uint_eq(read_be32(out + 20), ((uint32_t)(uint16_t)1 << 16) | (uint16_t)-2);
    ck_assert_uint_eq(read_be32(out + 24), ((uint32_t)(uint16_t)300 << 16) | (uint16_t)-400);
}
END_TEST

START_TEST(writes_if_data_packet_with_class_id)
{
    const iq_ci16_t samples[] = {
        {.i = 7, .q = -8},
    };
    uint8_t out[64];
    size_t written = 0;
    const vita49_if_data_packet_t packet = {
        .stream_id = 0x00000005U,
        .sequence = 4,
        .timestamp_ns = 1234567890ULL,
        .class_id_present = true,
        .class_id = {.oui = 0xABCDEFU, .information_class_code = 0x1234U, .packet_class_code = 0x5678U},
        .payload = samples,
        .payload_samples = 1,
        .format = SIM_OUTPUT_FORMAT_CI16,
    };

    ck_assert(vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, vita49_if_data_packet_size(1, true, SIM_OUTPUT_FORMAT_CI16));
    ck_assert_uint_eq(written, VITA49_IF_DATA_HEADER_BYTES + VITA49_CLASS_ID_BYTES + 4U);
    ck_assert_uint_eq((read_be32(out) >> 27U) & 0x01U, 1U); /* class id present */
    ck_assert_uint_eq(read_be32(out) & 0xffffU, written / 4U);
    ck_assert_uint_eq(read_be32(out + 4), 0x00000005U);
    /* Class ID: 24-bit OUI in bits 23..0, then info/packet class codes. */
    ck_assert_uint_eq(read_be32(out + 8), 0xABCDEFU);
    ck_assert_uint_eq(read_be32(out + 12), (0x1234U << 16U) | 0x5678U);
    /* Timestamps shift past the two class-id words. */
    ck_assert_uint_eq(read_be32(out + 16), 1U);
    ck_assert_uint_eq(read_be64(out + 20), 234567890000ULL);
    ck_assert_uint_eq(read_be32(out + 28), ((uint32_t)(uint16_t)7 << 16) | (uint16_t)-8);
}
END_TEST

START_TEST(writes_ci24_payload_left_justified)
{
    /* Native CI24 values are right-justified 24-bit ints; the writer left-justifies them (<<8). */
    const iq_ci24_t samples[] = {
        {.i = 0x123400, .q = -256},
        {.i = -8388608, .q = 8388607},
    };
    uint8_t out[64];
    size_t written = 0;
    const vita49_if_data_packet_t packet = {
        .stream_id = 1,
        .payload = samples,
        .payload_samples = 2,
        .format = SIM_OUTPUT_FORMAT_CI24,
    };

    ck_assert(vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    /* CI24: 8 bytes/sample, two 32-bit words (I, Q). The 24-bit item is left-justified in the
     * 32-bit field: value<<8 occupies bits 31..8, low 8 bits zero. */
    ck_assert_uint_eq(written, VITA49_IF_DATA_HEADER_BYTES + 2U * 8U);
    ck_assert_uint_eq(read_be32(out + 20), 0x12340000U); /* 0x123400 << 8 */
    ck_assert_uint_eq(read_be32(out + 24), 0xFFFF0000U); /* -256 << 8 */
    ck_assert_uint_eq(read_be32(out + 28), 0x80000000U); /* -2^23 << 8 */
    ck_assert_uint_eq(read_be32(out + 32), 0x7FFFFF00U); /* (2^23-1) << 8 */
    /* Low 8 bits of every item are unused (zero). */
    ck_assert_uint_eq(read_be32(out + 20) & 0xffU, 0U);
    ck_assert_uint_eq(read_be32(out + 24) & 0xffU, 0U);
}
END_TEST

START_TEST(writes_cf32_payload_full_scale)
{
    const iq_cf32_t samples[] = {
        {.i = 0.5f, .q = -1.0f},
    };
    uint8_t out[64];
    size_t written = 0;
    const vita49_if_data_packet_t packet = {
        .stream_id = 1,
        .payload = samples,
        .payload_samples = 1,
        .format = SIM_OUTPUT_FORMAT_CF32,
    };

    ck_assert(vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, VITA49_IF_DATA_HEADER_BYTES + 8U);
    /* IEEE-754 single big-endian: 0.5 -> 0x3F000000; -1.0 -> 0xBF800000. */
    ck_assert_uint_eq(read_be32(out + 20), 0x3F000000U);
    ck_assert_uint_eq(read_be32(out + 24), 0xBF800000U);
}
END_TEST

START_TEST(bytes_per_sample_matches_format)
{
    ck_assert_uint_eq(vita49_bytes_per_sample(SIM_OUTPUT_FORMAT_CI16), 4U);
    ck_assert_uint_eq(vita49_bytes_per_sample(SIM_OUTPUT_FORMAT_CI24), 8U);
    ck_assert_uint_eq(vita49_bytes_per_sample(SIM_OUTPUT_FORMAT_CF32), 8U);
}
END_TEST

START_TEST(rejects_too_small_output_buffer)
{
    const iq_ci16_t sample = {.i = 1, .q = 2};
    uint8_t out[8];
    size_t written = 99;
    const vita49_if_data_packet_t packet = {
        .stream_id = 0,
        .sequence = 0,
        .timestamp_ns = 0,
        .payload = &sample,
        .payload_samples = 1,
        .format = SIM_OUTPUT_FORMAT_CI16,
    };

    ck_assert(!vita49_write_if_data_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, 0);
}
END_TEST

/* Expected first Data Packet Payload Format word for each format (Complex Cartesian,
 * processing-efficient), verified against ANSI/VITA-49.2 Appendix B.9. */
static uint32_t expected_payload_format_word(sim_output_format_t format)
{
    switch (format) {
        case SIM_OUTPUT_FORMAT_CI24:
            return 0x200007D7U; /* signed fixed, item packing 32, data item 24 */
        case SIM_OUTPUT_FORMAT_CF32:
            return 0x2E0007DFU; /* IEEE-754 single, item packing 32, data item 32 */
        case SIM_OUTPUT_FORMAT_CI16:
        default:
            return 0x200003CFU; /* signed fixed, item packing 16, data item 16 */
    }
}

START_TEST(writes_context_packet_fields)
{
    const sim_output_format_t formats[] = {SIM_OUTPUT_FORMAT_CI16, SIM_OUTPUT_FORMAT_CI24, SIM_OUTPUT_FORMAT_CF32};
    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        uint8_t out[80];
        size_t written = 0;
        const vita49_context_packet_t packet = {
            .stream_id = 0x53440102U,
            .sequence = 5,
            .timestamp_ns = 1234567890ULL,
            .changed = true,
            .rf_reference_frequency_hz = 10005000000ULL,
            .bandwidth_hz = 20000000ULL,
            .sample_rate_hz = 24576000ULL,
            .reference_level_dbm = -55.0,
            .format = formats[f],
        };

        ck_assert(vita49_write_context_packet(&packet, out, sizeof(out), &written));
        ck_assert_uint_eq(written, vita49_context_packet_size(false));
        const uint32_t header = read_be32(out);
        ck_assert_uint_eq(header >> 28U, VITA49_PACKET_TYPE_CONTEXT);
        ck_assert_uint_eq((header >> 27U) & 0x01U, 0U); /* class id absent */
        ck_assert_uint_eq((header >> 22U) & 0x03U, VITA49_TSI_UTC);
        ck_assert_uint_eq((header >> 20U) & 0x03U, VITA49_TSF_REAL_TIME);
        ck_assert_uint_eq((header >> 16U) & 0x0fU, 5U);
        ck_assert_uint_eq(header & 0xffffU, written / 4U);
        ck_assert_uint_eq(read_be32(out + 4), 0x53440102U);
        ck_assert_uint_eq(read_be32(out + 8), 1U);
        ck_assert_uint_eq(read_be64(out + 12), 234567890000ULL);
        const uint32_t cif0 = read_be32(out + 20);
        ck_assert_uint_eq(cif0 >> 31U, 1U);        /* change indicator */
        ck_assert_uint_eq((cif0 >> 29U) & 1U, 1U); /* bandwidth */
        ck_assert_uint_eq((cif0 >> 27U) & 1U, 1U); /* rf reference frequency */
        ck_assert_uint_eq((cif0 >> 24U) & 1U, 1U); /* reference level */
        ck_assert_uint_eq((cif0 >> 21U) & 1U, 1U); /* sample rate */
        ck_assert_uint_eq((cif0 >> 15U) & 1U, 1U); /* data packet payload format */
        /* 64-bit fixed point, radix point after bit 20: value_hz << 20. */
        ck_assert_uint_eq(read_be64(out + 24) >> 20U, 20000000ULL);
        ck_assert_uint_eq(read_be64(out + 32) >> 20U, 10005000000ULL);
        /* Reference level: 32-bit word, low 16 bits are dBm * 2^7 two's complement. */
        ck_assert_int_eq((int16_t)(read_be32(out + 40) & 0xffffU), (int16_t)(-55 * 128));
        ck_assert_uint_eq(read_be64(out + 44) >> 20U, 24576000ULL);
        /* Data Packet Payload Format field (two words, sorts below sample rate). */
        ck_assert_uint_eq(read_be32(out + 52), expected_payload_format_word(formats[f]));
        ck_assert_uint_eq(read_be32(out + 56), 0U);
    }
}
END_TEST

START_TEST(writes_context_packet_with_class_id)
{
    uint8_t out[80];
    size_t written = 0;
    const vita49_context_packet_t packet = {
        .stream_id = 0x00000002U,
        .sequence = 3,
        .timestamp_ns = 1234567890ULL,
        .changed = false,
        .class_id_present = true,
        .class_id = {.oui = 0x00A2F5U, .information_class_code = 0x0001U, .packet_class_code = 0x0002U},
        .rf_reference_frequency_hz = 10005000000ULL,
        .bandwidth_hz = 20000000ULL,
        .sample_rate_hz = 24576000ULL,
        .reference_level_dbm = -55.0,
        .format = SIM_OUTPUT_FORMAT_CF32,
    };

    ck_assert(vita49_write_context_packet(&packet, out, sizeof(out), &written));
    ck_assert_uint_eq(written, vita49_context_packet_size(true));
    const uint32_t header = read_be32(out);
    ck_assert_uint_eq(header >> 28U, VITA49_PACKET_TYPE_CONTEXT);
    ck_assert_uint_eq((header >> 27U) & 0x01U, 1U); /* class id present */
    ck_assert_uint_eq(header & 0xffffU, written / 4U);
    ck_assert_uint_eq(read_be32(out + 4), 0x00000002U);
    ck_assert_uint_eq(read_be32(out + 8), 0x00A2F5U);
    ck_assert_uint_eq(read_be32(out + 12), (0x0001U << 16U) | 0x0002U);
    /* CIF0 and its fields shift past the two class-id words. */
    ck_assert_uint_eq(read_be32(out + 16), 1U); /* integer seconds */
    ck_assert_uint_eq(read_be64(out + 20), 234567890000ULL);
    const uint32_t cif0 = read_be32(out + 28);
    ck_assert_uint_eq(cif0 >> 31U, 0U);        /* not changed */
    ck_assert_uint_eq((cif0 >> 15U) & 1U, 1U); /* data packet payload format */
    ck_assert_uint_eq(read_be64(out + 32) >> 20U, 20000000ULL);
    ck_assert_uint_eq(read_be64(out + 40) >> 20U, 10005000000ULL);
    ck_assert_int_eq((int16_t)(read_be32(out + 48) & 0xffffU), (int16_t)(-55 * 128));
    ck_assert_uint_eq(read_be64(out + 52) >> 20U, 24576000ULL);
    ck_assert_uint_eq(read_be32(out + 60), expected_payload_format_word(SIM_OUTPUT_FORMAT_CF32));
    ck_assert_uint_eq(read_be32(out + 64), 0U);
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
    tcase_add_test(tc, writes_if_data_packet_with_class_id);
    tcase_add_test(tc, writes_ci24_payload_left_justified);
    tcase_add_test(tc, writes_cf32_payload_full_scale);
    tcase_add_test(tc, bytes_per_sample_matches_format);
    tcase_add_test(tc, rejects_too_small_output_buffer);
    tcase_add_test(tc, writes_context_packet_fields);
    tcase_add_test(tc, writes_context_packet_with_class_id);
    tcase_add_test(tc, context_packet_rejects_small_buffer);
    suite_add_tcase(suite, tc);
    return suite;
}
