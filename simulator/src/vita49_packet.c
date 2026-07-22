#include "vita49_packet.h"

#include <limits.h>
#include <string.h>

#define VITA49_PSI_VITA49_2 (1U << 25U)
/* CIF0 / header "Class ID present" indicator (VITA 49.2 header bit 27). */
#define VITA49_HDR_CLASS_ID_PRESENT (1U << 27U)

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)((value >> 24U) & 0xffU);
    out[1] = (uint8_t)((value >> 16U) & 0xffU);
    out[2] = (uint8_t)((value >> 8U) & 0xffU);
    out[3] = (uint8_t)(value & 0xffU);
}

static void write_be64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) {
        out[i] = (uint8_t)((value >> (56U - (8U * i))) & 0xffU);
    }
}

/* Two-word Class ID: 24-bit OUI in bits 23..0 of the first word (bits 31..24 reserved, per
 * VITA 49.2 §5.1.3 / DIFI), then the information/packet class codes packed into the halves
 * of the second word. */
static void write_class_id(uint8_t *out, const vita49_class_id_t *class_id)
{
    write_be32(out, class_id->oui & 0xffffffU);
    write_be32(out + 4, ((uint32_t)class_id->information_class_code << 16U) | (uint32_t)class_id->packet_class_code);
}

size_t vita49_bytes_per_sample(sim_output_format_t format)
{
    switch (format) {
        case SIM_OUTPUT_FORMAT_CI24:
        case SIM_OUTPUT_FORMAT_CF32:
            return 8U; /* two 32-bit item packing fields (I, Q) */
        case SIM_OUTPUT_FORMAT_CI16:
        default:
            return 4U; /* one 32-bit word packs both 16-bit I and Q */
    }
}

/* Internal storage and on-wire payload use the same bytes per complex sample. */
size_t sim_internal_bytes_per_sample(sim_output_format_t format)
{
    return vita49_bytes_per_sample(format);
}

bool sim_output_format_from_name(const char *name, sim_output_format_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "ci16") == 0) {
        *out = SIM_OUTPUT_FORMAT_CI16;
    } else if (strcmp(name, "ci24") == 0) {
        *out = SIM_OUTPUT_FORMAT_CI24;
    } else if (strcmp(name, "cf32") == 0) {
        *out = SIM_OUTPUT_FORMAT_CF32;
    } else {
        return false;
    }
    return true;
}

size_t vita49_if_data_packet_size(size_t payload_samples, bool class_id_present, sim_output_format_t format)
{
    const size_t class_id_bytes = class_id_present ? VITA49_CLASS_ID_BYTES : 0U;
    return VITA49_IF_DATA_HEADER_BYTES + class_id_bytes + payload_samples * vita49_bytes_per_sample(format);
}

/* Write one IEEE-754 single as a big-endian 32-bit word. */
static void write_be_f32(uint8_t *out, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_be32(out, bits);
}

/* Serialise the channel's native payload buffer into `out` in its on-wire format, big-endian,
 * processing-efficient, Complex Cartesian (VITA 49.2 6.1.1); values are unchanged. Returns the
 * number of bytes written. */
static size_t write_if_data_payload(const void *payload, size_t samples, sim_output_format_t format, uint8_t *out)
{
    size_t offset = 0;
    switch (format) {
        case SIM_OUTPUT_FORMAT_CF32: {
            const iq_cf32_t *p = (const iq_cf32_t *)payload;
            for (size_t s = 0; s < samples; s++) {
                write_be_f32(out + offset, p[s].i);
                write_be_f32(out + offset + 4U, p[s].q);
                offset += 8U;
            }
            break;
        }
        case SIM_OUTPUT_FORMAT_CI24: {
            /* 24-bit signed data item left-justified in a 32-bit item packing field (VITA 49.2
             * rule 6.1.1.1-2): the value occupies bits 31..8, low 8 bits unused (zero). The
             * internal value is right-justified in [-2^23, 2^23-1], so left-justify with <<8. */
            const iq_ci24_t *p = (const iq_ci24_t *)payload;
            for (size_t s = 0; s < samples; s++) {
                write_be32(out + offset, ((uint32_t)p[s].i) << 8U);
                write_be32(out + offset + 4U, ((uint32_t)p[s].q) << 8U);
                offset += 8U;
            }
            break;
        }
        case SIM_OUTPUT_FORMAT_CI16:
        default: {
            /* Two 16-bit signed items in one 32-bit word: I in bits 31..16, Q in bits 15..0. */
            const iq_ci16_t *p = (const iq_ci16_t *)payload;
            for (size_t s = 0; s < samples; s++) {
                const uint32_t word = ((uint32_t)(uint16_t)p[s].i << 16) | (uint32_t)(uint16_t)p[s].q;
                write_be32(out + offset, word);
                offset += 4U;
            }
            break;
        }
    }
    return offset;
}

/* Two-word Data Packet Payload Format field (VITA 49.2 9.13.3): processing-efficient (packing
 * method 0), Complex Cartesian. */
static void write_payload_format(uint8_t *out, sim_output_format_t format)
{
    uint32_t data_item_format;  /* 5-bit code, Table 9.13.3-4 */
    uint32_t item_packing_bits; /* actual item packing field size */
    uint32_t data_item_bits;    /* actual data item size */
    switch (format) {
        case SIM_OUTPUT_FORMAT_CF32:
            data_item_format = 0x0EU; /* IEEE-754 single-precision */
            item_packing_bits = 32U;
            data_item_bits = 32U;
            break;
        case SIM_OUTPUT_FORMAT_CI24:
            data_item_format = 0x00U; /* signed fixed-point */
            item_packing_bits = 32U;
            data_item_bits = 24U;
            break;
        case SIM_OUTPUT_FORMAT_CI16:
        default:
            data_item_format = 0x00U; /* signed fixed-point */
            item_packing_bits = 16U;
            data_item_bits = 16U;
            break;
    }
    /* Real/Complex = 01 (Complex, Cartesian). Item Packing Field Size and Data Item Size fields
     * carry one less than the actual size (rules 9.13.3-12/13). */
    const uint32_t word1 =
        (1U << 29U) |
        (data_item_format << 24U) |
        (((item_packing_bits - 1U) & 0x3fU) << 6U) |
        ((data_item_bits - 1U) & 0x3fU);
    write_be32(out, word1);
    write_be32(out + 4U, 0U); /* Repeat Count / Vector Size: 0 => actual 1 */
}

/* CIF0 indicator bits (VITA 49.2 section 9.1). */
#define VITA49_CIF0_CHANGE_INDICATOR (1U << 31U)
#define VITA49_CIF0_BANDWIDTH (1U << 29U)
#define VITA49_CIF0_RF_REFERENCE_FREQUENCY (1U << 27U)
#define VITA49_CIF0_REFERENCE_LEVEL (1U << 24U)
#define VITA49_CIF0_SAMPLE_RATE (1U << 21U)
#define VITA49_CIF0_DATA_PAYLOAD_FORMAT (1U << 15U)

/* Header, stream id, integer seconds, fractional ps (2), CIF0, three 64-bit fields, a single
 * 32-bit reference-level word, and the two-word Data Packet Payload Format field. */
#define VITA49_CONTEXT_BASE_WORDS (6U + 3U * 2U + 1U + 2U)
#define VITA49_CLASS_ID_WORDS 2U

/* Frequency/rate context fields are 64-bit two's complement Hz with the radix point after
 * bit 20 (VITA 49.2 rule 9.5.1-1 family): value_hz * 2^20. */
static uint64_t vita49_fixed_hz(uint64_t hz)
{
    return hz << 20U;
}

/* Reference Level (VITA 49.2 section 9.5.9): a 32-bit field whose low 16 bits are a two's
 * complement value in dBm with the radix point after bit 7 (dBm * 2^7); the high 16 bits are
 * reserved (zero). */
static uint32_t vita49_reference_level(double dbm)
{
    double scaled = dbm * 128.0;
    if (scaled > 32767.0) {
        scaled = 32767.0;
    } else if (scaled < -32768.0) {
        scaled = -32768.0;
    }
    const int32_t rounded = (int32_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
    return (uint32_t)((int16_t)rounded) & 0xffffU;
}

size_t vita49_context_packet_size(bool class_id_present)
{
    const size_t words = (size_t)VITA49_CONTEXT_BASE_WORDS + (class_id_present ? VITA49_CLASS_ID_WORDS : 0U);
    return words * 4U;
}

bool vita49_write_context_packet(const vita49_context_packet_t *packet, uint8_t *out, size_t out_size, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (packet == NULL || out == NULL || out_size < vita49_context_packet_size(packet->class_id_present)) {
        return false;
    }
    const size_t total_bytes = vita49_context_packet_size(packet->class_id_present);
    const uint64_t seconds = packet->timestamp_ns / 1000000000ULL;
    const uint64_t fractional_ps = (packet->timestamp_ns % 1000000000ULL) * 1000ULL;
    uint32_t header =
        (VITA49_PACKET_TYPE_CONTEXT << 28U) |
        (VITA49_TSI_UTC << 22U) |
        (VITA49_TSF_REAL_TIME << 20U) |
        (((uint32_t)packet->sequence & 0x0fU) << 16U) |
        (uint32_t)(total_bytes / 4U);
    if (packet->class_id_present) {
        header |= VITA49_HDR_CLASS_ID_PRESENT;
    }
    uint32_t cif0 = VITA49_CIF0_BANDWIDTH | VITA49_CIF0_RF_REFERENCE_FREQUENCY |
                    VITA49_CIF0_REFERENCE_LEVEL | VITA49_CIF0_SAMPLE_RATE |
                    VITA49_CIF0_DATA_PAYLOAD_FORMAT;
    if (packet->changed) {
        cif0 |= VITA49_CIF0_CHANGE_INDICATOR;
    }
    size_t offset = 0;
    write_be32(out + offset, header);
    offset += 4;
    write_be32(out + offset, packet->stream_id);
    offset += 4;
    if (packet->class_id_present) {
        write_class_id(out + offset, &packet->class_id);
        offset += VITA49_CLASS_ID_BYTES;
    }
    write_be32(out + offset, (uint32_t)seconds);
    offset += 4;
    write_be64(out + offset, fractional_ps);
    offset += 8;
    write_be32(out + offset, cif0);
    offset += 4;
    /* Fields follow in descending CIF0 bit order: bandwidth (29), RF reference frequency (27),
     * reference level (24), sample rate (21). */
    write_be64(out + offset, vita49_fixed_hz(packet->bandwidth_hz));
    offset += 8;
    write_be64(out + offset, vita49_fixed_hz(packet->rf_reference_frequency_hz));
    offset += 8;
    write_be32(out + offset, vita49_reference_level(packet->reference_level_dbm));
    offset += 4;
    write_be64(out + offset, vita49_fixed_hz(packet->sample_rate_hz));
    offset += 8;
    /* Data Packet Payload Format (CIF0 bit 15) sorts below sample rate (bit 21). */
    write_payload_format(out + offset, packet->format);
    offset += 8;
    if (written != NULL) {
        *written = total_bytes;
    }
    return true;
}

bool vita49_write_if_data_packet(const vita49_if_data_packet_t *packet, uint8_t *out, size_t out_size, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (packet == NULL) {
        return false;
    }
    const size_t header_bytes = vita49_if_data_packet_size(0, packet->class_id_present, packet->format);
    const size_t payload_bytes = packet->payload_samples * vita49_bytes_per_sample(packet->format);
    const size_t total_bytes = header_bytes + payload_bytes;
    if (out == NULL || packet->payload == NULL || out_size < total_bytes || total_bytes % 4U != 0U ||
        total_bytes / 4U > UINT16_MAX) {
        return false;
    }
    const uint16_t packet_words = (uint16_t)(total_bytes / 4U);
    const uint64_t seconds = packet->timestamp_ns / 1000000000ULL;
    const uint64_t fractional_ps = (packet->timestamp_ns % 1000000000ULL) * 1000ULL;
    uint32_t header =
        (VITA49_PACKET_TYPE_IF_DATA << 28U) |
        VITA49_PSI_VITA49_2 |
        (VITA49_TSI_UTC << 22U) |
        (VITA49_TSF_REAL_TIME << 20U) |
        (((uint32_t)packet->sequence & 0x0fU) << 16U) |
        packet_words;
    if (packet->class_id_present) {
        header |= VITA49_HDR_CLASS_ID_PRESENT;
    }

    size_t offset = 0;
    write_be32(out + offset, header);
    offset += 4;
    write_be32(out + offset, packet->stream_id);
    offset += 4;
    if (packet->class_id_present) {
        write_class_id(out + offset, &packet->class_id);
        offset += VITA49_CLASS_ID_BYTES;
    }
    write_be32(out + offset, (uint32_t)seconds);
    offset += 4;
    write_be64(out + offset, fractional_ps);
    offset += 8;
    write_if_data_payload(packet->payload, packet->payload_samples, packet->format, out + offset);
    if (written != NULL) {
        *written = total_bytes;
    }
    return true;
}
