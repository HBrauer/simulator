import json
import socket
import subprocess
import threading
import time
from urllib.error import HTTPError
from urllib.request import Request
from pathlib import Path
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[3]
SIM = ROOT / "build" / "sdr-simulator"
_PORT_BLOCK_CURSOR = 30000
_DEFAULT_RECEIVER_SAMPLE_RATE_HZ = 98304000


def _grid_time_ns(scenario_time_ns, block_samples, sample_rate_hz=_DEFAULT_RECEIVER_SAMPLE_RATE_HZ):
    """Render blocks are anchored to a fixed grid, so a packet carries the grid block that
    contains the requested time, not the exact requested time. Mirror the C 128-bit math."""
    sample_index = (scenario_time_ns * sample_rate_hz) // 1_000_000_000
    block_index = sample_index // block_samples
    return (block_index * block_samples * 1_000_000_000) // sample_rate_hz


def _free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _free_udp_port_block(count=5, start=30000):
    global _PORT_BLOCK_CURSOR
    search_start = max(start, _PORT_BLOCK_CURSOR)
    for base in range(search_start, 60000 - count):
        sockets = []
        try:
            for port in range(base, base + count):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(("127.0.0.1", port))
                sockets.append(sock)
            _PORT_BLOCK_CURSOR = base + count + 10
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("could not find a free UDP port block")


def _write_config(path, rest_port, udp_base, ch1_center=10005000000, stream_block_samples=None):
    stream_block_samples_line = ""
    if stream_block_samples is not None:
        stream_block_samples_line = f"stream_block_samples: {stream_block_samples}\n"
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_runtime"
scenario_file: "simulator/scenarios/scanner_fsk.json"
log_path: "logs/pytest_runtime.log"
{stream_block_samples_line}\
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 9960000000
    frequency_stop_hz: 10040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: {ch1_center}, bandwidth_hz: 20000000, udp_output_port: {udp_base + 1} }}
      - {{ channel_id: 2, center_frequency_hz: 10010000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 2} }}
      - {{ channel_id: 3, center_frequency_hz: 9995000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 3} }}
      - {{ channel_id: 4, center_frequency_hz: 10030000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 4} }}
""",
        encoding="utf-8",
    )


def _write_two_receiver_config(path, rest_port_0, rest_port_1, udp_base):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_two_receivers"
scenario_file: "simulator/scenarios/scanner_fsk.json"
log_path: "logs/pytest_two_receivers.log"
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port_0}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 9960000000
    frequency_stop_hz: 10040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: 10005000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 1} }}
      - {{ channel_id: 2, center_frequency_hz: 10010000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 2} }}
      - {{ channel_id: 3, center_frequency_hz: 9995000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 3} }}
      - {{ channel_id: 4, center_frequency_hz: 10030000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 4} }}
  - receiver_id: 1
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port_1}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 19960000000
    frequency_stop_hz: 20040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: {udp_base + 5} }}
      - {{ channel_id: 1, center_frequency_hz: 20000000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 6} }}
      - {{ channel_id: 2, center_frequency_hz: 20010000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 7} }}
      - {{ channel_id: 3, center_frequency_hz: 19990000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 8} }}
      - {{ channel_id: 4, center_frequency_hz: 20030000000, bandwidth_hz: 20000000, udp_output_port: {udp_base + 9} }}
""",
        encoding="utf-8",
    )


def _wait_json(url):
    deadline = time.time() + 5.0
    last_error = None
    while time.time() < deadline:
        try:
            with urlopen(url, timeout=0.5) as response:
                return json.loads(response.read().decode("utf-8"))
        except Exception as exc:  # pragma: no cover - diagnostic path
            last_error = exc
            time.sleep(0.05)
    raise AssertionError(f"endpoint did not become ready: {url}: {last_error}")


def _request_json(url, payload, method="POST"):
    data = json.dumps(payload).encode("utf-8")
    request = Request(url, data=data, method=method, headers={"Content-Type": "application/json"})
    with urlopen(request, timeout=2.0) as response:
        return response.status, json.loads(response.read().decode("utf-8"))


def _request_json_error(url, payload, method="POST"):
    try:
        _request_json(url, payload, method=method)
    except HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))
    raise AssertionError("request unexpectedly succeeded")


def _method_error(url, method):
    request = Request(url, method=method)
    try:
        urlopen(request, timeout=2.0)
    except HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))
    raise AssertionError("request unexpectedly succeeded")


def _assert_keys(obj, keys):
    missing = set(keys) - set(obj)
    assert not missing, f"missing keys: {sorted(missing)}"


def _assert_receiver_status_shape(status):
    _assert_keys(
        status,
        {
            "receiver_id",
            "effective_mode",
            "frequency_start_hz",
            "frequency_stop_hz",
            "center_frequency_hz",
            "frontend_bandwidth_hz",
            "scan_rate_hz_per_s",
            "output_scale",
            "rf_reference_power_dbm",
            "udp_output_host",
            "udp_multicast_interface",
            "channel_count",
            "config_epoch",
        },
    )


def _assert_channel_shape(channel):
    _assert_keys(
        channel,
        {
            "channel_id",
            "track_tuner",
            "center_frequency_hz",
            "configured_center_frequency_hz",
            "bandwidth_hz",
            "sample_rate_hz",
            "profile",
            "in_frontend_window",
            "stream_enabled",
            "active",
            "udp_port",
            "output_scale",
            "rf_reference_power_dbm",
        },
    )


def _assert_metrics_shape(metrics):
    _assert_keys(
        metrics,
        {
            "samples_rendered",
            "samples_sent",
            "actual_sample_rate_sps",
            "samples_missed",
            "samples_late",
            "samples_send_dropped",
            "udp_packets_sent",
            "udp_bytes_sent",
            "udp_send_errors",
            "udp_send_would_block",
            "udp_send_no_buffer",
            "udp_send_other_errors",
            "active_streams",
            "ringbuffer_overruns",
            "ringbuffer_underruns",
            "samples_dropped",
            "worker_errors",
            "streams",
        },
    )
    assert len(metrics["streams"]) == 5
    for stream in metrics["streams"]:
        _assert_keys(
            stream,
            {
                "stream_type",
                "stream_id",
                "sample_rate_hz",
                "active",
                "samples_rendered",
                "samples_sent",
                "actual_sample_rate_sps",
                "samples_missed",
                "samples_late",
                "samples_send_dropped",
                "udp_packets_sent",
                "udp_bytes_sent",
                "udp_send_errors",
                "udp_send_would_block",
                "udp_send_no_buffer",
                "udp_send_other_errors",
                "ringbuffer_overruns",
                "ringbuffer_underruns",
                "samples_dropped",
                "worker_errors",
            },
        )
        assert stream["stream_type"] == "channel"


def _assert_stream_status_shape(streams):
    _assert_keys(streams, {"receiver_id", "streams"})
    assert len(streams["streams"]) == 5
    for stream in streams["streams"]:
        _assert_keys(
            stream,
            {
                "stream_type",
                "stream_id",
                "udp_port",
                "sample_rate_hz",
                "bandwidth_hz",
                "center_frequency_hz",
                "track_tuner",
                "output_scale",
                "rf_reference_power_dbm",
                "in_frontend_window",
                "enabled",
                "active",
                "samples_rendered",
                "samples_sent",
                "samples_missed",
                "samples_late",
                "samples_send_dropped",
                "udp_packets_sent",
                "udp_send_would_block",
                "udp_send_no_buffer",
                "udp_send_other_errors",
                "ringbuffer_overruns",
                "ringbuffer_underruns",
            },
        )
        assert stream["stream_type"] == "channel"


def _wait_stream_state(rest_port, stream_id, enabled):
    for _ in range(50):
        streams = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/streams")
        for stream in streams["streams"]:
            if stream["stream_id"] == stream_id and stream["enabled"] is enabled and stream["active"] is enabled:
                return stream
        time.sleep(0.02)
    raise AssertionError(f"channel {stream_id} did not reach enabled={enabled}")


def _parse_vita49_packet(packet):
    assert len(packet) >= 20
    header = int.from_bytes(packet[0:4], "big")
    packet_words = header & 0xFFFF
    assert packet_words * 4 == len(packet)
    return {
        "packet_type": header >> 28,
        "tsi": (header >> 22) & 0x3,
        "tsf": (header >> 20) & 0x3,
        "sequence": (header >> 16) & 0xF,
        "packet_words": packet_words,
        "stream_id": int.from_bytes(packet[4:8], "big"),
        "integer_seconds": int.from_bytes(packet[8:12], "big"),
        "fractional_ps": int.from_bytes(packet[12:20], "big"),
        "payload": packet[20:],
    }


def _parse_vita49_context(packet):
    """IF context packet: CIF0 word then bandwidth, RF reference, sample rate as 64-bit
    fixed point (radix point after bit 20)."""
    vita = _parse_vita49_packet(packet)
    assert vita["packet_type"] == 4
    cif0 = int.from_bytes(packet[20:24], "big")
    vita["cif0_changed"] = bool(cif0 >> 31)
    vita["bandwidth_hz"] = int.from_bytes(packet[24:32], "big") >> 20
    vita["rf_reference_frequency_hz"] = int.from_bytes(packet[32:40], "big") >> 20
    vita["sample_rate_hz"] = int.from_bytes(packet[40:48], "big") >> 20
    return vita


def _recv_vita49(sock, packet_type):
    """Receive until a packet of the requested VITA type arrives (context packets are
    interleaved with IF data on every stream)."""
    deadline = time.time() + 5.0
    while time.time() < deadline:
        packet, addr = sock.recvfrom(65536)
        vita = _parse_vita49_packet(packet)
        if vita["packet_type"] == packet_type:
            return packet, vita, addr
    raise AssertionError(f"no VITA packet of type {packet_type} received")


def _recv_if_data(sock):
    return _recv_vita49(sock, 1)


def _ci16_payload_has_nonzero(payload):
    assert len(payload) % 4 == 0
    for offset in range(0, len(payload), 2):
        if int.from_bytes(payload[offset:offset + 2], "little", signed=True) != 0:
            return True
    return False


def _terminate(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5.0)
    stderr = proc.stderr.read() if proc.stderr else ""
    assert proc.returncode in (0, -15), stderr


def _start_sim(config, extra_args=()):
    return subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/scanner_fsk.json",
            "--scenario-time-ns",
            "450000",
            *extra_args,
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def test_runtime_rest_and_udp_stream(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime.yaml"
    _write_config(config, rest_port, udp_port)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = _start_sim(config, ("--stream-block-samples", "256"))
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/status")
        _assert_receiver_status_shape(status)
        assert status["receiver_id"] == 0
        assert status["center_frequency_hz"] == 10000000000
        assert status["channel_count"] == 5
        scenario = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/scenario/status")
        _assert_keys(scenario, {"scenario_id", "loaded", "scenario_time_ns", "source_count", "signal_count"})
        assert scenario["scenario_time_ns"] == 450000

        packet, vita, addr = _recv_if_data(udp_sock)
        assert addr[0] == "127.0.0.1"
        assert vita["stream_id"] == 0  # count-up default: channel 0
        assert len(vita["payload"]) == 256 * 4
        assert _ci16_payload_has_nonzero(vita["payload"])
        metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")
        _assert_metrics_shape(metrics)
        assert metrics["active_streams"] == 5
        assert metrics["samples_rendered"] >= 256
        assert metrics["samples_sent"] >= 256
        assert metrics["samples_sent"] == metrics["samples_rendered"]
        assert metrics["actual_sample_rate_sps"] >= 0
        assert metrics["udp_packets_sent"] >= 1
        assert metrics["udp_bytes_sent"] >= 20 + 256 * 4
        assert metrics["udp_send_errors"] == 0
        assert {stream["stream_id"] for stream in metrics["streams"]} == {0, 1, 2, 3, 4}
        assert metrics["streams"][0]["sample_rate_hz"] == 98304000
        assert metrics["streams"][1]["sample_rate_hz"] == 24576000
        assert metrics["streams"][0]["active"] is True
        assert metrics["streams"][0]["samples_sent"] == metrics["streams"][0]["samples_rendered"]
        streams = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/streams")
        _assert_stream_status_shape(streams)
        assert streams["receiver_id"] == 0
        assert streams["streams"][0]["udp_port"] == udp_port
        assert streams["streams"][0]["track_tuner"] is True
        assert streams["streams"][0]["enabled"] is True
        assert streams["streams"][0]["active"] is True
        assert all(stream["in_frontend_window"] for stream in streams["streams"])
        assert all(stream["enabled"] for stream in streams["streams"])

        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/0/stream",
            {"enabled": False},
        )
        assert code == 200
        assert stream_update["enabled"] is False
        _wait_stream_state(rest_port, 0, False)
        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/0/stream",
            {"enabled": True},
        )
        assert code == 200
        assert stream_update["enabled"] is True
        _wait_stream_state(rest_port, 0, True)

        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1/stream",
            {"enabled": False},
        )
        assert code == 200
        assert stream_update["stream_type"] == "channel"
        assert stream_update["stream_id"] == 1
        assert stream_update["enabled"] is False
        _wait_stream_state(rest_port, 1, False)
        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1/stream",
            {"enabled": True},
        )
        assert code == 200
        assert stream_update["enabled"] is True
        _wait_stream_state(rest_port, 1, True)

        expected = subprocess.check_output(
            [
                str(SIM),
                "--config",
                str(config),
                "--scenario",
                "simulator/scenarios/scanner_fsk.json",
                "--scenario-time-ns",
                str(_grid_time_ns(450000, 256)),
                "--render-once-samples",
                "256",
            ],
            cwd=ROOT,
        )
        assert vita["payload"] == expected

        code, error = _method_error(f"http://127.0.0.1:{rest_port}/api/v1/does-not-exist", "GET")
        assert code == 404
        assert error["error"]["code"] == "not_found"
        code, error = _method_error(f"http://127.0.0.1:{rest_port}/api/v1/status", "POST")
        assert code == 404
        assert error["error"]["code"] == "not_found"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/output-scale",
            {"output_scale": 0},
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_output_scale"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/output-scale",
            {"output_scale": 0.5, "padding": "x" * 5000},
        )
        assert code == 413
        assert error["error"]["code"] == "request_too_large"

        code, scaled = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/output-scale",
            {"output_scale": 0.5},
        )
        assert code == 200
        assert scaled["output_scale"] == 0.5
        assert all(channel["output_scale"] == 0.5 for channel in scaled["channels"])

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/frequency-range",
            {"frequency_start_hz": 1000},
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_request"

        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/frequency-range",
            {
                "frequency_start_hz": 9960000000,
                "frequency_stop_hz": 10060000000,
                "scan_rate_hz_per_s": 100000000000,
            },
        )
        assert code == 200
        assert updated["effective_mode"] == "scan"
        assert updated["center_frequency_hz"] == 10005000000
    finally:
        _terminate(proc)
        udp_sock.close()


def test_capabilities_and_channel_configuration(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_channels.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=256)

    proc = _start_sim(config)
    try:
        capabilities = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/capabilities")
        _assert_keys(
            capabilities,
            {
                "receiver_id",
                "frequency_min_hz",
                "frequency_max_hz",
                "frontend_bandwidth_hz",
                "tuner",
                "channel_count",
                "profiles",
                "iq_format",
                "udp_output_host",
                "config_epoch",
            },
        )
        assert capabilities["receiver_id"] == 0
        assert capabilities["frequency_max_hz"] == 40000000000
        assert capabilities["frontend_bandwidth_hz"] == 80000000
        assert capabilities["channel_count"] == 5
        assert capabilities["iq_format"] == "vita49_2_ci16"
        assert capabilities["tuner"]["mode"] == "fixed"
        profile_bandwidths = {profile["bandwidth_hz"] for profile in capabilities["profiles"]}
        assert {80000000, 20000000, 5000000} <= profile_bandwidths
        for profile in capabilities["profiles"]:
            assert profile["sample_rate_hz"] >= profile["bandwidth_hz"]
        epoch_0 = capabilities["config_epoch"]

        channels = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/channels")
        assert len(channels) == 5
        for channel in channels:
            _assert_channel_shape(channel)
        assert channels[0]["track_tuner"] is True
        assert channels[0]["bandwidth_hz"] == 80000000
        assert channels[0]["udp_port"] == udp_port
        assert channels[1]["center_frequency_hz"] == 10005000000

        channel_1 = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/channels/1")
        _assert_channel_shape(channel_1)
        assert channel_1["channel_id"] == 1

        # Retune + narrow the channel; the sample rate must follow the profile.
        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": 10012000000, "bandwidth_hz": 5000000},
            method="PUT",
        )
        assert code == 200
        _assert_channel_shape(updated)
        assert updated["center_frequency_hz"] == 10012000000
        assert updated["bandwidth_hz"] == 5000000
        assert updated["sample_rate_hz"] == 6144000
        assert updated["in_frontend_window"] is True

        capabilities = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/capabilities")
        assert capabilities["config_epoch"] > epoch_0

        # Error paths.
        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/99",
            {"center_frequency_hz": 10005000000},
            method="PUT",
        )
        assert code == 404
        assert error["error"]["code"] == "invalid_channel_id"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": "bad"},
            method="PUT",
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_request"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"output_scale": -1.0},
            method="PUT",
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_output_scale"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"bandwidth_hz": 12345},
            method="PUT",
        )
        assert code == 400
        assert error["error"]["code"] == "unsupported_bandwidth"
        assert "80000000" in error["error"]["message"]

        # A tuner-tracking channel rejects a direct retune.
        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/0",
            {"center_frequency_hz": 10005000000},
            method="PUT",
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_request"

        # Unless it is detached from the tuner in the same request.
        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/0",
            {"track_tuner": False, "center_frequency_hz": 10001000000, "bandwidth_hz": 20000000},
            method="PUT",
        )
        assert code == 200
        assert updated["track_tuner"] is False
        assert updated["center_frequency_hz"] == 10001000000

        # Retune outside the front-end window: accepted, but flagged out-of-window.
        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": 10060000000},
            method="PUT",
        )
        assert code == 200
        assert updated["in_frontend_window"] is False
    finally:
        _terminate(proc)


def test_channel_bandwidth_change_switches_stream_rate(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_rate_change.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=256)

    ch1_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ch1_sock.bind(("127.0.0.1", udp_port + 1))
    ch1_sock.settimeout(5.0)

    proc = _start_sim(config)
    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        _recv_if_data(ch1_sock)  # channel is streaming at 24.576 MS/s

        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"bandwidth_hz": 5000000},
            method="PUT",
        )
        assert code == 200
        assert updated["sample_rate_hz"] == 6144000

        # The stream announces the new configuration in-band via a context packet.
        deadline = time.time() + 5.0
        while True:
            assert time.time() < deadline, "no context packet with the new rate"
            packet, vita, _ = _recv_vita49(ch1_sock, 4)
            context = _parse_vita49_context(packet)
            if context["sample_rate_hz"] == 6144000:
                assert context["bandwidth_hz"] == 5000000
                break

        # The REST metrics follow, and the paced rate drops measurably.
        metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")
        assert metrics["streams"][1]["sample_rate_hz"] == 6144000
        start = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")["streams"][1]["samples_sent"]
        time.sleep(1.5)
        end = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")["streams"][1]["samples_sent"]
        measured = (end - start) / 1.5
        assert 3000000 < measured < 12000000, f"measured {measured} sps after switch to 6.144 MS/s"

        metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")
        assert metrics["streams"][1]["worker_errors"] == 0
    finally:
        _terminate(proc)
        ch1_sock.close()


def test_context_packets_announce_stream_configuration(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_context.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=256)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = _start_sim(config)
    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        packet, vita, _ = _recv_vita49(udp_sock, 4)
        context = _parse_vita49_context(packet)
        assert context["stream_id"] == 0  # count-up default: channel 0
        assert context["bandwidth_hz"] == 80000000
        assert context["sample_rate_hz"] == 98304000
        assert context["rf_reference_frequency_hz"] == 10000000000
    finally:
        _terminate(proc)
        udp_sock.close()


def test_vita49_udp_wraps_and_keeps_raw_payload(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_framed.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=128)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = _start_sim(config)
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        packet, vita, addr = _recv_if_data(udp_sock)
        assert addr[0] == "127.0.0.1"
        assert vita["tsi"] == 1
        assert vita["tsf"] == 2
        assert vita["sequence"] == 0
        assert vita["stream_id"] == 0  # count-up default: channel 0
        assert vita["integer_seconds"] == 0
        # The block grid quantises the render time to the block that contains 450000 ns.
        grid_ns = _grid_time_ns(450000, 128)
        assert vita["fractional_ps"] == grid_ns * 1000
        payload = vita["payload"]
        assert len(payload) == 128 * 4
        expected = subprocess.check_output(
            [
                str(SIM),
                "--config",
                str(config),
                "--scenario",
                "simulator/scenarios/scanner_fsk.json",
                "--scenario-time-ns",
                str(grid_ns),
                "--render-once-samples",
                "128",
            ],
            cwd=ROOT,
        )
        assert payload == expected
    finally:
        _terminate(proc)
        udp_sock.close()


def test_stream_block_samples_config_controls_udp_packet_size(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_packet_size.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=128)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = _start_sim(config)
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        packet, vita, addr = _recv_if_data(udp_sock)
        assert addr[0] == "127.0.0.1"
        assert len(vita["payload"]) == 128 * 4
    finally:
        _terminate(proc)
        udp_sock.close()


def test_channel_4096_sample_vita49_packet_contains_signal(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_channel_4096_signal.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=4096)

    ch1_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ch1_sock.bind(("127.0.0.1", udp_port + 1))
    ch1_sock.settimeout(5.0)

    proc = _start_sim(config)
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        packet, vita, addr = _recv_if_data(ch1_sock)
        assert addr[0] == "127.0.0.1"
        assert vita["stream_id"] == 1  # count-up default: channel 1
        assert len(vita["payload"]) == 4096 * 4
        assert _ci16_payload_has_nonzero(vita["payload"])
    finally:
        _terminate(proc)
        ch1_sock.close()


def test_concurrent_rest_updates_and_status_reads(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_concurrent.yaml"
    _write_config(config, rest_port, udp_port)

    proc = _start_sim(config, ("--stream-block-samples", "128"))
    errors = []

    def update_output_scale():
        try:
            for index in range(20):
                scale = 0.25 + (index % 4) * 0.25
                code, body = _request_json(
                    f"http://127.0.0.1:{rest_port}/api/v1/output-scale",
                    {"output_scale": scale},
                )
                assert code == 200
                assert body["output_scale"] == scale
        except Exception as exc:  # pragma: no cover - diagnostic path
            errors.append(exc)

    def update_channel():
        try:
            for index in range(20):
                code, body = _request_json(
                    f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
                    {
                        "center_frequency_hz": 10005000000 + index,
                        "output_scale": 0.5,
                    },
                    method="PUT",
                )
                assert code == 200
                assert body["channel_id"] == 1
        except Exception as exc:  # pragma: no cover - diagnostic path
            errors.append(exc)

    def read_status():
        try:
            for _ in range(40):
                status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/status")
                assert status["receiver_id"] == 0
                streams = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/streams")
                assert len(streams["streams"]) == 5
        except Exception as exc:  # pragma: no cover - diagnostic path
            errors.append(exc)

    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        threads = [
            threading.Thread(target=update_output_scale),
            threading.Thread(target=update_channel),
            threading.Thread(target=read_status),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10.0)
        assert not errors
        assert all(not thread.is_alive() for thread in threads)
    finally:
        _terminate(proc)


def test_channel_udp_stream_is_empty_when_outside_frontend_window(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_channel_outside.yaml"
    _write_config(config, rest_port, udp_port, ch1_center=10060000000)

    ch1_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ch1_sock.bind(("127.0.0.1", udp_port + 1))
    ch1_sock.settimeout(5.0)

    proc = _start_sim(config, ("--stream-block-samples", "256"))
    try:
        channel = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/channels/1")
        assert channel["in_frontend_window"] is False
        packet, vita, addr = _recv_if_data(ch1_sock)
        assert addr[0] == "127.0.0.1"
        assert len(vita["payload"]) == 256 * 4
        assert all(byte == 0 for byte in vita["payload"])
    finally:
        _terminate(proc)
        ch1_sock.close()


def test_two_receivers_expose_independent_rest_apis(tmp_path):
    rest_port_0 = _free_tcp_port()
    rest_port_1 = _free_tcp_port()
    udp_base = _free_udp_port_block(count=10)
    config = tmp_path / "runtime_two_receivers.yaml"
    _write_two_receiver_config(config, rest_port_0, rest_port_1, udp_base)

    proc = _start_sim(config, ("--stream-block-samples", "128"))
    try:
        status_0 = _wait_json(f"http://127.0.0.1:{rest_port_0}/api/v1/status")
        status_1 = _wait_json(f"http://127.0.0.1:{rest_port_1}/api/v1/status")
        assert status_0["receiver_id"] == 0
        assert status_1["receiver_id"] == 1
        assert status_0["center_frequency_hz"] == 10000000000
        assert status_1["center_frequency_hz"] == 20000000000
    finally:
        _terminate(proc)


def test_two_instances_emit_identical_udp_for_same_scenario_time(tmp_path):
    rest_port_1 = _free_tcp_port()
    rest_port_2 = _free_tcp_port()
    udp_base_1 = _free_udp_port_block()
    udp_base_2 = _free_udp_port_block(start=udp_base_1 + 10)
    config_1 = tmp_path / "instance_1.yaml"
    config_2 = tmp_path / "instance_2.yaml"
    _write_config(config_1, rest_port_1, udp_base_1)
    _write_config(config_2, rest_port_2, udp_base_2)

    sock_1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_1.bind(("127.0.0.1", udp_base_1))
    sock_2.bind(("127.0.0.1", udp_base_2))
    sock_1.settimeout(5.0)
    sock_2.settimeout(5.0)

    procs = [
        _start_sim(config_1, ("--stream-block-samples", "256")),
        _start_sim(config_2, ("--stream-block-samples", "256")),
    ]
    try:
        status_1 = _wait_json(f"http://127.0.0.1:{rest_port_1}/api/v1/status")
        status_2 = _wait_json(f"http://127.0.0.1:{rest_port_2}/api/v1/status")
        assert status_1["receiver_id"] == status_2["receiver_id"] == 0
        packet_1, vita_1, _ = _recv_if_data(sock_1)
        packet_2, vita_2, _ = _recv_if_data(sock_2)
        assert len(vita_1["payload"]) == len(vita_2["payload"]) == 256 * 4
        assert packet_1 == packet_2
    finally:
        for proc in procs:
            _terminate(proc)
        sock_1.close()
        sock_2.close()


def test_runtime_soak_keeps_streaming_without_send_errors(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_soak.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=512)

    proc = _start_sim(config)
    try:
        start_metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")
        time.sleep(2.0)
        end_metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")

        _assert_metrics_shape(end_metrics)
        assert end_metrics["active_streams"] == 5
        assert end_metrics["samples_rendered"] > start_metrics["samples_rendered"]
        assert end_metrics["udp_packets_sent"] > start_metrics["udp_packets_sent"]
        assert end_metrics["udp_bytes_sent"] > start_metrics["udp_bytes_sent"]
        assert end_metrics["udp_send_errors"] == 0
        assert all(stream["active"] is True for stream in end_metrics["streams"])
    finally:
        _terminate(proc)
