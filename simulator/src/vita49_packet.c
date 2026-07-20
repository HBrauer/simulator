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

/* Two-word Class ID: OUI left-justified into bits 31..8 of the first word, then the
 * information/packet class codes packed into the halves of the second word. */
static void write_class_id(uint8_t *out, const vita49_class_id_t *class_id)
{
    write_be32(out, (class_id->oui & 0xffffffU) << 8U);
    write_be32(out + 4, ((uint32_t)class_id->information_class_code << 16U) | (uint32_t)class_id->packet_class_code);
}

size_t vita49_if_data_packet_size(size_t payload_samples, bool class_id_present)
{
    const size_t class_id_bytes = class_id_present ? VITA49_CLASS_ID_BYTES : 0U;
    return VITA49_IF_DATA_HEADER_BYTES + class_id_bytes + payload_samples * sizeof(iq_ci16_t);
}

/* CIF0 indicator bits (VITA 49.2 section 9.1). */
#define VITA49_CIF0_CHANGE_INDICATOR (1U << 31U)
#define VITA49_CIF0_BANDWIDTH (1U << 29U)
#define VITA49_CIF0_RF_REFERENCE_FREQUENCY (1U << 27U)
#define VITA49_CIF0_SAMPLE_RATE (1U << 21U)

/* Header, stream id, integer seconds, fractional ps (2), CIF0, then three 64-bit fields. */
#define VITA49_CONTEXT_BASE_WORDS (6U + 3U * 2U)
#define VITA49_CLASS_ID_WORDS 2U

/* Frequency/rate context fields are 64-bit two's complement Hz with the radix point after
 * bit 20 (VITA 49.2 rule 9.5.1-1 family): value_hz * 2^20. */
static uint64_t vita49_fixed_hz(uint64_t hz)
{
    return hz << 20U;
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
    uint32_t cif0 = VITA49_CIF0_BANDWIDTH | VITA49_CIF0_RF_REFERENCE_FREQUENCY | VITA49_CIF0_SAMPLE_RATE;
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
    /* Fields follow in descending CIF0 bit order. */
    write_be64(out + offset, vita49_fixed_hz(packet->bandwidth_hz));
    offset += 8;
    write_be64(out + offset, vita49_fixed_hz(packet->rf_reference_frequency_hz));
    offset += 8;
    write_be64(out + offset, vita49_fixed_hz(packet->sample_rate_hz));
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
    const size_t header_bytes = vita49_if_data_packet_size(0, packet->class_id_present);
    const size_t payload_bytes = packet->payload_samples * sizeof(iq_ci16_t);
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
    memcpy(out + offset, packet->payload, payload_bytes);
    if (written != NULL) {
        *written = total_bytes;
    }
    return true;
}
