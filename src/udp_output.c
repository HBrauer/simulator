#include "udp_output.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool udp_output_open(udp_output_t *output, const char *host, uint16_t port)
{
    output->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (output->fd < 0) {
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(output->fd);
        output->fd = -1;
        return false;
    }
    if (connect(output->fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(output->fd);
        output->fd = -1;
        return false;
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
    const ssize_t n = send(output->fd, data, bytes, 0);
    if (n < 0) {
        *sent_bytes = 0;
        return false;
    }
    *sent_bytes = (size_t)n;
    return (size_t)n == bytes;
}
