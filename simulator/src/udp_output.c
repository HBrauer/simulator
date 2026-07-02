#include "udp_output.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static bool ipv4_is_multicast(struct in_addr addr)
{
    return IN_MULTICAST(ntohl(addr.s_addr));
}

bool udp_output_open(udp_output_t *output, const char *host, uint16_t port)
{
    output->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (output->fd < 0) {
        return false;
    }
    memset(&output->addr, 0, sizeof(output->addr));
    output->addr.sin_family = AF_INET;
    output->addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &output->addr.sin_addr) != 1) {
        close(output->fd);
        output->fd = -1;
        return false;
    }
    if (ipv4_is_multicast(output->addr.sin_addr)) {
        const unsigned char ttl = 1;
        const unsigned char loop = 1;
        (void)setsockopt(output->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        (void)setsockopt(output->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    }
    return true;
}

void udp_output_close(udp_output_t *output)
{
    if (output->fd >= 0) {
        close(output->fd);
        output->fd = -1;
    }
}

bool udp_output_send(udp_output_t *output, const void *data, size_t bytes, size_t *sent_bytes)
{
    const ssize_t n = sendto(output->fd, data, bytes, 0, (const struct sockaddr *)&output->addr, sizeof(output->addr));
    if (n < 0) {
        *sent_bytes = 0;
        return false;
    }
    *sent_bytes = (size_t)n;
    return (size_t)n == bytes;
}
