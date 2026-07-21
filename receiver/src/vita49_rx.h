#ifndef VITA49_RX_H
#define VITA49_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA49_RX_IF_DATA_HEADER_BYTES 20U
#define VITA49_RX_PACKET_TYPE_IF_DATA 1U
#define VITA49_RX_PACKET_TYPE_CONTEXT 4U

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

/* IF context packet announcing the stream configuration in-band (see the simulator's
 * vita49_write_context_packet): RF reference frequency, bandwidth, sample rate, and the
 * reference level (RF power at digital full scale) for absolute-power calibration. */
typedef struct {
    uint8_t sequence;
    uint32_t stream_id;
    uint64_t timestamp_ns;
    bool changed;
    uint64_t bandwidth_hz;
    uint64_t rf_reference_frequency_hz;
    uint64_t sample_rate_hz;
    bool has_reference_level;      /* true when the packet carried the Reference Level field */
    double reference_level_dbm;    /* RF power (dBm) at 0 dBFS; valid only if has_reference_level */
} vita49_rx_context_t;

/* Packet type from the first header word, or 0xff if the buffer is too small. */
uint8_t vita49_rx_packet_type(const uint8_t *data, size_t bytes);
bool vita49_rx_parse_if_data(const uint8_t *data, size_t bytes, vita49_rx_packet_t *packet);
bool vita49_rx_parse_context(const uint8_t *data, size_t bytes, vita49_rx_context_t *context);

#endif
