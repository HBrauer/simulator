#ifndef UDP_OUTPUT_H
#define UDP_OUTPUT_H

#include <stdbool.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define UDP_OUTPUT_MAX_BATCH 16U
/* SO_SNDBUF requested per stream socket; the kernel silently caps it at net.core.wmem_max. */
#define UDP_OUTPUT_SNDBUF_BYTES (16 * 1024 * 1024)

typedef struct {
    int fd;
    struct sockaddr_in addr;
} udp_output_t;

bool udp_output_open(udp_output_t *output, const char *host, uint16_t port, const char *multicast_interface);
void udp_output_close(udp_output_t *output);
/* Effective SO_SNDBUF as the kernel reports it (about twice the usable payload bytes). */
bool udp_output_sndbuf(const udp_output_t *output, int *bytes);
/* Connect the (probe) socket to its destination and read the route's IP MTU. Only for a socket
 * opened to probe with: a connected socket keeps working, but this is not needed to send. */
bool udp_output_path_mtu(udp_output_t *output, int *mtu);
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
