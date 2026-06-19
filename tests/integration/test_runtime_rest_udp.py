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


def _free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _write_config(path, rest_port, udp_base):
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
    udp_port = _free_udp_port()
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

        code, ddc_updated = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/configure",
            {"center_frequency_hz": 10005000001},
        )
        assert code == 200
        assert ddc_updated["status"] == "ok"
        ddc_status = _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/ddc/0/status")
        assert ddc_status["center_frequency_hz"] == 10005000001
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
