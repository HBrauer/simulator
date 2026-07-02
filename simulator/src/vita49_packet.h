#ifndef VITA49_PACKET_H
#define VITA49_PACKET_H

#include "sim_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA49_IF_DATA_HEADER_BYTES 20U
#define VITA49_PACKET_TYPE_IF_DATA 1U
#define VITA49_TSI_UTC 1U
#define VITA49_TSF_REAL_TIME 2U

typedef struct {
    uint32_t stream_id;
    uint8_t sequence;
    uint64_t timestamp_ns;
    const iq_ci16_t *payload;
    size_t payload_samples;
} vita49_if_data_packet_t;

size_t vita49_if_data_packet_size(size_t payload_samples);
uint32_t vita49_stream_id(uint32_t receiver_id, bool is_ddc, uint32_t ddc_id);
bool vita49_write_if_data_packet(const vita49_if_data_packet_t *packet, uint8_t *out, size_t out_size, size_t *written);

#endif
