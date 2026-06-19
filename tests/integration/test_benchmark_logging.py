import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BENCHMARK = ROOT / "build" / "renderer_benchmark"


def test_renderer_benchmark_writes_json_report(tmp_path):
    report = tmp_path / "renderer_benchmark.json"

    completed = subprocess.run(
        [str(BENCHMARK), "2", "16", "--json", str(report), "--receivers", "4"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )

    assert "samples_per_second=" in completed.stdout
    data = json.loads(report.read_text(encoding="utf-8"))
    assert data["benchmark"] == "renderer_80mhz_scalar"
    assert data["blocks"] == 2
    assert data["samples_per_block"] == 16
    assert data["receivers"] == 4
    assert data["total_samples"] == 128
    assert data["seconds"] > 0
    assert data["samples_per_second"] > 0
