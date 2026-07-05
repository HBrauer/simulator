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


def _write_config(path, rest_port, udp_base, ddc0_center=10005000000, stream_block_samples=None):
    stream_block_samples_line = ""
    if stream_block_samples is not None:
        stream_block_samples_line = f"stream_block_samples: {stream_block_samples}\n"
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_runtime"
scenario_file: "simulator/scenarios/test_scenario_001.json"
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
    udp_80mhz_output_port: {udp_base}
    ddc:
      - ddc_id: 0
        center_frequency_hz: {ddc0_center}
        udp_output_port: {udp_base + 1}
      - ddc_id: 1
        center_frequency_hz: 10010000000
        udp_output_port: {udp_base + 2}
      - ddc_id: 2
        center_frequency_hz: 9995000000
        udp_output_port: {udp_base + 3}
      - ddc_id: 3
        center_frequency_hz: 10030000000
        udp_output_port: {udp_base + 4}
""",
        encoding="utf-8",
    )


def _write_two_receiver_config(path, rest_port_0, rest_port_1, udp_base):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_two_receivers"
scenario_file: "simulator/scenarios/test_scenario_001.json"
log_path: "logs/pytest_two_receivers.log"
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port_0}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 9960000000
    frequency_stop_hz: 10040000000
    scan_rate_hz_per_s: 100000000000
    udp_80mhz_output_port: {udp_base}
    ddc:
      - ddc_id: 0
        center_frequency_hz: 10005000000
        udp_output_port: {udp_base + 1}
      - ddc_id: 1
        center_frequency_hz: 10010000000
        udp_output_port: {udp_base + 2}
      - ddc_id: 2
        center_frequency_hz: 9995000000
        udp_output_port: {udp_base + 3}
      - ddc_id: 3
        center_frequency_hz: 10030000000
        udp_output_port: {udp_base + 4}
  - receiver_id: 1
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port_1}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 19960000000
    frequency_stop_hz: 20040000000
    scan_rate_hz_per_s: 100000000000
    udp_80mhz_output_port: {udp_base + 5}
    ddc:
      - ddc_id: 0
        center_frequency_hz: 20000000000
        udp_output_port: {udp_base + 6}
      - ddc_id: 1
        center_frequency_hz: 20010000000
        udp_output_port: {udp_base + 7}
      - ddc_id: 2
        center_frequency_hz: 19990000000
        udp_output_port: {udp_base + 8}
      - ddc_id: 3
        center_frequency_hz: 20030000000
        udp_output_port: {udp_base + 9}
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


def _request_json(url, payload):
    data = json.dumps(payload).encode("utf-8")
    request = Request(url, data=data, method="POST", headers={"Content-Type": "application/json"})
    with urlopen(request, timeout=2.0) as response:
        return response.status, json.loads(response.read().decode("utf-8"))


def _request_json_error(url, payload):
    try:
        _request_json(url, payload)
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
            "scan_rate_hz_per_s",
            "output_scale",
            "rf_reference_power_dbm",
            "stream_enabled",
            "bandwidth_hz",
            "udp_output_host",
            "udp_multicast_interface",
            "udp_outputs",
            "streams_active",
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
        if stream["stream_type"] == "ddc":
            _assert_keys(stream, {"center_frequency_hz", "output_scale", "rf_reference_power_dbm", "in_receiver_window"})


def _wait_stream_state(rest_port, stream_type, stream_id, enabled):
    for _ in range(50):
        streams = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/streams")
        for stream in streams["streams"]:
            if stream["stream_type"] == stream_type and stream["stream_id"] == stream_id and stream["enabled"] is enabled and stream["active"] is enabled:
                return stream
        time.sleep(0.02)
    raise AssertionError(f"stream {stream_type}/{stream_id} did not reach enabled={enabled}")


def _parse_vita49_if_data(packet):
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


def _ci16_payload_has_nonzero(payload):
    assert len(payload) % 4 == 0
    for offset in range(0, len(payload), 2):
        if int.from_bytes(payload[offset:offset + 2], "little", signed=True) != 0:
            return True
    return False


def test_runtime_rest_and_udp_stream(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime.yaml"
    _write_config(config, rest_port, udp_port)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
            "--stream-block-samples",
            "256",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/status")
        _assert_receiver_status_shape(status)
        assert status["receiver_id"] == 0
        assert status["center_frequency_hz"] == 10000000000
        scenario = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/scenario/status")
        _assert_keys(scenario, {"scenario_id", "loaded", "scenario_time_ns", "source_count", "signal_count"})
        assert scenario["scenario_time_ns"] == 450000

        packet, addr = udp_sock.recvfrom(65536)
        vita = _parse_vita49_if_data(packet)
        assert addr[0] == "127.0.0.1"
        assert vita["packet_type"] == 1
        assert vita["stream_id"] == 0x53440000
        assert len(vita["payload"]) == 256 * 4
        assert _ci16_payload_has_nonzero(vita["payload"])
        metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")
        _assert_metrics_shape(metrics)
        assert metrics["active_streams"] == 5
        assert metrics["samples_rendered"] >= 256
        assert metrics["samples_sent"] >= 256
        assert metrics["samples_sent"] == metrics["samples_rendered"]
        assert metrics["actual_sample_rate_sps"] >= 0
        assert metrics["samples_missed"] >= 0
        assert metrics["samples_late"] >= 0
        assert metrics["samples_send_dropped"] >= 0
        assert metrics["udp_packets_sent"] >= 1
        assert metrics["udp_bytes_sent"] >= 20 + 256 * 4
        assert metrics["udp_send_errors"] == 0
        assert metrics["udp_send_would_block"] >= 0
        assert metrics["udp_send_no_buffer"] >= 0
        assert metrics["udp_send_other_errors"] >= 0
        assert "ringbuffer_overruns" in metrics
        assert "ringbuffer_underruns" in metrics
        assert "samples_dropped" in metrics
        assert len(metrics["streams"]) == 5
        assert metrics["streams"][0]["stream_type"] == "iq_80mhz"
        assert metrics["streams"][0]["stream_id"] == -1
        assert metrics["streams"][0]["sample_rate_hz"] == 98304000
        assert metrics["streams"][0]["active"] is True
        assert metrics["streams"][0]["samples_sent"] == metrics["streams"][0]["samples_rendered"]
        assert metrics["streams"][0]["actual_sample_rate_sps"] >= 0
        assert metrics["streams"][0]["samples_missed"] >= 0
        assert metrics["streams"][0]["samples_late"] >= 0
        assert metrics["streams"][0]["samples_send_dropped"] >= 0
        assert metrics["streams"][0]["udp_packets_sent"] >= 1
        assert metrics["streams"][0]["udp_send_would_block"] >= 0
        assert metrics["streams"][0]["udp_send_no_buffer"] >= 0
        assert metrics["streams"][0]["udp_send_other_errors"] >= 0
        assert "ringbuffer_overruns" in metrics["streams"][0]
        assert "ringbuffer_underruns" in metrics["streams"][0]
        assert "samples_dropped" in metrics["streams"][0]
        assert {stream["stream_id"] for stream in metrics["streams"][1:]} == {0, 1, 2, 3}
        streams = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/streams")
        _assert_stream_status_shape(streams)
        assert streams["receiver_id"] == 0
        assert len(streams["streams"]) == 5
        assert streams["streams"][0]["stream_type"] == "iq_80mhz"
        assert streams["streams"][0]["udp_port"] == udp_port
        assert streams["streams"][0]["enabled"] is True
        assert streams["streams"][0]["active"] is True
        ddc_streams = [stream for stream in streams["streams"] if stream["stream_type"] == "ddc"]
        assert {stream["stream_id"] for stream in ddc_streams} == {0, 1, 2, 3}
        assert all("in_receiver_window" in stream for stream in ddc_streams)
        assert all(stream["enabled"] is True for stream in ddc_streams)

        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/streams/80mhz",
            {"enabled": False},
        )
        assert code == 200
        assert stream_update["enabled"] is False
        _wait_stream_state(rest_port, "iq_80mhz", -1, False)
        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/streams/80mhz",
            {"enabled": True},
        )
        assert code == 200
        assert stream_update["enabled"] is True
        _wait_stream_state(rest_port, "iq_80mhz", -1, True)

        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/stream",
            {"enabled": False},
        )
        assert code == 200
        assert stream_update["stream_type"] == "ddc"
        assert stream_update["stream_id"] == 0
        assert stream_update["enabled"] is False
        _wait_stream_state(rest_port, "ddc", 0, False)
        code, stream_update = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/stream",
            {"enabled": True},
        )
        assert code == 200
        assert stream_update["enabled"] is True
        _wait_stream_state(rest_port, "ddc", 0, True)

        expected = subprocess.check_output(
            [
                str(SIM),
                "--config",
                str(config),
                "--scenario",
                "simulator/scenarios/test_scenario_001.json",
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
        assert all(ddc["output_scale"] == 0.5 for ddc in scaled["ddc"])

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

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/99/configure",
            {"center_frequency_hz": 10005000000},
        )
        assert code == 404
        assert error["error"]["code"] == "invalid_ddc_id"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/configure",
            {"center_frequency_hz": "bad"},
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_request"

        code, error = _request_json_error(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/configure",
            {"output_scale": -1.0},
        )
        assert code == 400
        assert error["error"]["code"] == "invalid_output_scale"

        code, ddc_updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/configure",
            {"center_frequency_hz": 10005000001, "output_scale": 0.25},
        )
        assert code == 200
        assert ddc_updated["status"] == "ok"
        ddc_status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/status")
        assert ddc_status["center_frequency_hz"] == 10005000001
        assert ddc_status["output_scale"] == 0.25
        assert ddc_status["in_receiver_window"] is True

        code, ddc_updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/configure",
            {"center_frequency_hz": 10060000000},
        )
        assert code == 200
        assert ddc_updated["status"] == "ok"
        ddc_status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/status")
        assert ddc_status["in_receiver_window"] is False
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        udp_sock.close()
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


def test_vita49_udp_wraps_and_keeps_raw_payload(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_framed.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=128)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        packet, addr = udp_sock.recvfrom(65536)
        vita = _parse_vita49_if_data(packet)
        assert addr[0] == "127.0.0.1"
        assert vita["packet_type"] == 1
        assert vita["tsi"] == 1
        assert vita["tsf"] == 2
        assert vita["sequence"] == 0
        assert vita["stream_id"] == 0x53440000
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
                "simulator/scenarios/test_scenario_001.json",
                "--scenario-time-ns",
                str(grid_ns),
                "--render-once-samples",
                "128",
            ],
            cwd=ROOT,
        )
        assert payload == expected
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        udp_sock.close()
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


def test_stream_block_samples_config_controls_udp_packet_size(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_packet_size.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=128)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("127.0.0.1", udp_port))
    udp_sock.settimeout(5.0)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        packet, addr = udp_sock.recvfrom(65536)
        assert addr[0] == "127.0.0.1"
        vita = _parse_vita49_if_data(packet)
        assert len(vita["payload"]) == 128 * 4
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        udp_sock.close()
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


def test_ddc_4096_sample_vita49_packet_contains_signal(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_ddc_4096_signal.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=4096)

    ddc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ddc_sock.bind(("127.0.0.1", udp_port + 1))
    ddc_sock.settimeout(5.0)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        health = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")
        assert health["status"] == "ok"
        packet, addr = ddc_sock.recvfrom(65536)
        assert addr[0] == "127.0.0.1"
        vita = _parse_vita49_if_data(packet)
        assert vita["stream_id"] == 0x53440001
        assert len(vita["payload"]) == 4096 * 4
        assert _ci16_payload_has_nonzero(vita["payload"])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        ddc_sock.close()
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


def test_concurrent_rest_updates_and_status_reads(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_concurrent.yaml"
    _write_config(config, rest_port, udp_port)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
            "--stream-block-samples",
            "128",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
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

    def update_ddc():
        try:
            for index in range(20):
                code, body = _request_json(
                    f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/configure",
                    {
                        "center_frequency_hz": 10005000000 + index,
                        "output_scale": 0.5,
                    },
                )
                assert code == 200
                assert body["status"] == "ok"
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
            threading.Thread(target=update_ddc),
            threading.Thread(target=read_status),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10.0)
        assert not errors
        assert all(not thread.is_alive() for thread in threads)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


def test_ddc_udp_stream_is_empty_when_outside_receiver_window(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_ddc_outside.yaml"
    _write_config(config, rest_port, udp_port, ddc0_center=10060000000)

    ddc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ddc_sock.bind(("127.0.0.1", udp_port + 1))
    ddc_sock.settimeout(5.0)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
            "--stream-block-samples",
            "256",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ddc_status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/status")
        assert ddc_status["in_receiver_window"] is False
        packet, addr = ddc_sock.recvfrom(65536)
        vita = _parse_vita49_if_data(packet)
        assert addr[0] == "127.0.0.1"
        assert len(vita["payload"]) == 256 * 4
        assert all(byte == 0 for byte in vita["payload"])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        ddc_sock.close()
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


def test_two_receivers_expose_independent_rest_apis(tmp_path):
    rest_port_0 = _free_tcp_port()
    rest_port_1 = _free_tcp_port()
    udp_base = _free_udp_port_block(count=10)
    config = tmp_path / "runtime_two_receivers.yaml"
    _write_two_receiver_config(config, rest_port_0, rest_port_1, udp_base)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
            "--stream-block-samples",
            "128",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        status_0 = _wait_json(f"http://127.0.0.1:{rest_port_0}/api/v1/status")
        status_1 = _wait_json(f"http://127.0.0.1:{rest_port_1}/api/v1/status")
        assert status_0["receiver_id"] == 0
        assert status_1["receiver_id"] == 1
        assert status_0["center_frequency_hz"] == 10000000000
        assert status_1["center_frequency_hz"] == 20000000000
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr


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
        subprocess.Popen(
            [
                str(SIM),
                "--config",
                str(config_1),
                "--scenario",
                "simulator/scenarios/test_scenario_001.json",
                "--scenario-time-ns",
                "450000",
                "--stream-block-samples",
                "256",
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ),
        subprocess.Popen(
            [
                str(SIM),
                "--config",
                str(config_2),
                "--scenario",
                "simulator/scenarios/test_scenario_001.json",
                "--scenario-time-ns",
                "450000",
                "--stream-block-samples",
                "256",
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ),
    ]
    try:
        status_1 = _wait_json(f"http://127.0.0.1:{rest_port_1}/api/v1/status")
        status_2 = _wait_json(f"http://127.0.0.1:{rest_port_2}/api/v1/status")
        assert status_1["receiver_id"] == status_2["receiver_id"] == 0
        packet_1, _ = sock_1.recvfrom(65536)
        packet_2, _ = sock_2.recvfrom(65536)
        vita_1 = _parse_vita49_if_data(packet_1)
        vita_2 = _parse_vita49_if_data(packet_2)
        assert len(vita_1["payload"]) == len(vita_2["payload"]) == 256 * 4
        assert packet_1 == packet_2
    finally:
        for proc in procs:
            proc.terminate()
        for proc in procs:
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5.0)
            stderr = proc.stderr.read() if proc.stderr else ""
            assert proc.returncode in (0, -15), stderr
        sock_1.close()
        sock_2.close()


def test_runtime_soak_keeps_streaming_without_send_errors(tmp_path):
    rest_port = _free_tcp_port()
    udp_port = _free_udp_port_block()
    config = tmp_path / "runtime_soak.yaml"
    _write_config(config, rest_port, udp_port, stream_block_samples=512)

    proc = subprocess.Popen(
        [
            str(SIM),
            "--config",
            str(config),
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
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
        proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5.0)
        stderr = proc.stderr.read() if proc.stderr else ""
        assert proc.returncode in (0, -15), stderr
