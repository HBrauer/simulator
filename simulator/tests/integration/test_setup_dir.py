import os
import pathlib
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[3]
SIM = ROOT / "build" / "sdr-simulator"
ASSET = ROOT / "simulator" / "assets" / "burst_1s_1msps.c16"


def render_once(*args, cwd, env=None):
    return subprocess.run(
        [str(SIM), *args, "--scenario-time-ns", "0", "--render-once-samples", "1024"],
        cwd=cwd,
        env=env,
        capture_output=True,
        timeout=20,
    )


def test_init_creates_runnable_setup_and_refuses_overwrite(tmp_path):
    setup = tmp_path / "site-a" / "sim"
    result = subprocess.run([str(SIM), "--init", str(setup)], capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert (setup / "receiver.yaml").is_file()
    assert (setup / "scenario.yaml").is_file()
    assert (setup / "assets").is_dir()

    again = subprocess.run([str(SIM), "--init", str(setup)], capture_output=True, timeout=10)
    assert again.returncode != 0
    assert b"already exists" in again.stderr

    run = render_once("--config-dir", str(setup), cwd=tmp_path)
    assert run.returncode == 0, run.stderr
    assert len(run.stdout) == 1024 * 4


def test_env_var_selects_setup_and_assets_resolve_against_it(tmp_path):
    setup = tmp_path / "sim"
    subprocess.run([str(SIM), "--init", str(setup)], check=True, capture_output=True, timeout=10)
    shutil.copy(ASSET, setup / "assets" / "burst.c16")
    scenario = setup / "scenario.yaml"
    scenario.write_text(
        scenario.read_text().replace(
            "sources: []\nsignals: []\n",
            "sources:\n"
            "- id: burst\n"
            "  source_type: iq_file\n"
            "  file: assets/burst.c16\n"
            "  format: ci16\n"
            "  byte_order: little_endian\n"
            "  iq_layout: interleaved_iq\n"
            "  sample_rate_hz: 1024000\n"
            "  bandwidth_hz: 1000000\n"
            "  center_frequency_hz: 0\n"
            "  nominal_level_dbfs: -10.0\n"
            "signals:\n"
            "- signal_id: burst\n"
            "  source_reference: burst\n"
            "  center_frequency_hz: 100000000\n"
            "  bandwidth_hz: 1000000\n"
            "  power_dbm: -70.0\n",
        )
    )

    # Run from an unrelated directory: the asset must resolve against the setup folder.
    env = {**os.environ, "SDR_SIMULATOR_CONFIG_DIR": str(setup)}
    run = render_once(cwd="/", env=env)
    assert run.returncode == 0, run.stderr
    assert len(run.stdout) == 1024 * 4

    (setup / "assets" / "burst.c16").unlink()
    missing = render_once(cwd="/", env=env)
    assert missing.returncode != 0
    assert b"assets/burst.c16" in missing.stderr


def test_no_setup_runs_builtin_starter(tmp_path):
    env = {k: v for k, v in os.environ.items() if k != "SDR_SIMULATOR_CONFIG_DIR"}
    run = render_once(cwd=tmp_path, env=env)
    assert run.returncode == 0, run.stderr
    assert len(run.stdout) == 1024 * 4
    assert b"built-in" in run.stderr


def test_missing_setup_points_at_init(tmp_path):
    run = render_once("--config-dir", str(tmp_path / "nothing"), cwd=tmp_path)
    assert run.returncode == 2
    assert b"--init" in run.stderr
