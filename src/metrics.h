#ifndef METRICS_H
#define METRICS_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    atomic_uint_fast64_t samples_rendered;
    atomic_uint_fast64_t udp_packets_sent;
    atomic_uint_fast64_t udp_bytes_sent;
    atomic_uint_fast64_t udp_send_errors;
    atomic_uint_fast64_t active_streams;
} receiver_metrics_t;

static inline void receiver_metrics_init(receiver_metrics_t *metrics)
{
    atomic_init(&metrics->samples_rendered, 0);
    atomic_init(&metrics->udp_packets_sent, 0);
    atomic_init(&metrics->udp_bytes_sent, 0);
    atomic_init(&metrics->udp_send_errors, 0);
    atomic_init(&metrics->active_streams, 0);
}

#endif
