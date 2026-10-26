#include "host_check.h"

#include "streamer.h"
#include "udp_output.h"
#include "vita49_packet.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/timex.h>

/* IPv4 + UDP headers on top of the UDP payload when comparing against the IP MTU. */
#define HOST_CHECK_IP_UDP_HEADER_BYTES 28U

__attribute__((format(printf, 4, 5))) static void report(FILE *out, host_check_result_t *result, bool error, const char *fmt, ...)
{
    if (error) {
        result->errors++;
    } else {
        result->warnings++;
    }
    if (out == NULL) {
        return;
    }
    fputs(error ? "host check error: " : "host check warning: ", out);
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fputc('\n', out);
}

/* Render a CPU set as "0-3,6" for messages. */
static void format_cpu_set(const cpu_set_t *set, char *buf, size_t size)
{
    size_t used = 0;
    buf[0] = '\0';
    for (int cpu = 0; cpu < CPU_SETSIZE && used < size; cpu++) {
        if (!CPU_ISSET((size_t)cpu, set)) {
            continue;
        }
        int last = cpu;
        while (last + 1 < CPU_SETSIZE && CPU_ISSET((size_t)(last + 1), set)) {
            last++;
        }
        const int n = last > cpu
            ? snprintf(buf + used, size - used, "%s%d-%d", used > 0 ? "," : "", cpu, last)
            : snprintf(buf + used, size - used, "%s%d", used > 0 ? "," : "", cpu);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
        cpu = last;
    }
}

static uint32_t channel_max_rate_hz(const channel_config_t *channel)
{
    uint32_t rate = channel->sample_rate_hz;
    for (size_t r = 0; r < channel->rate_count; r++) {
        if (channel->rates[r].sample_rate_hz > rate) {
            rate = channel->rates[r].sample_rate_hz;
        }
    }
    return rate;
}

size_t host_check_max_datagram_bytes(const simulator_config_t *config, const receiver_config_t *receiver, size_t block_samples)
{
    size_t max_bytes = 0;
    for (size_t c = 0; c < receiver->channel_count; c++) {
        const channel_config_t *channel = &receiver->channels[c];
        const size_t samples = streamer_block_samples_for_rate(block_samples, channel_max_rate_hz(channel));
        size_t bytes = vita49_if_data_packet_size(samples, config->class_id_present, channel->output_format);
        if (receiver->vrl_framing) {
            bytes += VITA49_VRL_OVERHEAD_BYTES;
        }
        if (bytes > max_bytes) {
            max_bytes = bytes;
        }
    }
    return max_bytes;
}

void host_check_stream_cpus(const simulator_config_t *config, const cpu_set_t *allowed, FILE *out, host_check_result_t *result)
{
    char allowed_text[256];
    format_cpu_set(allowed, allowed_text, sizeof(allowed_text));
    for (size_t i = 0; i < config->stream_cpu_count; i++) {
        const int cpu = config->stream_cpus[i];
        if (cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET((size_t)cpu, allowed)) {
            report(out, result, true,
                   "stream_cpus lists CPU %d, but this process may only run on CPUs %s "
                   "(check the CPU count, taskset, or the service's CPUAffinity=/AllowedCPUs=)",
                   cpu, allowed_text);
        }
    }
}

void host_check_thread_count(const simulator_config_t *config, const cpu_set_t *allowed, FILE *out, host_check_result_t *result)
{
    size_t threads = 0;
    for (size_t r = 0; r < config->receiver_count; r++) {
        const receiver_config_t *receiver = &config->receivers[r];
        for (size_t c = 0; c < receiver->channel_count; c++) {
            const uint32_t render_threads = receiver->channels[c].render_threads > 0U ? receiver->channels[c].render_threads : 1U;
            threads += (size_t)render_threads + 1U; /* render thread(s) + UDP send thread */
        }
    }
    size_t cpus = 0;
    if (config->stream_cpu_count > 0) {
        cpu_set_t pinned;
        CPU_ZERO(&pinned);
        for (size_t i = 0; i < config->stream_cpu_count; i++) {
            const int cpu = config->stream_cpus[i];
            if (cpu >= 0 && cpu < CPU_SETSIZE && CPU_ISSET((size_t)cpu, allowed)) {
                CPU_SET((size_t)cpu, &pinned);
            }
        }
        cpus = (size_t)CPU_COUNT(&pinned);
    } else {
        cpus = (size_t)CPU_COUNT(allowed);
    }
    if (cpus > 0 && threads > cpus) {
        report(out, result, false,
               "%zu stream threads (one UDP thread plus render_threads per channel) share %zu CPU%s%s; "
               "high-rate channels may fall behind real time (watch the late/underrun metrics)",
               threads, cpus, cpus == 1 ? "" : "s", config->stream_cpu_count > 0 ? " from stream_cpus" : "");
    }
}

void host_check_sndbuf(const simulator_config_t *config, size_t block_samples, int reported_sndbuf, FILE *out, host_check_result_t *result)
{
    size_t datagram = 0;
    for (size_t r = 0; r < config->receiver_count; r++) {
        const size_t bytes = host_check_max_datagram_bytes(config, &config->receivers[r], block_samples);
        if (bytes > datagram) {
            datagram = bytes;
        }
    }
    /* The non-blocking socket must queue a full batch, with headroom for the previous one still
     * draining; the kernel charges each datagram's size plus skb overhead against SO_SNDBUF. */
    const size_t needed = 2U * STREAM_SEND_BATCH_SIZE * (datagram + HOST_CHECK_SKB_OVERHEAD_BYTES);
    if (datagram == 0 || reported_sndbuf <= 0 || (size_t)reported_sndbuf >= needed) {
        return;
    }
    report(out, result, false,
           "UDP send buffer is %d KiB (requested %d KiB, capped by net.core.wmem_max) but bursts of %u "
           "%zu-byte datagrams need about %zu KiB; sends may fail and count as drops. "
           "Fix: sudo sysctl -w net.core.wmem_max=%d",
           reported_sndbuf / 1024, UDP_OUTPUT_SNDBUF_BYTES / 1024, STREAM_SEND_BATCH_SIZE, datagram,
           needed / 1024U, UDP_OUTPUT_SNDBUF_BYTES);
}

void host_check_datagram_mtu(const simulator_config_t *config, const receiver_config_t *receiver, size_t block_samples, int path_mtu, FILE *out, host_check_result_t *result)
{
    const size_t datagram = host_check_max_datagram_bytes(config, receiver, block_samples);
    if (path_mtu <= 0 || datagram + HOST_CHECK_IP_UDP_HEADER_BYTES <= (size_t)path_mtu) {
        return;
    }
    const size_t fragments = (datagram + HOST_CHECK_IP_UDP_HEADER_BYTES + (size_t)path_mtu - 21U) / ((size_t)path_mtu - 20U);
    report(out, result, false,
           "receiver %u: %zu-byte datagrams exceed the %d-byte MTU towards %s and are split into ~%zu IP "
           "fragments (losing one loses the packet). Lower stream_block_samples or use jumbo frames (MTU 9000)",
           receiver->id, datagram, path_mtu, receiver->udp_output_host, fragments);
}

void host_check_clock(bool synchronised, FILE *out, host_check_result_t *result)
{
    if (synchronised) {
        return;
    }
    report(out, result, false,
           "the system clock is not NTP/PTP-synchronised; VITA timestamps and replay positions follow "
           "CLOCK_REALTIME and will not line up across hosts (check: timedatectl / chronyc tracking)");
}

void host_check_memory(size_t loaded_bytes, size_t available_bytes, FILE *out, host_check_result_t *result)
{
    if (available_bytes == 0 || (double)loaded_bytes <= HOST_CHECK_MEMORY_WARN_FRACTION * (double)available_bytes) {
        return;
    }
    report(out, result, false,
           "asset data takes %zu MiB of the %zu MiB that was available; the process risks being OOM-killed. "
           "Lower asset_cache_max_bytes so large passthrough captures are memory-mapped instead of copied",
           loaded_bytes >> 20U, available_bytes >> 20U);
}

static bool clock_synchronised(bool *known)
{
    struct timex tx;
    memset(&tx, 0, sizeof(tx)); /* modes = 0: read only */
    const int state = adjtimex(&tx);
    *known = state >= 0;
    return state != TIME_ERROR && (tx.status & STA_UNSYNC) == 0;
}

void host_check_startup(const simulator_config_t *config, size_t block_samples, bool check_clock, FILE *out, host_check_result_t *result)
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        host_check_stream_cpus(config, &allowed, out, result);
        host_check_thread_count(config, &allowed, out, result);
    }

    bool sndbuf_checked = false;
    for (size_t r = 0; r < config->receiver_count; r++) {
        const receiver_config_t *receiver = &config->receivers[r];
        if (receiver->channel_count == 0) {
            continue;
        }
        /* Probe with a socket set up exactly like a stream socket (same SO_SNDBUF request and
         * multicast interface), so the readings match what the streams will get. */
        udp_output_t probe = {.fd = -1};
        if (!udp_output_open(&probe, receiver->udp_output_host, receiver->channels[0].udp_output.port, receiver->udp_multicast_interface)) {
            continue;
        }
        int sndbuf = 0;
        if (!sndbuf_checked && udp_output_sndbuf(&probe, &sndbuf)) {
            host_check_sndbuf(config, block_samples, sndbuf, out, result);
            sndbuf_checked = true;
        }
        int mtu = 0;
        if (udp_output_path_mtu(&probe, &mtu)) {
            host_check_datagram_mtu(config, receiver, block_samples, mtu, out, result);
        }
        udp_output_close(&probe);
    }

    if (check_clock) {
        bool known = false;
        const bool synced = clock_synchronised(&known);
        if (known) {
            host_check_clock(synced, out, result);
        }
    }
}

/* First "<key> <value> kB" line of a /proc file, in bytes; 0 if absent. */
static size_t read_proc_kb(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    char line[256];
    size_t value = 0;
    const size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, key_len) == 0) {
            unsigned long long kb = 0;
            if (sscanf(line + key_len, " %llu", &kb) == 1) {
                value = (size_t)kb * 1024U;
            }
            break;
        }
    }
    fclose(f);
    return value;
}

static bool read_u64_file(const char *path, uint64_t *value)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    unsigned long long v = 0;
    const bool ok = fscanf(f, "%llu", &v) == 1; /* "max" (no limit) fails to parse */
    fclose(f);
    *value = (uint64_t)v;
    return ok;
}

/* Headroom under this process's cgroup v2 memory.max, or 0 when unlimited/unknown. */
static size_t cgroup_memory_headroom_bytes(void)
{
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (f == NULL) {
        return 0;
    }
    char line[512];
    char path[600] = "";
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "0::", 3) == 0) {
            line[strcspn(line, "\n")] = '\0';
            snprintf(path, sizeof(path), "/sys/fs/cgroup%s", line + 3);
            break;
        }
    }
    fclose(f);
    if (path[0] == '\0') {
        return 0;
    }
    char file[700];
    uint64_t max = 0;
    uint64_t current = 0;
    snprintf(file, sizeof(file), "%s/memory.max", path);
    if (!read_u64_file(file, &max)) {
        return 0;
    }
    snprintf(file, sizeof(file), "%s/memory.current", path);
    if (!read_u64_file(file, &current) || current >= max) {
        return 1; /* at or over the limit: report (nearly) nothing left */
    }
    return (size_t)(max - current);
}

size_t host_check_available_memory_bytes(void)
{
    size_t available = read_proc_kb("/proc/meminfo", "MemAvailable:");
    const size_t headroom = cgroup_memory_headroom_bytes();
    if (headroom > 0 && (available == 0 || headroom < available)) {
        available = headroom;
    }
    return available;
}

size_t host_check_resident_bytes(void)
{
    return read_proc_kb("/proc/self/status", "VmRSS:");
}
