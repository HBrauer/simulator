"""DDC sub-band extraction end to end: a multitone wideband recording replayed in shift mode,
with a narrow DDC channel streaming over VITA-49 UDP. Ratio-preserving scale-down of the real
use case (80 MHz -> 100 kHz at 768:1 becomes 9.8304 MS/s -> 12.8 kS/s at 768:1) so the test
stays CPU-light in CI. Asserts the extracted tone lands at the right frequency and amplitude,
neighbor tones fold nowhere, and a REST retune moves the channel to another sub-band."""
import math
import socket
import subprocess
import sys
import time
from pathlib import Path

from test_runtime_rest_udp import (
    _free_tcp_port,
    _free_udp_port_block,
    _parse_vita49_packet,
    _request_json,
    _terminate,
    _wait_json,
)

ROOT = Path(__file__).resolve().parents[3]
SIM = ROOT / "build" / "sdr-simulator"

SOURCE_RATE = 9830400
SOURCE_SAMPLES = 983040  # 0.1 s loop; divisible by the 48x front decimation of ratio 768
F0 = 100000000  # recording center (spans f0 +/- 4.9152 MHz)
# Loop-periodic baseband tone offsets (0.1 s loop -> 10 Hz granularity). At the initial tune
# (f0 + 200 kHz) the 201 kHz tone lands at +1 kHz in the channel; 240 kHz folds (40 kHz ->
# 1600 Hz) and must vanish; after retuning to f0 + 240 kHz, 240 kHz is DC and 241 kHz is the
# new +1 kHz tone.
TONES = [-3200000, -1500000, 201000, 240000, 241000]
CHANNEL_RATE = 12800
CHANNEL_BW = 10000
TONE_AMPLITUDE = 8000.0 / len(TONES)  # generator splits amplitude across tones


def _write_ddc_scenario(path, asset_path):
    path.write_text(
        f"""{{
  "schema_version": 1,
  "scenario_id": "pytest_ddc",
  "sources": [
    {{
      "id": "wideband",
      "source_type": "iq_file",
      "file": "{asset_path}",
      "format": "ci16",
      "byte_order": "little_endian",
      "iq_layout": "interleaved_iq",
      "sample_rate_hz": {SOURCE_RATE},
      "bandwidth_hz": 8000000,
      "center_frequency_hz": {F0},
      "sample_count": {SOURCE_SAMPLES},
      "nominal_level_dbfs": -12.0
    }}
  ],
  "signals": [
    {{
      "signal_id": "wideband_replay",
      "source_reference": "wideband",
      "replay_mode": "shift",
      "center_frequency_hz": {F0},
      "frequency_range": {{ "start_hz": {F0 - 4000000}, "stop_hz": {F0 + 4000000} }},
      "bandwidth_hz": 8000000,
      "power_dbm": -55.0
    }}
  ]
}}
""",
        encoding="utf-8",
    )


def _write_ddc_config(path, rest_port, udp_base, scenario_path):
    path.write_text(
        f"""schema_version: 1
instance_id: "pytest_ddc"
scenario_file: "{scenario_path}"
log_path: "logs/pytest_ddc.log"
stream_block_samples: 1536
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: {rest_port}
    udp_output_host: "127.0.0.1"
    frequency_start_hz: {F0 - 4000000}
    frequency_stop_hz: {F0 + 4000000}
    frontend_bandwidth_hz: 8000000
    scan_rate_hz_per_s: 0
    rf_reference_power_dbm: -55.0
    profiles:
      - {{ bandwidth_hz: {CHANNEL_BW}, sample_rate_hz: {CHANNEL_RATE}, name: "10K" }}
    channels:
      - {{ channel_id: 0, center_frequency_hz: {F0 + 200000}, bandwidth_hz: {CHANNEL_BW}, udp_output_port: {udp_base} }}
""",
        encoding="utf-8",
    )


def _dft_mag(payload, freq_hz, rate_hz):
    """Complex-DFT magnitude of a CI16 payload at freq_hz (payload windows are chosen
    loop-periodic so bins are exact)."""
    count = len(payload) // 4
    re = 0.0
    im = 0.0
    for k in range(count):
        i = int.from_bytes(payload[4 * k:4 * k + 2], "little", signed=True)
        q = int.from_bytes(payload[4 * k + 2:4 * k + 4], "little", signed=True)
        theta = 2.0 * math.pi * freq_hz * k / rate_hz
        c = math.cos(theta)
        s = math.sin(theta)
        re += i * c + q * s
        im += q * c - i * s
    return math.hypot(re, im) / count


def _recv_full_if_payload(sock, samples):
    deadline = time.time() + 5.0
    while time.time() < deadline:
        packet, _ = sock.recvfrom(65536)
        vita = _parse_vita49_packet(packet)
        if vita["packet_type"] == 1 and len(vita["payload"]) == 4 * samples:
            return vita["payload"]
    raise AssertionError("no full-size IF data packet received")


def test_ddc_channel_extracts_and_retunes(tmp_path):
    asset = tmp_path / "wideband.c16"
    generate = subprocess.run(
        [
            sys.executable,
            str(ROOT / "simulator/scripts/generate_sample_iq.py"),
            "--pattern", "multitone",
            "--sample-rate", str(SOURCE_RATE),
            "--samples", str(SOURCE_SAMPLES),
            "--tones=" + ",".join(str(t) for t in TONES),
            "--output", str(asset),
        ],
        capture_output=True,
        text=True,
    )
    assert generate.returncode == 0, generate.stderr

    scenario = tmp_path / "ddc_scenario.json"
    config = tmp_path / "ddc_config.yaml"
    _write_ddc_scenario(scenario, asset)
    rest_port = _free_tcp_port()
    udp_base = _free_udp_port_block()
    _write_ddc_config(config, rest_port, udp_base, scenario)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", udp_base))
    sock.settimeout(5.0)

    proc = subprocess.Popen(
        [str(SIM), "--config", str(config)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        _wait_json(f"http://127.0.0.1:{rest_port}/api/v1/health")

        # Initial tune f0 + 200 kHz: the 201 kHz tone is the only in-band content -> +1 kHz.
        # 1536 samples at 12.8 kS/s hold exactly 120 cycles of 1 kHz, so the bin is exact.
        payload = _recv_full_if_payload(sock, 1536)
        wanted = _dft_mag(payload, 1000.0, CHANNEL_RATE)
        assert 0.75 * TONE_AMPLITUDE <= wanted <= 1.25 * TONE_AMPLITUDE, wanted
        # The 240 kHz neighbor (40 kHz off-center) would alias to 1600 Hz; the cascade must
        # bury it (>= 60 dB below the wanted tone here, dominated by ci16 quantization).
        assert _dft_mag(payload, 1600.0, CHANNEL_RATE) < 20.0
        # Strong far tones (-3.2 MHz, -1.5 MHz) must not fold in anywhere near DC either.
        assert _dft_mag(payload, 0.0, CHANNEL_RATE) < 20.0

        # Retune to f0 + 240 kHz: new content has the 240 kHz tone at DC and 241 kHz at
        # +1 kHz; the old +1 kHz tone (201 kHz) is now 39 kHz out of band and must be gone.
        # Old blocks can still be in flight, so wait until DC energy shows the new tune.
        code, _ = _request_json(
            f"http://127.0.0.1:{rest_port}/api/v1/channels/0",
            {"center_frequency_hz": F0 + 240000},
            method="PUT",
        )
        assert code == 200
        deadline = time.time() + 10.0
        while True:
            payload = _recv_full_if_payload(sock, 1536)
            if _dft_mag(payload, 0.0, CHANNEL_RATE) > 0.75 * TONE_AMPLITUDE:
                break
            assert time.time() < deadline, "retuned content never arrived"
        assert _dft_mag(payload, 1000.0, CHANNEL_RATE) > 0.75 * TONE_AMPLITUDE
        assert _dft_mag(payload, 1600.0, CHANNEL_RATE) < 20.0
    finally:
        _terminate(proc)
        sock.close()
