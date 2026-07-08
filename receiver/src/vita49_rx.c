#include "vita49_rx.h"

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

uint8_t vita49_rx_packet_type(const uint8_t *data, size_t bytes)
{
    if (data == 0 || bytes < 4U) {
        return 0xffU;
    }
    return (uint8_t)(read_be32(data) >> 28U);
}

/* CIF0 bits the simulator emits; anything else means a layout we do not know. */
#define VITA49_RX_CIF0_CHANGE (1U << 31U)
#define VITA49_RX_CIF0_BANDWIDTH (1U << 29U)
#define VITA49_RX_CIF0_RF_REFERENCE (1U << 27U)
#define VITA49_RX_CIF0_SAMPLE_RATE (1U << 21U)

bool vita49_rx_parse_context(const uint8_t *data, size_t bytes, vita49_rx_context_t *context)
{
    if (data == 0 || context == 0 || bytes < 24U) {
        return false;
    }
    const uint32_t header = read_be32(data);
    const uint8_t packet_type = (uint8_t)(header >> 28U);
    const uint16_t packet_words = (uint16_t)(header & 0xffffU);
    if (packet_type != VITA49_RX_PACKET_TYPE_CONTEXT || (size_t)packet_words * 4U != bytes) {
        return false;
    }
    const uint32_t cif0 = read_be32(data + 20);
    const uint32_t known = VITA49_RX_CIF0_CHANGE | VITA49_RX_CIF0_BANDWIDTH |
                           VITA49_RX_CIF0_RF_REFERENCE | VITA49_RX_CIF0_SAMPLE_RATE;
    if ((cif0 & ~known) != 0U) {
        return false; /* unknown fields shift the layout; refuse rather than misread */
    }
    const uint64_t seconds = read_be32(data + 8);
    const uint64_t fractional_ps = read_be64(data + 12);
    memset(context, 0, sizeof(*context));
    context->sequence = (uint8_t)((header >> 16U) & 0x0fU);
    context->stream_id = read_be32(data + 4);
    context->timestamp_ns = seconds * 1000000000ULL + fractional_ps / 1000ULL;
    context->changed = (cif0 & VITA49_RX_CIF0_CHANGE) != 0U;
    /* Fields follow the CIF0 word in descending bit order, 64-bit fixed point with the
     * radix point after bit 20. */
    size_t offset = 24U;
    if ((cif0 & VITA49_RX_CIF0_BANDWIDTH) != 0U) {
        if (offset + 8U > bytes) {
            return false;
        }
        context->bandwidth_hz = read_be64(data + offset) >> 20U;
        offset += 8U;
    }
    if ((cif0 & VITA49_RX_CIF0_RF_REFERENCE) != 0U) {
        if (offset + 8U > bytes) {
            return false;
        }
        context->rf_reference_frequency_hz = read_be64(data + offset) >> 20U;
        offset += 8U;
    }
    if ((cif0 & VITA49_RX_CIF0_SAMPLE_RATE) != 0U) {
        if (offset + 8U > bytes) {
            return false;
        }
        context->sample_rate_hz = read_be64(data + offset) >> 20U;
        offset += 8U;
    }
    return true;
}

bool vita49_rx_parse_if_data(const uint8_t *data, size_t bytes, vita49_rx_packet_t *packet)
{
    if (data == 0 || packet == 0 || bytes < VITA49_RX_IF_DATA_HEADER_BYTES) {
        return false;
    }
    const uint32_t header = read_be32(data);
    const uint8_t packet_type = (uint8_t)(header >> 28U);
    const uint16_t packet_words = (uint16_t)(header & 0xffffU);
    const size_t packet_bytes = (size_t)packet_words * 4U;
    if (packet_type != 1U || packet_words == 0U || packet_bytes != bytes) {
        return false;
    }
    const uint8_t tsi = (uint8_t)((header >> 22U) & 0x03U);
    const uint8_t tsf = (uint8_t)((header >> 20U) & 0x03U);
    if (tsi != 1U || tsf != 2U) {
        return false;
    }
    const uint64_t seconds = read_be32(data + 8);
    const uint64_t fractional_ps = read_be64(data + 12);
    *packet = (vita49_rx_packet_t){
        .packet_type = packet_type,
        .vita49_2 = ((header >> 25U) & 0x01U) != 0U,
        .tsi = tsi,
        .tsf = tsf,
        .sequence = (uint8_t)((header >> 16U) & 0x0fU),
        .packet_words = packet_words,
        .stream_id = read_be32(data + 4),
        .timestamp_ns = seconds * 1000000000ULL + fractional_ps / 1000ULL,
        .payload = data + VITA49_RX_IF_DATA_HEADER_BYTES,
        .payload_bytes = bytes - VITA49_RX_IF_DATA_HEADER_BYTES,
    };
    return packet->payload_bytes % 4U == 0U;
}
