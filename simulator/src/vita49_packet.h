#ifndef VITA49_PACKET_H
#define VITA49_PACKET_H

#include "sim_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Prefix bytes before the payload/CIF for a packet without a Class ID: header word, stream
 * id, integer-seconds, and the 64-bit fractional timestamp. A present Class ID inserts two
 * more words (VITA49_CLASS_ID_BYTES) between the stream id and the timestamps. */
#define VITA49_IF_DATA_HEADER_BYTES 20U
#define VITA49_CLASS_ID_BYTES 8U
#define VITA49_PACKET_TYPE_IF_DATA 1U
#define VITA49_PACKET_TYPE_CONTEXT 4U
#define VITA49_TSI_UTC 1U
#define VITA49_TSF_REAL_TIME 2U

/* Optional Class ID (VITA 49.2 §5.1.3): a 24-bit OUI plus 16-bit information and packet
 * class codes. Packed into two words as (oui in bits 23..0) then (info << 16 | packet). */
typedef struct {
    uint32_t oui; /* 24-bit Organizationally Unique Identifier */
    uint16_t information_class_code;
    uint16_t packet_class_code;
} vita49_class_id_t;

typedef struct {
    uint32_t stream_id;
    uint8_t sequence;
    uint64_t timestamp_ns;
    bool class_id_present;
    vita49_class_id_t class_id;
    const iq_ci16_t *payload;
    size_t payload_samples;
} vita49_if_data_packet_t;

/* IF Context packet (VITA 49.2 packet type 4) announcing the stream's RF reference
 * frequency, bandwidth, and sample rate in-band, so UDP consumers can follow retunes
 * without polling the REST API. */
typedef struct {
    uint32_t stream_id;
    uint8_t sequence;
    uint64_t timestamp_ns;
    bool changed; /* sets the CIF0 context-field-change indicator */
    bool class_id_present;
    vita49_class_id_t class_id;
    uint64_t rf_reference_frequency_hz;
    uint64_t bandwidth_hz;
    uint64_t sample_rate_hz;
} vita49_context_packet_t;

size_t vita49_if_data_packet_size(size_t payload_samples, bool class_id_present);
size_t vita49_context_packet_size(bool class_id_present);
bool vita49_write_if_data_packet(const vita49_if_data_packet_t *packet, uint8_t *out, size_t out_size, size_t *written);
bool vita49_write_context_packet(const vita49_context_packet_t *packet, uint8_t *out, size_t out_size, size_t *written);

#endif
