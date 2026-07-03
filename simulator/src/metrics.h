#ifndef METRICS_H
#define METRICS_H

#include <stdatomic.h>
#include <stdint.h>

#include "sim_types.h"

typedef struct {
    atomic_uint_fast64_t samples_rendered;
    atomic_uint_fast64_t samples_sent;
    atomic_uint_fast64_t samples_missed;
    atomic_uint_fast64_t samples_late;
    atomic_uint_fast64_t samples_send_dropped;
    atomic_uint_fast64_t udp_packets_sent;
    atomic_uint_fast64_t udp_bytes_sent;
    atomic_uint_fast64_t udp_send_errors;
    atomic_uint_fast64_t udp_send_would_block;
    atomic_uint_fast64_t udp_send_no_buffer;
    atomic_uint_fast64_t udp_send_other_errors;
    atomic_uint_fast64_t ringbuffer_overruns;
    atomic_uint_fast64_t ringbuffer_underruns;
    atomic_uint_fast64_t samples_dropped;
    atomic_bool active;
} stream_metrics_t;

typedef struct {
    atomic_uint_fast64_t samples_rendered;
    atomic_uint_fast64_t samples_sent;
    atomic_uint_fast64_t samples_missed;
    atomic_uint_fast64_t samples_late;
    atomic_uint_fast64_t samples_send_dropped;
    atomic_uint_fast64_t udp_packets_sent;
    atomic_uint_fast64_t udp_bytes_sent;
    atomic_uint_fast64_t udp_send_errors;
    atomic_uint_fast64_t udp_send_would_block;
    atomic_uint_fast64_t udp_send_no_buffer;
    atomic_uint_fast64_t udp_send_other_errors;
    atomic_uint_fast64_t ringbuffer_overruns;
    atomic_uint_fast64_t ringbuffer_underruns;
    atomic_uint_fast64_t samples_dropped;
    atomic_uint_fast64_t active_streams;
    stream_metrics_t streams[1 + SIM_DDC_COUNT];
} receiver_metrics_t;

static inline void stream_metrics_init(stream_metrics_t *metrics)
{
    atomic_init(&metrics->samples_rendered, 0);
    atomic_init(&metrics->samples_sent, 0);
    atomic_init(&metrics->samples_missed, 0);
    atomic_init(&metrics->samples_late, 0);
    atomic_init(&metrics->samples_send_dropped, 0);
    atomic_init(&metrics->udp_packets_sent, 0);
    atomic_init(&metrics->udp_bytes_sent, 0);
    atomic_init(&metrics->udp_send_errors, 0);
    atomic_init(&metrics->udp_send_would_block, 0);
    atomic_init(&metrics->udp_send_no_buffer, 0);
    atomic_init(&metrics->udp_send_other_errors, 0);
    atomic_init(&metrics->ringbuffer_overruns, 0);
    atomic_init(&metrics->ringbuffer_underruns, 0);
    atomic_init(&metrics->samples_dropped, 0);
    atomic_init(&metrics->active, false);
}

static inline void receiver_metrics_init(receiver_metrics_t *metrics)
{
    atomic_init(&metrics->samples_rendered, 0);
    atomic_init(&metrics->samples_sent, 0);
    atomic_init(&metrics->samples_missed, 0);
    atomic_init(&metrics->samples_late, 0);
    atomic_init(&metrics->samples_send_dropped, 0);
    atomic_init(&metrics->udp_packets_sent, 0);
    atomic_init(&metrics->udp_bytes_sent, 0);
    atomic_init(&metrics->udp_send_errors, 0);
    atomic_init(&metrics->udp_send_would_block, 0);
    atomic_init(&metrics->udp_send_no_buffer, 0);
    atomic_init(&metrics->udp_send_other_errors, 0);
    atomic_init(&metrics->ringbuffer_overruns, 0);
    atomic_init(&metrics->ringbuffer_underruns, 0);
    atomic_init(&metrics->samples_dropped, 0);
    atomic_init(&metrics->active_streams, 0);
    for (size_t i = 0; i < 1 + SIM_DDC_COUNT; i++) {
        stream_metrics_init(&metrics->streams[i]);
    }
}

#endif
