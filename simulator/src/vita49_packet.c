#include "vita49_packet.h"

#include <limits.h>
#include <string.h>

#define VITA49_PSI_VITA49_2 (1U << 25U)

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

size_t vita49_if_data_packet_size(size_t payload_samples)
{
    return VITA49_IF_DATA_HEADER_BYTES + payload_samples * sizeof(iq_ci16_t);
}

/* Channel ids map 1:1 onto the historical stream numbering: channel 0 is the former
 * wideband stream (index 0) and channels 1..N are the former DDC streams (1 + ddc_id). */
uint32_t vita49_stream_id(uint32_t receiver_id, uint32_t channel_id)
{
    return 0x53440000U | ((receiver_id & 0xffU) << 8U) | (channel_id & 0xffU);
}

/* CIF0 indicator bits (VITA 49.2 section 9.1). */
#define VITA49_CIF0_CHANGE_INDICATOR (1U << 31U)
#define VITA49_CIF0_BANDWIDTH (1U << 29U)
#define VITA49_CIF0_RF_REFERENCE_FREQUENCY (1U << 27U)
#define VITA49_CIF0_SAMPLE_RATE (1U << 21U)

/* Header, stream id, integer seconds, fractional ps (2), CIF0, then three 64-bit fields. */
#define VITA49_CONTEXT_WORDS (6U + 3U * 2U)

/* Frequency/rate context fields are 64-bit two's complement Hz with the radix point after
 * bit 20 (VITA 49.2 rule 9.5.1-1 family): value_hz * 2^20. */
static uint64_t vita49_fixed_hz(uint64_t hz)
{
    return hz << 20U;
}

size_t vita49_context_packet_size(void)
{
    return (size_t)VITA49_CONTEXT_WORDS * 4U;
}

bool vita49_write_context_packet(const vita49_context_packet_t *packet, uint8_t *out, size_t out_size, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (packet == NULL || out == NULL || out_size < vita49_context_packet_size()) {
        return false;
    }
    const uint64_t seconds = packet->timestamp_ns / 1000000000ULL;
    const uint64_t fractional_ps = (packet->timestamp_ns % 1000000000ULL) * 1000ULL;
    const uint32_t header =
        (VITA49_PACKET_TYPE_CONTEXT << 28U) |
        (VITA49_TSI_UTC << 22U) |
        (VITA49_TSF_REAL_TIME << 20U) |
        (((uint32_t)packet->sequence & 0x0fU) << 16U) |
        VITA49_CONTEXT_WORDS;
    uint32_t cif0 = VITA49_CIF0_BANDWIDTH | VITA49_CIF0_RF_REFERENCE_FREQUENCY | VITA49_CIF0_SAMPLE_RATE;
    if (packet->changed) {
        cif0 |= VITA49_CIF0_CHANGE_INDICATOR;
    }
    write_be32(out, header);
    write_be32(out + 4, packet->stream_id);
    write_be32(out + 8, (uint32_t)seconds);
    write_be64(out + 12, fractional_ps);
    write_be32(out + 20, cif0);
    /* Fields follow in descending CIF0 bit order. */
    write_be64(out + 24, vita49_fixed_hz(packet->bandwidth_hz));
    write_be64(out + 32, vita49_fixed_hz(packet->rf_reference_frequency_hz));
    write_be64(out + 40, vita49_fixed_hz(packet->sample_rate_hz));
    if (written != NULL) {
        *written = vita49_context_packet_size();
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
    const size_t payload_bytes = packet->payload_samples * sizeof(iq_ci16_t);
    const size_t total_bytes = VITA49_IF_DATA_HEADER_BYTES + payload_bytes;
    if (packet == NULL || out == NULL || packet->payload == NULL || out_size < total_bytes || total_bytes % 4U != 0U ||
        total_bytes / 4U > UINT16_MAX) {
        return false;
    }
    const uint16_t packet_words = (uint16_t)(total_bytes / 4U);
    const uint64_t seconds = packet->timestamp_ns / 1000000000ULL;
    const uint64_t fractional_ps = (packet->timestamp_ns % 1000000000ULL) * 1000ULL;
    const uint32_t header =
        (VITA49_PACKET_TYPE_IF_DATA << 28U) |
        VITA49_PSI_VITA49_2 |
        (VITA49_TSI_UTC << 22U) |
        (VITA49_TSF_REAL_TIME << 20U) |
        (((uint32_t)packet->sequence & 0x0fU) << 16U) |
        packet_words;

    write_be32(out, header);
    write_be32(out + 4, packet->stream_id);
    write_be32(out + 8, (uint32_t)seconds);
    write_be64(out + 12, fractional_ps);
    memcpy(out + VITA49_IF_DATA_HEADER_BYTES, packet->payload, payload_bytes);
    if (written != NULL) {
        *written = total_bytes;
    }
    return true;
}
