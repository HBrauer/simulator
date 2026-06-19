import json
import socket
import subprocess
import time
from urllib.error import HTTPError
from urllib.request import Request
from pathlib import Path
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[2]
SIM = ROOT / "build" / "sdr-simulator"


def _free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _free_udp_port_block(count=5, start=30000):
    for base in range(start, 60000 - count):
        sockets = []
        try:
            for port in range(base, base + count):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(("127.0.0.1", port))
                sockets.append(sock)
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("could not find a free UDP port block")


def _write_config(path, rest_port, udp_base, ddc0_center=10005000000):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_runtime"
scenario_file: "scenarios/test_scenario_001.json"
log_path: "logs/pytest_runtime.log"
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
scenario_file: "scenarios/test_scenario_001.json"
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
            "scenarios/test_scenario_001.json",
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
        assert status["receiver_id"] == 0
        assert status["center_frequency_hz"] == 10000000000
        scenario = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/scenario/status")
        assert scenario["scenario_time_ns"] == 450000

        packet, addr = udp_sock.recvfrom(4096)
        assert addr[0] == "127.0.0.1"
        assert len(packet) == 256 * 4
        assert any(byte != 0 for byte in packet)
        metrics = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/metrics")
        assert metrics["active_streams"] == 5
        assert metrics["samples_rendered"] >= 256
        assert metrics["udp_packets_sent"] >= 1
        assert metrics["udp_bytes_sent"] >= 256 * 4
        assert metrics["udp_send_errors"] == 0
        assert "ringbuffer_overruns" in metrics
        assert "ringbuffer_underruns" in metrics
        assert "samples_dropped" in metrics
        assert len(metrics["streams"]) == 5
        assert metrics["streams"][0]["stream_type"] == "iq_80mhz"
        assert metrics["streams"][0]["stream_id"] == -1
        assert metrics["streams"][0]["active"] is True
        assert metrics["streams"][0]["udp_packets_sent"] >= 1
        assert "ringbuffer_overruns" in metrics["streams"][0]
        assert "ringbuffer_underruns" in metrics["streams"][0]
        assert "samples_dropped" in metrics["streams"][0]
        assert {stream["stream_id"] for stream in metrics["streams"][1:]} == {0, 1, 2, 3}
        streams = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/streams")
        assert streams["receiver_id"] == 0
        assert len(streams["streams"]) == 5
        assert streams["streams"][0]["stream_type"] == "iq_80mhz"
        assert streams["streams"][0]["udp_port"] == udp_port
        assert streams["streams"][0]["active"] is True
        ddc_streams = [stream for stream in streams["streams"] if stream["stream_type"] == "ddc"]
        assert {stream["stream_id"] for stream in ddc_streams} == {0, 1, 2, 3}
        assert all("in_receiver_window" in stream for stream in ddc_streams)

        expected = subprocess.check_output(
            [
                str(SIM),
                "--config",
                str(config),
                "--scenario",
                "scenarios/test_scenario_001.json",
                "--scenario-time-ns",
                "450000",
                "--render-once-samples",
                "256",
            ],
            cwd=ROOT,
        )
        assert packet == expected

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
            "scenarios/test_scenario_001.json",
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
        packet, addr = ddc_sock.recvfrom(4096)
        assert addr[0] == "127.0.0.1"
        assert len(packet) == 256 * 4
        assert all(byte == 0 for byte in packet)
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
            "scenarios/test_scenario_001.json",
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
                "scenarios/test_scenario_001.json",
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
                "scenarios/test_scenario_001.json",
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
        packet_1, _ = sock_1.recvfrom(4096)
        packet_2, _ = sock_2.recvfrom(4096)
        assert len(packet_1) == len(packet_2) == 256 * 4
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
