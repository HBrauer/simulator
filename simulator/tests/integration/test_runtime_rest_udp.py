import json
import socket
import subprocess
import threading
import time
from urllib.error import HTTPError
from urllib.request import Request
from pathlib import Path
from urllib.request import urlopen

import pytest


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
scenario_file: "simulator/scenarios/scanner_fsk.yaml"
log_path: "logs/pytest_runtime.log"
{stream_block_samples_line}\
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_min_hz: 9960000000
    frequency_max_hz: 10040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, track_tuner: true, rates: [ {{ bandwidth_hz: 80000000, sample_rate_hz: 98304000 }} ], udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: {ch1_center}, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }}, {{ bandwidth_hz: 5000000, sample_rate_hz: 6144000 }} ], udp_output_port: {udp_base + 1} }}
      - {{ channel_id: 2, center_frequency_hz: 10010000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 2} }}
      - {{ channel_id: 3, center_frequency_hz: 9995000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 3} }}
      - {{ channel_id: 4, center_frequency_hz: 10030000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 4} }}
""",
        encoding="utf-8",
    )


def _write_two_receiver_config(path, rest_port_0, rest_port_1, udp_base):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_two_receivers"
scenario_file: "simulator/scenarios/scanner_fsk.yaml"
log_path: "logs/pytest_two_receivers.log"
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port_0}
    udp_output_host: "127.0.0.1"
    frequency_min_hz: 9960000000
    frequency_max_hz: 10040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, track_tuner: true, rates: [ {{ bandwidth_hz: 80000000, sample_rate_hz: 98304000 }} ], udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: 10005000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 1} }}
      - {{ channel_id: 2, center_frequency_hz: 10010000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 2} }}
      - {{ channel_id: 3, center_frequency_hz: 9995000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 3} }}
      - {{ channel_id: 4, center_frequency_hz: 10030000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 4} }}
  - receiver_id: 1
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port_1}
    udp_output_host: "127.0.0.1"
    frequency_min_hz: 19960000000
    frequency_max_hz: 20040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, track_tuner: true, rates: [ {{ bandwidth_hz: 80000000, sample_rate_hz: 98304000 }} ], udp_output_port: {udp_base + 5} }}
      - {{ channel_id: 1, center_frequency_hz: 20000000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 6} }}
      - {{ channel_id: 2, center_frequency_hz: 20010000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 7} }}
      - {{ channel_id: 3, center_frequency_hz: 19990000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 8} }}
      - {{ channel_id: 4, center_frequency_hz: 20030000000, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], udp_output_port: {udp_base + 9} }}
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
            "frequency_min_hz",
            "frequency_max_hz",
            "center_frequency_hz",
            "bandwidth_hz",
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
            "rates",
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
    """IF context packet fields in descending CIF0 bit order: bandwidth (bit 29, 64-bit),
    RF reference frequency (27, 64-bit), reference level (24, a single 32-bit word), sample
    rate (21, 64-bit), then the Data Packet Payload Format field (15, two words). Frequency/rate
    fields are 64-bit fixed point with the radix point after bit 20."""
    vita = _parse_vita49_packet(packet)
    assert vita["packet_type"] == 4
    cif0 = int.from_bytes(packet[20:24], "big")
    vita["cif0_changed"] = bool(cif0 >> 31)
    vita["bandwidth_hz"] = int.from_bytes(packet[24:32], "big") >> 20
    vita["rf_reference_frequency_hz"] = int.from_bytes(packet[32:40], "big") >> 20
    vita["reference_level_dbm"] = (
        int.from_bytes(packet[42:44], "big", signed=True) / 128.0
    )
    vita["sample_rate_hz"] = int.from_bytes(packet[44:52], "big") >> 20
    vita["payload_format_word0"] = int.from_bytes(packet[52:56], "big")
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
            "simulator/scenarios/scanner_fsk.yaml",
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
                "simulator/scenarios/scanner_fsk.yaml",
                "--scenario-time-ns",
                str(_grid_time_ns(450000, 256)),
                "--render-once-samples",
                "256",
            ],
            cwd=ROOT,
        )
        # UDP payload is big-endian VITA 49.2; render-once dumps native little-endian CI16.
        expected_wire = bytes(
            b for k in range(0, len(expected), 2) for b in (expected[k + 1], expected[k])
        )
        assert vita["payload"] == expected_wire

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
            {"frequency_min_hz": 1000},
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_request"

        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/frequency-range",
            {
                "frequency_min_hz": 9960000000,
                "frequency_max_hz": 10060000000,
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
                "simulator_frequency_max_hz",
                "bandwidth_hz",
                "tuner",
                "channel_count",
                "iq_format",
                "udp_output_host",
                "config_epoch",
            },
        )
        assert capabilities["receiver_id"] == 0
        assert capabilities["simulator_frequency_max_hz"] == 100000000000
        assert capabilities["bandwidth_hz"] == 80000000
        assert capabilities["channel_count"] == 5
        assert capabilities["iq_format"] == "vita49_2_ci16"
        assert capabilities["tuner"]["mode"] == "fixed"
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

        # Retune + narrow the channel; bandwidth and sample rate are set together.
        code, updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"center_frequency_hz": 10012000000, "bandwidth_hz": 5000000, "sample_rate_hz": 6144000},
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
            {"bandwidth_hz": 20000000, "sample_rate_hz": 1000},
            method="PUT",
        )
        assert code == 400
        assert error["error"]["code"] == "unsupported_channel_rate"

        # bandwidth_hz and sample_rate_hz must be set together.
        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/1",
            {"bandwidth_hz": 5000000},
            method="PUT",
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_request"

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
            {"track_tuner": False, "center_frequency_hz": 10001000000},
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
            {"bandwidth_hz": 5000000, "sample_rate_hz": 6144000},
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
                "simulator/scenarios/scanner_fsk.yaml",
                "--scenario-time-ns",
                str(grid_ns),
                "--render-once-samples",
                "128",
            ],
            cwd=ROOT,
        )
        # The UDP payload is big-endian per VITA 49.2; --render-once-samples dumps native
        # little-endian CI16, so byteswap each 16-bit component before comparing.
        expected_wire = bytes(
            b for k in range(0, len(expected), 2) for b in (expected[k + 1], expected[k])
        )
        assert payload == expected_wire
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


def _write_output_format_config(path, rest_port, udp_base):
    """Three fixed DDC channels at the same center and rate but different on-wire sample
    formats, so their payloads decode to identical I/Q and can be cross-checked."""
    center = 10005000000
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_formats"
scenario_file: "simulator/scenarios/scanner_fsk.yaml"
log_path: "logs/pytest_formats.log"
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_min_hz: 9960000000
    frequency_max_hz: 10040000000
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, center_frequency_hz: {center}, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], output_format: ci16, udp_output_port: {udp_base} }}
      - {{ channel_id: 1, center_frequency_hz: {center}, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], output_format: cf32, udp_output_port: {udp_base + 1} }}
      - {{ channel_id: 2, center_frequency_hz: {center}, rates: [ {{ bandwidth_hz: 20000000, sample_rate_hz: 24576000 }} ], output_format: ci24, udp_output_port: {udp_base + 2} }}
""",
        encoding="utf-8",
    )


def test_output_formats_decode_with_vita49io(tmp_path):
    """Each configured output format (CI16, CF32, CI24) produces valid VITA 49.2 packets whose
    context field self-describes the payload and whose samples decode via the vita49io reference
    library to the same I/Q the CI16 channel carries. CI24 uses spec-strict left-justified data
    items (VITA 49.2 6.1.1.1-2), decoded correctly by vita49io >= 0.1.8."""
    pytest.importorskip("vita49io")
    from importlib.metadata import version as _pkg_version

    try:
        from packaging.version import Version

        if Version(_pkg_version("vita49io")) < Version("0.1.8"):
            pytest.skip("vita49io < 0.1.8 decodes left-justified sub-word items incorrectly")
    except ImportError:  # pragma: no cover - packaging usually present with pytest
        pass
    from vita49io import ContextPacket, DataPacket
    from vita49io.io.payload_codec import payload_as_numpy
    from vita49io.protocol.cif0 import DataItemFormat, PayloadFormat, SampleType
    import numpy as np

    rest_port = _free_tcp_port()
    udp_base = _free_udp_port_block()
    config = tmp_path / "runtime_formats.yaml"
    _write_output_format_config(config, rest_port, udp_base)

    socks = {}
    for name, offset in (("ci16", 0), ("cf32", 1), ("ci24", 2)):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", udp_base + offset))
        sock.settimeout(5.0)
        socks[name] = sock

    # Expected Data Packet Payload Format subfields per channel format.
    expected_pf = {
        "ci16": (SampleType.COMPLEX_CARTESIAN, DataItemFormat.SIGNED_FIXED_POINT, 16, 16),
        "cf32": (SampleType.COMPLEX_CARTESIAN, DataItemFormat.IEEE754_SINGLE, 32, 32),
        "ci24": (SampleType.COMPLEX_CARTESIAN, DataItemFormat.SIGNED_FIXED_POINT, 32, 24),
    }

    proc = _start_sim(config)
    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")

        # 1) Every channel's context packet self-describes its payload format via vita49io.
        for name, sock in socks.items():
            ctx_bytes, _, _ = _recv_vita49(sock, 4)
            ctx = ContextPacket.from_bytes(ctx_bytes)
            pf = ctx.cif0.payload_format
            sample_type, fmt, ipf, di = expected_pf[name]
            assert pf.sample_type == sample_type, name
            assert pf.data_item_format == fmt, name
            assert pf.item_packing_field_size_bits == ipf, name
            assert pf.data_item_size_bits == di, name

        # 2) Collect IF-data packets keyed by timestamp, then decode a shared timestamp from all
        # three streams. Identical channels render identical samples, so the decoded I/Q match.
        collected = {name: {} for name in socks}
        deadline = time.time() + 5.0
        shared = None
        while time.time() < deadline and shared is None:
            for name, sock in socks.items():
                try:
                    packet, vita, _ = _recv_if_data(sock)
                except socket.timeout:  # pragma: no cover - diagnostic path
                    continue
                key = (vita["integer_seconds"], vita["fractional_ps"])
                collected[name][key] = packet
            shared = next(
                (k for k in collected["ci16"] if k in collected["cf32"] and k in collected["ci24"]),
                None,
            )
        assert shared is not None, "no common timestamp across all three format streams"

        decoded = {}
        for name in socks:
            dp = DataPacket.from_bytes(collected[name][shared])
            st, fmt, ipf, di = expected_pf[name]
            pf = PayloadFormat(
                sample_type=st,
                data_item_format=fmt,
                data_item_fraction_size_bits=0,
                item_packing_field_size_bits=ipf,
                data_item_size_bits=di,
            )
            decoded[name] = payload_as_numpy(bytes(dp.payload), pf)

        assert len(decoded["ci16"]) == len(decoded["cf32"]) == len(decoded["ci24"])
        # CI16 quantises to 16 bits; CF32/CI24 carry the same 16-bit samples widened, so all
        # three agree to within a CI16 quantisation step (1/32768).
        np.testing.assert_allclose(decoded["cf32"], decoded["ci16"], atol=1.0 / 32768.0)
        np.testing.assert_allclose(decoded["ci24"], decoded["ci16"], atol=1.0 / 32768.0)
    finally:
        _terminate(proc)
        for sock in socks.values():
            sock.close()


def test_native_passthrough_full_precision(tmp_path):
    """A cf32 and a ci24 passthrough capture, each replayed verbatim into a same-format channel,
    decode via vita49io to the file's exact samples -- proving the native pipeline preserves
    precision an int16 round-trip would destroy (an off-grid float; a set low byte in ci24)."""
    pytest.importorskip("vita49io")
    np = pytest.importorskip("numpy")
    from importlib.metadata import version as _pkg_version

    try:
        from packaging.version import Version

        if Version(_pkg_version("vita49io")) < Version("0.1.8"):
            pytest.skip("vita49io < 0.1.8 decodes left-justified sub-word items incorrectly")
    except ImportError:  # pragma: no cover
        pass
    from vita49io import DataPacket
    from vita49io.io.payload_codec import payload_as_numpy
    from vita49io.protocol.cif0 import DataItemFormat, PayloadFormat, SampleType

    rate, bw, center, nsamp = 1536000, 1000000, 100000000, 15360

    # cf32 capture: a constant off the 1/32768 grid; ci24 capture: a constant with a set low byte.
    cf32_i, cf32_q = 0.30001100, -0.70002200
    cf32_file = tmp_path / "cap.cf32"
    a = np.empty(nsamp * 2, dtype="<f4")
    a[0::2], a[1::2] = cf32_i, cf32_q
    cf32_file.write_bytes(a.tobytes())

    ci24_v = 0x123456  # low byte 0x56 must survive
    ci24_file = tmp_path / "cap.ci24"
    b = np.empty(nsamp * 2, dtype="<i4")
    b[0::2], b[1::2] = ci24_v, -ci24_v
    ci24_file.write_bytes(b.tobytes())

    def scenario(fmt, cap):
        return (
            "schema_version: 1\n"
            f"scenario_id: pt_{fmt}\n"
            "sources:\n"
            "- id: cap\n"
            "  source_type: iq_file\n"
            f"  format: {fmt}\n"
            "  byte_order: little_endian\n"
            "  iq_layout: interleaved_iq\n"
            "  passthrough_variants:\n"
            f"  - {{ sample_rate_hz: {rate}, bandwidth_hz: {bw}, file: {cap} }}\n"
            "signals:\n"
            "- signal_id: sig\n"
            "  source_reference: cap\n"
            "  replay_mode: range\n"
            "  frequency_range: { start_hz: 99900000, stop_hz: 100100000 }\n"
            f"  bandwidth_hz: {bw}\n"
            "  power_dbm: -55.0\n"
            "  passthrough: true\n"
        )

    def run(fmt, cap, pf):
        scen = tmp_path / f"pt_{fmt}.yaml"
        scen.write_text(scenario(fmt, cap), encoding="utf-8")
        rest_port = _free_tcp_port()
        udp_port = _free_udp_port_block()
        cfg = tmp_path / f"pt_{fmt}_cfg.yaml"
        cfg.write_text(
            f"""schema_version: 1
instance_id: "pytest_native_pt"
scenario_file: "{scen}"
log_path: "logs/pytest_native_pt.log"
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_min_hz: 99500000
    frequency_max_hz: 100500000
    bandwidth_hz: {bw}
    scan_rate_hz_per_s: 100000000000
    channels:
      - {{ channel_id: 0, center_frequency_hz: {center}, rates: [ {{ bandwidth_hz: {bw}, sample_rate_hz: {rate} }} ], output_format: {fmt}, rf_reference_power_dbm: -55.0, udp_output_port: {udp_port} }}
""",
            encoding="utf-8",
        )
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", udp_port))
        sock.settimeout(5.0)
        proc = subprocess.Popen(
            [str(SIM), "--config", str(cfg)], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        try:
            _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
            packet, _, _ = _recv_if_data(sock)
            dp = DataPacket.from_bytes(packet)
            return payload_as_numpy(bytes(dp.payload), pf)
        finally:
            _terminate(proc)
            sock.close()

    cf32_pf = PayloadFormat(
        sample_type=SampleType.COMPLEX_CARTESIAN, data_item_format=DataItemFormat.IEEE754_SINGLE,
        data_item_fraction_size_bits=0, item_packing_field_size_bits=32, data_item_size_bits=32,
    )
    iq = run("cf32", cf32_file, cf32_pf)
    np.testing.assert_allclose(iq.real, cf32_i, atol=1e-7)
    np.testing.assert_allclose(iq.imag, cf32_q, atol=1e-7)
    # The source value is off the int16 grid: an int16 round-trip could not reproduce it.
    assert abs(round(cf32_i * 32768) / 32768 - cf32_i) > 1e-6

    ci24_pf = PayloadFormat(
        sample_type=SampleType.COMPLEX_CARTESIAN, data_item_format=DataItemFormat.SIGNED_FIXED_POINT,
        data_item_fraction_size_bits=0, item_packing_field_size_bits=32, data_item_size_bits=24,
    )
    iq = run("ci24", ci24_file, ci24_pf)
    np.testing.assert_allclose(iq.real, ci24_v / 2**23, atol=1e-9)
    np.testing.assert_allclose(iq.imag, -ci24_v / 2**23, atol=1e-9)
    # The low byte (0x56) is non-zero: a 16-bit sample widened by <<8 would have zeroed it.
    assert (ci24_v & 0xFF) != 0


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
