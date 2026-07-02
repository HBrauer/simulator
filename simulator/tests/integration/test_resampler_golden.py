import math
import struct
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SIM = ROOT / "build" / "sdr-simulator"


def _clip_i16(value):
    rounded = int(round(value))
    return max(-32768, min(32767, rounded))


def _sinc(x):
    if abs(x) < 1e-12:
        return 1.0
    return math.sin(math.pi * x) / (math.pi * x)


def _hann(distance):
    normalized = abs(distance) / 4.0
    if normalized >= 1.0:
        return 0.0
    return 0.5 + 0.5 * math.cos(math.pi * normalized)


def _resample(samples, position):
    center = math.floor(position)
    acc_i = 0.0
    acc_q = 0.0
    weight_sum = 0.0
    for tap in range(-3, 5):
        index = center + tap
        if index < 0 or index >= len(samples):
            continue
        distance = position - index
        weight = _sinc(distance) * _hann(distance)
        acc_i += samples[index][0] * weight
        acc_q += samples[index][1] * weight
        weight_sum += weight
    return acc_i / weight_sum, acc_q / weight_sum


def _read_ci16(path):
    data = path.read_bytes()
    values = struct.unpack("<" + "hh" * (len(data) // 4), data)
    return list(zip(values[0::2], values[1::2]))


def _unpack_ci16(data):
    values = struct.unpack("<" + "hh" * (len(data) // 4), data)
    return list(zip(values[0::2], values[1::2]))


def test_80mhz_resampler_matches_python_windowed_sinc_reference():
    rendered = subprocess.check_output(
        [
            str(SIM),
            "--config",
            "simulator/configs/instance_001.yaml",
            "--scenario",
            "simulator/scenarios/test_scenario_001.json",
            "--scenario-time-ns",
            "450000",
            "--render-once-samples",
            "8",
        ],
        cwd=ROOT,
    )
    actual = _unpack_ci16(rendered)
    asset = _read_ci16(ROOT / "simulator/assets/fsk_20mhz.c16")
    offset = 11059
    source = asset[offset : offset + 8]
    expected = []
    phase_step = 2.0 * math.pi * 5_000_000.0 / 98_304_000.0
    for index in range(8):
        ii, qq = _resample(source, index * 0.25)
        phase = phase_step * index
        rotated_i = ii * math.cos(phase) - qq * math.sin(phase)
        rotated_q = ii * math.sin(phase) + qq * math.cos(phase)
        expected.append((_clip_i16(rotated_i), _clip_i16(rotated_q)))
    assert actual == expected
