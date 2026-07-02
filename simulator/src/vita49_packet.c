#include "vita49_packet.h"

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

uint32_t vita49_stream_id(uint32_t receiver_id, bool is_ddc, uint32_t ddc_id)
{
    const uint32_t stream_index = is_ddc ? (1U + ddc_id) : 0U;
    return 0x53440000U | ((receiver_id & 0xffU) << 8U) | (stream_index & 0xffU);
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
    if (packet == NULL || out == NULL || packet->payload == NULL || out_size < total_bytes || total_bytes % 4U != 0U) {
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
