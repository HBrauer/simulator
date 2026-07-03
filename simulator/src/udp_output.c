#include "udp_output.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static bool ipv4_is_multicast(struct in_addr addr)
{
    return IN_MULTICAST(ntohl(addr.s_addr));
}

bool udp_output_open(udp_output_t *output, const char *host, uint16_t port, const char *multicast_interface)
{
    output->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (output->fd < 0) {
        return false;
    }
    int sndbuf = 16 * 1024 * 1024;
    (void)setsockopt(output->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    const int flags = fcntl(output->fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(output->fd, F_SETFL, flags | O_NONBLOCK);
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
        if (multicast_interface != NULL && multicast_interface[0] != '\0') {
            struct in_addr iface;
            if (inet_pton(AF_INET, multicast_interface, &iface) != 1) {
                close(output->fd);
                output->fd = -1;
                return false;
            }
            (void)setsockopt(output->fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
        }
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

bool udp_output_send(udp_output_t *output, const void *data, size_t bytes, size_t *sent_bytes, int *error_code)
{
    const ssize_t n = sendto(output->fd, data, bytes, 0, (const struct sockaddr *)&output->addr, sizeof(output->addr));
    if (n < 0) {
        *sent_bytes = 0;
        if (error_code != NULL) {
            *error_code = errno;
        }
        return false;
    }
    *sent_bytes = (size_t)n;
    if (error_code != NULL) {
        *error_code = 0;
    }
    return (size_t)n == bytes;
}

bool udp_output_send_batch(
    udp_output_t *output,
    const void *const *data,
    const size_t *bytes,
    size_t count,
    size_t *sent_messages,
    size_t *sent_bytes,
    int *error_code)
{
    if (sent_messages != NULL) {
        *sent_messages = 0;
    }
    if (sent_bytes != NULL) {
        *sent_bytes = 0;
    }
    if (error_code != NULL) {
        *error_code = 0;
    }
    if (output == NULL || data == NULL || bytes == NULL || count == 0U || count > UDP_OUTPUT_MAX_BATCH) {
        if (error_code != NULL) {
            *error_code = EINVAL;
        }
        return false;
    }

#ifdef __linux__
    struct mmsghdr messages[UDP_OUTPUT_MAX_BATCH];
    struct iovec iovecs[UDP_OUTPUT_MAX_BATCH];
    memset(messages, 0, sizeof(messages));
    memset(iovecs, 0, sizeof(iovecs));
    for (size_t i = 0; i < count; i++) {
        iovecs[i].iov_base = (void *)data[i];
        iovecs[i].iov_len = bytes[i];
        messages[i].msg_hdr.msg_name = &output->addr;
        messages[i].msg_hdr.msg_namelen = sizeof(output->addr);
        messages[i].msg_hdr.msg_iov = &iovecs[i];
        messages[i].msg_hdr.msg_iovlen = 1;
    }

    const int sent = sendmmsg(output->fd, messages, (unsigned int)count, 0);
    if (sent < 0) {
        if (error_code != NULL) {
            *error_code = errno;
        }
        return false;
    }
    size_t total_bytes = 0;
    for (int i = 0; i < sent; i++) {
        total_bytes += messages[i].msg_len;
    }
    if (sent_messages != NULL) {
        *sent_messages = (size_t)sent;
    }
    if (sent_bytes != NULL) {
        *sent_bytes = total_bytes;
    }
    if ((size_t)sent < count) {
        if (error_code != NULL) {
            *error_code = EAGAIN;
        }
        return false;
    }
    return true;
#else
    size_t total_messages = 0;
    size_t total_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        size_t sent = 0;
        int err = 0;
        if (!udp_output_send(output, data[i], bytes[i], &sent, &err)) {
            if (sent_messages != NULL) {
                *sent_messages = total_messages;
            }
            if (sent_bytes != NULL) {
                *sent_bytes = total_bytes;
            }
            if (error_code != NULL) {
                *error_code = err;
            }
            return false;
        }
        total_messages++;
        total_bytes += sent;
    }
    if (sent_messages != NULL) {
        *sent_messages = total_messages;
    }
    if (sent_bytes != NULL) {
        *sent_bytes = total_bytes;
    }
    return true;
#endif
}
