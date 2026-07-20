"""Range/shift file replay over the live UDP pipeline.

Uses scenarios/replay_range_shift.yaml: a range-mode capture active for tunes inside
99.9-100.1 MHz and a shift-mode capture pinned to 100 MHz absolute while tuned anywhere
in 80-120 MHz. Noise floor is disabled in the scenario, so "silent" means all-zero payloads.
"""

import socket
import subprocess
import threading
import time
from pathlib import Path

from test_runtime_rest_udp import (
    _free_tcp_port,
    _free_udp_port_block,
    _parse_vita49_packet,
    _recv_if_data,
    _request_json,
    _terminate,
    _wait_json,
    _ci16_payload_has_nonzero,
)

ROOT = Path(__file__).resolve().parents[3]
SIM = ROOT / "build" / "sdr-simulator"
SCENARIO = "simulator/scenarios/replay_range_shift.yaml"


def _write_replay_config(path, rest_port, udp_base):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_replay"
scenario_file: "{SCENARIO}"
log_path: "logs/pytest_replay.log"
stream_block_samples: 256
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 60000000
    frequency_stop_hz: 140000000
    scan_rate_hz_per_s: 100000000000
    profiles:
      - {{ bandwidth_hz: 80000000, sample_rate_hz: 98304000, name: "80M" }}
      - {{ bandwidth_hz: 1000000, sample_rate_hz: 1536000, name: "1M" }}
    channels:
      - {{ channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: 100000000, bandwidth_hz: 1000000, udp_output_port: {udp_base + 1} }}
""",
        encoding="utf-8",
    )


def _start_replay_sim(config):
    """Live wall-clock run (no --scenario-time-ns): the replay position is anchored to
    CLOCK_REALTIME, which is exactly what the cross-instance sync tests exercise."""
    return subprocess.Popen(
        [str(SIM), "--config", str(config), "--scenario", SCENARIO],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def _wait_payload_state(sock, want_energy, deadline_s=5.0):
    """Drain IF packets until the payload energy matches (retunes land on block boundaries
    within the ring depth, so a few stale packets can arrive first)."""
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        packet, vita, _ = _recv_if_data(sock)
        if _ci16_payload_has_nonzero(vita["payload"]) == want_energy:
            return vita
    raise AssertionError(f"payload never reached energy={want_energy}")


def test_replay_modes_follow_channel_retunes(tmp_path):
    rest_port = _free_tcp_port()
    udp_base = _free_udp_port_block()
    config = tmp_path / "replay.yaml"
    _write_replay_config(config, rest_port, udp_base)

    ch1_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ch1_sock.bind(("127.0.0.1", udp_base + 1))
    ch1_sock.settimeout(5.0)

    proc = _start_replay_sim(config)
    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")

        # 100 MHz: both replay signals are in range and centred -> energy.
        _wait_payload_state(ch1_sock, True)

        # 130 MHz: outside the range signal's 99.9-100.1 window and beyond the shift
        # signal's 80-120 range -> silence.
        code, _ = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": 130000000},
            method="PUT",
        )
        assert code == 200
        _wait_payload_state(ch1_sock, False)

        # 100.3 MHz: range signal silent (> 100.1 MHz) but the shift signal stays pinned to
        # 100 MHz absolute, appearing at -300 kHz in the 1 MHz channel -> energy.
        code, _ = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": 100300000},
            method="PUT",
        )
        assert code == 200
        _wait_payload_state(ch1_sock, True)

        # Back to 100 MHz -> energy again.
        code, _ = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": 100000000},
            method="PUT",
        )
        assert code == 200
        _wait_payload_state(ch1_sock, True)
    finally:
        _terminate(proc)
        ch1_sock.close()


def test_replay_instances_started_apart_stay_sample_synchronous(tmp_path):
    """The temp-simulator property: two instances started at different wall-clock times must
    emit identical payloads for identical packet timestamps (position is a pure function of
    epoch time). Sequence numbers differ (per-process), so payloads are matched by timestamp."""
    rest_a = _free_tcp_port()
    rest_b = _free_tcp_port()
    udp_a = _free_udp_port_block()
    udp_b = _free_udp_port_block(start=udp_a + 10)
    config_a = tmp_path / "replay_a.yaml"
    config_b = tmp_path / "replay_b.yaml"
    _write_replay_config(config_a, rest_a, udp_a)
    _write_replay_config(config_b, rest_b, udp_b)

    sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_a.bind(("127.0.0.1", udp_a + 1))
    sock_b.bind(("127.0.0.1", udp_b + 1))
    sock_a.settimeout(5.0)
    sock_b.settimeout(5.0)

    proc_a = _start_replay_sim(config_a)
    procs = [proc_a]
    try:
        _wait_json(f"http://127.0.0.1:{rest_a}/api/v1/health")
        time.sleep(0.5)  # deliberately staggered start
        proc_b = _start_replay_sim(config_b)
        procs.append(proc_b)
        _wait_json(f"http://127.0.0.1:{rest_b}/api/v1/health")

        def flush(sock, duration_s=0.3):
            """Discard the backlog buffered since bind (packets keep arriving at ~6 kpps, so
            drain for a fixed wall time instead of waiting for the socket to go quiet)."""
            deadline = time.time() + duration_s
            sock.settimeout(0.05)
            try:
                while time.time() < deadline:
                    sock.recvfrom(65536)
            except socket.timeout:
                pass
            sock.settimeout(5.0)

        def collect(sock, payloads, count=400):
            for _ in range(count):
                packet, _ = sock.recvfrom(65536)
                vita = _parse_vita49_packet(packet)
                if vita["packet_type"] == 1:
                    payloads[(vita["integer_seconds"], vita["fractional_ps"])] = vita["payload"]

        # The kernel receive buffer keeps only the oldest packets once full, so the two
        # sockets must be flushed and drained concurrently for their windows to overlap.
        flush(sock_a)
        flush(sock_b)
        payloads_a = {}
        payloads_b = {}
        threads = [
            threading.Thread(target=collect, args=(sock_a, payloads_a)),
            threading.Thread(target=collect, args=(sock_b, payloads_b)),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10.0)
        common = set(payloads_a) & set(payloads_b)
        assert len(common) >= 20, f"only {len(common)} overlapping timestamps captured"
        for key in common:
            assert payloads_a[key] == payloads_b[key], f"payload mismatch at timestamp {key}"
        # The content itself must be non-trivial, or the comparison proves nothing.
        assert any(_ci16_payload_has_nonzero(payloads_a[key]) for key in common)
    finally:
        for proc in procs:
            _terminate(proc)
        sock_a.close()
        sock_b.close()


PASSTHROUGH_SCENARIO = "simulator/tests/fixtures/passthrough_no_80m.yaml"


def _write_passthrough_config(path, rest_port, udp_base):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_passthrough"
scenario_file: "{PASSTHROUGH_SCENARIO}"
log_path: "logs/pytest_passthrough.log"
stream_block_samples: 256
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 60000000
    frequency_stop_hz: 140000000
    rf_reference_power_dbm: -55.0
    scan_rate_hz_per_s: 100000000000
    profiles:
      - {{ bandwidth_hz: 80000000, sample_rate_hz: 98304000, name: "80M" }}
      - {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000, name: "20M" }}
      - {{ bandwidth_hz: 1000000, sample_rate_hz: 1536000, name: "1M" }}
    channels:
      - {{ channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: 100000000, bandwidth_hz: 1000000, udp_output_port: {udp_base + 1} }}
""",
        encoding="utf-8",
    )


def _set_bandwidth(rest_port, hz):
    code, _ = _request_json(
        f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
        {"bandwidth_hz": hz},
        method="PUT",
    )
    assert code == 200, f"bandwidth retune to {hz} failed: {code}"


def test_passthrough_selects_variant_by_bandwidth(tmp_path):
    """The passthrough source carries a capture for 1 MHz and 20 MHz but not 80 MHz. Retuning the
    channel bandwidth selects the matching variant (energy) or, where none exists, renders silence
    -- passthrough never resamples."""
    rest_port = _free_tcp_port()
    udp_base = _free_udp_port_block()
    config = tmp_path / "passthrough.yaml"
    _write_passthrough_config(config, rest_port, udp_base)

    ch1_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ch1_sock.bind(("127.0.0.1", udp_base + 1))
    ch1_sock.settimeout(5.0)

    proc = subprocess.Popen(
        [str(SIM), "--config", str(config), "--scenario", PASSTHROUGH_SCENARIO],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")

        # 1 MHz channel: the 1.536 MS/s variant exists -> energy.
        _wait_payload_state(ch1_sock, True)

        # 20 MHz: the 24.576 MS/s variant exists -> energy.
        _set_bandwidth(rest_port, 20000000)
        _wait_payload_state(ch1_sock, True)

        # 80 MHz: no variant for 98.304 MS/s -> silence (no resample).
        _set_bandwidth(rest_port, 80000000)
        _wait_payload_state(ch1_sock, False)

        # Back to 1 MHz -> energy again.
        _set_bandwidth(rest_port, 1000000)
        _wait_payload_state(ch1_sock, True)
    finally:
        _terminate(proc)
        ch1_sock.close()
