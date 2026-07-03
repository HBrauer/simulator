#ifndef UDP_OUTPUT_H
#define UDP_OUTPUT_H

#include <stdbool.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define UDP_OUTPUT_MAX_BATCH 16U

typedef struct {
    int fd;
    struct sockaddr_in addr;
} udp_output_t;

bool udp_output_open(udp_output_t *output, const char *host, uint16_t port, const char *multicast_interface);
void udp_output_close(udp_output_t *output);
bool udp_output_send(udp_output_t *output, const void *data, size_t bytes, size_t *sent_bytes, int *error_code);
bool udp_output_send_batch(
    udp_output_t *output,
    const void *const *data,
    const size_t *bytes,
    size_t count,
    size_t *sent_messages,
    size_t *sent_bytes,
    int *error_code);

#endif
