# SDR Simulator And Waterfall Receiver

This repository contains two applications:

- `simulator/`: C SDR receiver simulator based on `Anforderungen.md`.
- `receiver/`: C/SDL2 VITA 49.2 UDP receiver with decimated FFTW spectrum/waterfall display.

Current implementation includes:

- YAML instance configuration.
- JSON RF scenario loading.
- deterministic scenario-time override.
- per-receiver REST API.
- VITA 49.2 UDP output for receiver-bandwidth and DDC streams.
- cached IQ assets.
- cached PCM WAV audio assets with WBFM, AM, USB, and LSB modulation.
- scalar sample-rate-aware renderer.
- DDC out-of-window empty stream behavior.
- runtime metrics endpoint.
- unit, integration, sanitizer, and benchmark targets.

## Build

```sh
meson setup build
meson compile -C build
```

## Quick Start Example

Terminal 1, start the waterfall receiver on DDC 0. This stream is centered on the demo signal, so the SDL window should show colored waterfall lines immediately:

```sh
build/sdr-waterfall-receiver \
  --host 127.0.0.1 \
  --port 50001 \
  --fft-size 1024 \
  --sample-rate-hz 24576000
```

Terminal 2, start the simulator:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/gnuradio_demo.json \
  --stream-block-samples 1536
```

The receiver opens an SDL2 window, parses VITA 49.2 packets at UDP line rate, and displays the selected history duration across automatically computed waterfall rows. The 80-MHz scanner stream is still available on port `50000`, but the quick start uses DDC port `50001` because it is centered on the demo signal and is easier to verify visually.

For a non-continuous signal, use `simulator/scenarios/burst_1s_every_5s.json`. It emits a one-second burst every five seconds on DDC 0 over a continuous simulated noise floor:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/burst_1s_every_5s.json \
  --stream-block-samples 1536
```

For an audio-modulated radio demo, create a mono 48 kHz PCM WAV asset first:

```sh
ffmpeg -i input.mp3 -ac 1 -ar 48000 -sample_fmt s16 simulator/assets/radio_clip.wav
```

Then run the audio scenario. It places WBFM, AM, USB, and LSB signals from the same WAV file inside DDC 0:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/audio_radio_demo.json \
  --stream-block-samples 1536
```

## Run

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/test_scenario_001.json
```

`stream_block_samples` in the instance YAML controls the CI16 IQ payload size in samples inside each VITA 49.2 UDP packet. The default is 1024 samples, and the allowed range is 1 to 4096 samples. `--stream-block-samples` overrides the YAML value for ad hoc runs. For high-rate receiver tests over jumbo-frame Ethernet, use `1536` samples; that produces a `6164` byte VITA/UDP payload and avoids IP fragmentation with MTU 9000.

Receiver and DDC `sample_rate_hz` and `bandwidth_hz` are configurable in YAML. Existing defaults remain `98304000`/`80000000` for the receiver stream and `24576000`/`20000000` for DDC streams.

Useful REST endpoints for receiver 0 in the sample config:

```text
GET  http://127.0.0.1:8100/api/v1/health
GET  http://127.0.0.1:8100/api/v1/status
GET  http://127.0.0.1:8100/api/v1/config
GET  http://127.0.0.1:8100/api/v1/scenario/status
GET  http://127.0.0.1:8100/api/v1/metrics
GET  http://127.0.0.1:8100/api/v1/ddc/0/status
POST http://127.0.0.1:8100/api/v1/frequency-range
POST http://127.0.0.1:8100/api/v1/streams/80mhz
POST http://127.0.0.1:8100/api/v1/ddc/0/configure
POST http://127.0.0.1:8100/api/v1/ddc/0/stream
```

To check whether the simulator is actually sending the configured sample rate, inspect `/api/v1/metrics`. For the full-bandwidth stream, look at the `iq_80mhz` entry:

```sh
curl -s http://127.0.0.1:8100/api/v1/metrics
```

Compare `sample_rate_hz` to `actual_sample_rate_sps`. `samples_sent` is the number of CI16 samples successfully handed to UDP. `samples_late` means the sender loop missed configured pacing slots. `samples_send_dropped` means the nonblocking UDP socket could not queue a datagram. `samples_dropped` is the total discarded before successful send, and `samples_missed` includes underruns plus pacing misses.

## Multicast UDP

Set `udp_output_host` to an IPv4 multicast group such as `239.10.10.10`. The simulator sends the same VITA 49.2 UDP packets to the multicast group, and receivers join the group explicitly.

For local-machine testing, set `udp_multicast_interface: "127.0.0.1"` in the simulator config and pass `--interface 127.0.0.1` to the receiver. Otherwise the kernel may route multicast over the default physical NIC, which cannot carry a 98 MS/s CI16 stream.

Terminal 1, join DDC0 on multicast:

```sh
build/sdr-waterfall-receiver \
  --host 239.10.10.10 \
  --interface 127.0.0.1 \
  --port 50001 \
  --fft-size 1024 \
  --sample-rate-hz 24576000
```

Terminal 2, start the multicast simulator:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_multicast.yaml \
  --scenario simulator/scenarios/burst_1s_every_5s.json
```

## Deterministic Render Check

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/test_scenario_001.json \
  --scenario-time-ns 450000 \
  --render-once-samples 256 | sha256sum
```

Expected hash at the current implementation state:

```text
34ac4515f66233038dc8a2d1f6c225dfe9a8dc8a6ec58fe664cf58565e7935c2
```

### Determinism guarantees

Rendered output is a pure function of the scenario, the receiver/DDC configuration, and the
scenario time. Two instances started with the same configuration produce **byte-identical**
samples and VITA-49 timestamps for the same block, because:

- Block boundaries are anchored to a fixed grid derived from the time of day using exact
  integer math (no accumulated-nanosecond drift, even at 96 MS/s).
- Each signal's frequency-shift phase is derived from the absolute output-sample index, so it is
  continuous across blocks and identical across instances.
- Each VITA-49 packet carries the scenario time its samples were rendered for (not the send-time
  clock).
- The noise floor is drawn from a fixed reservoir at a slice chosen by the scenario `seed`, so
  even the noise is reproducible; change the `seed` for a different (still calibrated) realization.

The guarantee holds between configuration changes. A live retune is an external event and takes
effect within the ring depth (~8 blocks); the in-flight blocks rendered under the old
configuration are not retroactively changed.

### Streaming configuration

- `stream_cpus` (instance YAML): CPUs to spread the render/UDP threads across, as a comma list
  and/or ranges, e.g. `"2-7"` or `"0,2,4-6"`. Render and UDP threads are placed on distinct cores.
  Absent means no pinning. The older single-integer `stream_cpu` still works and maps to a
  one-element set.
- `noise_floor.power_dbm_per_hz` (scenario JSON): preferred way to specify the noise floor as a
  spectral density that scales with window bandwidth. The legacy total-power `power_dbm` is still
  accepted; see `docs/schemas.md`.

## Tests

The integration tests bind local REST and UDP sockets.

```sh
meson test -C build
```

Static analysis:

```sh
cppcheck --enable=warning,style,performance,portability \
  --std=c11 \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  simulator/src simulator/tests/unit simulator/tests/benchmarks
meson compile -C build clang-tidy
```

Coverage report:

```sh
simulator/scripts/run_coverage.sh
```

The script configures `build-coverage` with Meson coverage instrumentation, runs the normal test suite, and writes reports under `build-coverage/meson-logs/coveragereport/`.

Sanitizer build:

```sh
meson setup build-sanitize -Db_sanitize=address,undefined
meson compile -C build-sanitize
meson test -C build-sanitize
```

## Benchmark

```sh
build/renderer_benchmark 1000 4096
build/renderer_benchmark 1000 4096 --receivers 4 --json build/renderer_benchmark.json
build-perf/renderer_benchmark 2000 4096 \
  --config simulator/configs/instance_multicast.yaml \
  --scenario simulator/scenarios/burst_1s_every_5s.json
meson test --benchmark -C build -j 1
```

The benchmark reports scalar 80-MHz renderer throughput in samples per second. Use `--config` and `--scenario` to test a specific runtime setup. The optional JSON report records `benchmark`, `blocks`, `samples_per_block`, `receivers`, `total_samples`, `seconds`, and `samples_per_second`. Meson registers 1, 4, and 12 receiver benchmark cases. It is intended for comparing renderer changes on the same machine, not as a final full-system throughput claim.

## Full Receiver Performance Build

For one real-time receiver with one 80-MHz stream plus four 20-MHz DDC streams, use a release build with fast math, and run with larger UDP blocks:

```sh
meson setup build-perf --buildtype=release -Dfast_math=true
meson compile -C build-perf

build-perf/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/gnuradio_demo.json \
  --stream-block-samples 1536
```

On a Ryzen 7 5800XT test run, this delivered about `196.6 MSamples/s` total (`98.3 MSamples/s` on the 80-MHz stream and `24.58 MSamples/s` on each DDC) with no UDP errors, overruns, or underruns over a short local-loopback metrics sample. This path trades strict IEEE floating-point behavior for throughput; keep the default build for deterministic correctness checks. `-Dnative_optimizations=true` is available for local experiments, but was slightly slower than fast-math-only on this test host.

For `stream_block_samples: 1536`, each VITA 49.2 UDP datagram is `20 + 1536 * 4 = 6164` bytes before UDP/IP headers. This profile is intended for MTU 9000 jumbo-frame links between simulator and receiver.

## Waterfall Receiver

The receiver is implemented in C with SDL2 and FFTW3f. The default waterfall history is 30 seconds and can be changed from 1 to 60 seconds with the `-` and `+` buttons in the top toolbar. By default, the receiver computes as many waterfall rows as fit in the current window height; `--rows N` is only a manual override.

```sh
build/sdr-waterfall-receiver --host 127.0.0.1 --port 50000
```

## Documentation

- [Architecture](docs/architecture.md)
- [Config and scenario schemas](docs/schemas.md)
- [VITA 49.2 UDP output](docs/vita49_udp.md)
- [Waterfall receiver](receiver/README.md)
- [Release checklist](docs/release_checklist.md)

## Current Limits

- DSP path is correctness-first, with specialized direct/resampled and baseband/NCO render loops.
- Resampling uses a scalar Hann-windowed sinc FIR path by default; `-Dliquid_resampler=enabled` builds the optional liquid-dsp dot-product backend.
- VOLK is detected by Meson and used for direct NCO rotation when available.
- Receiver and DDC windows use an explicit rectangular passband gain based on signal/window bandwidth overlap.
- UDP streaming uses a renderer-to-UDP ringbuffer pipeline with sample-rate pacing.
- Metrics are available per receiver and per stream.
- Frontend impairments, IQ imbalance, and high-performance SIMD kernels are not yet implemented.
