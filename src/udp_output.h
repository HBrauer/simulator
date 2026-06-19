#ifndef UDP_OUTPUT_H
#define UDP_OUTPUT_H

#include <stdbool.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    struct sockaddr_in addr;
} udp_output_t;

bool udp_output_open(udp_output_t *output, const char *host, uint16_t port);
void udp_output_close(udp_output_t *output);
bool udp_output_send(udp_output_t *output, const void *data, size_t bytes, size_t *sent_bytes);

#endif
