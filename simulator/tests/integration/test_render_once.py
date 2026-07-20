import hashlib
import subprocess


def test_render_once_is_deterministic():
    cmd = [
        "build/sdr-simulator",
        "--config",
        "simulator/configs/receiver_scanner.yaml",
        "--scenario",
        "simulator/scenarios/scanner_fsk.json",
        "--scenario-time-ns",
        "450000",
        "--render-once-samples",
        "256",
    ]
    first = subprocess.check_output(cmd)
    second = subprocess.check_output(cmd)
    assert len(first) == 256 * 4
    assert first == second
    assert hashlib.sha256(first).hexdigest()
