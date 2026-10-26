#ifndef VITA49_RX_H
#define VITA49_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA49_RX_IF_DATA_HEADER_BYTES 20U
#define VITA49_RX_PACKET_TYPE_IF_DATA 1U
#define VITA49_RX_PACKET_TYPE_CONTEXT 4U

/* On-wire sample format decoded from the context packet's Data Packet Payload Format field.
 * CI16 = 4 bytes/sample; CI24 and CF32 = 8. UNKNOWN means the field described a layout this
 * receiver cannot decode (the caller should keep its previous format). */
typedef enum {
    VITA49_RX_FORMAT_CI16,
    VITA49_RX_FORMAT_CI24,
    VITA49_RX_FORMAT_CF32,
    VITA49_RX_FORMAT_UNKNOWN
} vita49_rx_format_t;

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
    bool has_format;               /* true when the packet carried a Data Packet Payload Format field */
    vita49_rx_format_t format;     /* paired IF-data sample format; valid only if has_format */
} vita49_rx_context_t;

/* VITA 49.1 VRL framing: "VRLP", frame count/size word, VRT packet(s), then a CRC-32 or
 * "VEND" trailer. */
#define VITA49_RX_VRL_FRAME_ALIGNMENT_WORD 0x56524C50U
#define VITA49_RX_VRL_NO_CRC_WORD 0x56454E44U
#define VITA49_RX_VRL_OVERHEAD_BYTES 12U

/* Locate the VRT packet in a datagram. A datagram that does not start with "VRLP" is taken as
 * a bare VRT packet (*framed = false; framed may be NULL). A VRL frame is validated (frame size matches the
 * datagram; trailer is "VEND" or a matching IEEE 802.3 CRC-32) and *vrt points at its payload.
 * Returns false for a malformed frame or a CRC mismatch. */
bool vita49_rx_vrl_unwrap(const uint8_t *data, size_t bytes, const uint8_t **vrt, size_t *vrt_bytes, bool *framed);
uint32_t vita49_rx_vrl_crc32(const uint8_t *data, size_t bytes);

/* Packet type from the first header word, or 0xff if the buffer is too small. */
uint8_t vita49_rx_packet_type(const uint8_t *data, size_t bytes);
bool vita49_rx_parse_if_data(const uint8_t *data, size_t bytes, vita49_rx_packet_t *packet);
bool vita49_rx_parse_context(const uint8_t *data, size_t bytes, vita49_rx_context_t *context);

#endif
