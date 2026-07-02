#ifndef VITA49_RX_H
#define VITA49_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA49_RX_IF_DATA_HEADER_BYTES 20U

typedef struct {
    uint8_t packet_type;
    bool vita49_2;
    uint8_t tsi;
    uint8_t tsf;
    uint8_t sequence;
    uint16_t packet_words;
    uint32_t stream_id;
    uint64_t timestamp_ns;
    const uint8_t *payload;
    size_t payload_bytes;
} vita49_rx_packet_t;

bool vita49_rx_parse_if_data(const uint8_t *data, size_t bytes, vita49_rx_packet_t *packet);

#endif
