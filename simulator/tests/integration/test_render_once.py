import hashlib
import subprocess


def test_render_once_is_deterministic():
    cmd = [
        "build/sdr-simulator",
        "--config",
        "simulator/configs/instance_001.yaml",
        "--scenario",
        "simulator/scenarios/test_scenario_001.json",
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
