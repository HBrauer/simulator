#ifndef HOST_CHECK_H
#define HOST_CHECK_H

#include "sim_types.h"

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Startup checks that the host can carry the configured streams. Errors are configurations the
 * host cannot honour at all (startup fails); warnings degrade the output (drops, fragmentation,
 * unsynchronised timestamps) and fail startup only with --strict-host-checks. Each finding is
 * one line on `out` that names the fix. */
typedef struct {
    size_t errors;
    size_t warnings;
} host_check_result_t;

/* Approximate kernel accounting overhead per queued datagram (skb + headers), used to turn a
 * datagram size into the send-buffer bytes it occupies. */
#define HOST_CHECK_SKB_OVERHEAD_BYTES 1024U
/* Warn when the asset cache's resident data takes more than this share of the memory that was
 * available before it loaded. */
#define HOST_CHECK_MEMORY_WARN_FRACTION 0.75

/* Largest IF-data datagram (UDP payload) any channel of `receiver` can emit: the block size at
 * its highest listed rate, its output format, the optional Class ID and VRL framing. */
size_t host_check_max_datagram_bytes(const simulator_config_t *config, const receiver_config_t *receiver, size_t block_samples);

/* Individual checks over already-gathered host facts (kept pure for testing). */
void host_check_stream_cpus(const simulator_config_t *config, const cpu_set_t *allowed, FILE *out, host_check_result_t *result);
void host_check_thread_count(const simulator_config_t *config, const cpu_set_t *allowed, FILE *out, host_check_result_t *result);
void host_check_sndbuf(const simulator_config_t *config, size_t block_samples, int reported_sndbuf, FILE *out, host_check_result_t *result);
void host_check_datagram_mtu(const simulator_config_t *config, const receiver_config_t *receiver, size_t block_samples, int path_mtu, FILE *out, host_check_result_t *result);
void host_check_clock(bool synchronised, FILE *out, host_check_result_t *result);
void host_check_memory(size_t loaded_bytes, size_t available_bytes, FILE *out, host_check_result_t *result);

/* Gather the host facts and run every pre-load check. check_clock is false when the run uses a
 * fixed scenario clock (--scenario-time-ns), where wall-clock sync is irrelevant. */
void host_check_startup(const simulator_config_t *config, size_t block_samples, bool check_clock, FILE *out, host_check_result_t *result);

/* Memory the process may still allocate: MemAvailable, lowered to the cgroup v2 memory.max
 * headroom when that is tighter. 0 if unknown. */
size_t host_check_available_memory_bytes(void);
/* Resident set size of this process in bytes (0 if unknown). */
size_t host_check_resident_bytes(void);

#endif
