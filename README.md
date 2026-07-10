# SDR Simulator And Waterfall Receiver

This repository contains two applications:

- `simulator/`: C SDR receiver simulator based on `Anforderungen.md`.
- `receiver/`: C/SDL2 VITA 49.2 UDP receiver with decimated FFTW spectrum/waterfall display.

Current implementation includes:

- YAML instance configuration.
- JSON RF scenario loading.
- deterministic scenario-time override.
- unified channel model: every stream is a channel with a supported {bandwidth, sample rate} profile; the wideband stream is channel 0 tracking the tuner.
- per-receiver REST API with capability discovery and runtime channel frequency/bandwidth control.
- VITA 49.2 UDP output per channel, with in-band IF-context packets announcing retunes and rate changes.
- cached IQ assets.
- cached PCM WAV audio assets with WBFM, AM, USB, and LSB modulation.
- scalar sample-rate-aware renderer.
- out-of-front-end-window channels stream empty blocks, like a hardware DDC tuned outside the digitised band.
- runtime metrics endpoint.
- unit, integration, sanitizer, and benchmark targets.

## Build

```sh
meson setup build
meson compile -C build
```

## Quick Start Example

Terminal 1, start the simulator:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/gnuradio_demo.json \
  --stream-block-samples 1536
```

Terminal 2, start the waterfall receiver on channel 1. It discovers the channel's UDP port, sample rate, and bandwidth from the REST API — no `--port`/`--sample-rate-hz` needed:

```sh
build/sdr-waterfall-receiver \
  --control-url http://127.0.0.1:8100 \
  --channel 1 \
  --fft-size 1024
```

The receiver opens an SDL2 window, parses VITA 49.2 packets at UDP line rate, and displays the selected history duration across automatically computed waterfall rows. The toolbar's left control group has a channel combo box, a center-frequency input field (click, type MHz, Enter), and a bandwidth combo box listing the supported profiles (keyboard: PgUp/PgDn or `0`-`7` for channels, Left/Right with Shift for x10 retune steps, `B` to cycle bandwidth). The wideband scanner stream is channel `0` on port `50000`; the quick start uses channel `1` (port `50001`) because it is centered on the demo signal and is easier to verify visually. Manual mode still works: `--host 127.0.0.1 --port 50001 --sample-rate-hz 24576000`.

For a non-continuous signal, use `simulator/scenarios/burst_1s_every_5s.json`. It emits a one-second burst every five seconds on channel 1 over a continuous simulated noise floor:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/burst_1s_every_5s.json \
  --stream-block-samples 1536
```

### Recorded-file replay (range and shift modes)

`simulator/scenarios/replay_range_shift.json` demonstrates replaying recorded IQ captures tied to
the tuned frequency (see `docs/schemas.md` for the full schema):

- **range mode** — the file plays *centered at the tuned frequency* whenever the channel's center
  lies inside the configured `frequency_range` (e.g. 99.9–100.1 MHz), identical output anywhere in
  the range, silence outside it.
- **shift mode** — the file content stays at its absolute RF position: configured for 100 MHz with
  a shift range of 80–120 MHz, tuning a channel to 110 MHz shows the content at −10 MHz offset
  (the IQ is rotated by `f_file − f_tune`; no band-limiting, wrap-around at ±Fs/2 is accepted).

Replay signals loop continuously and the playback position is derived from wall-clock epoch time,
so **simulator instances started at different times emit identical samples (and identical VITA-49
payloads) at identical wall-clock times**:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_replay.yaml \
  --scenario simulator/scenarios/replay_range_shift.json
```

Large captures are handled by the asset budget (`asset_cache_max_bytes`, default 16 GiB): IQ files
over the remaining budget are memory-mapped read-only instead of copied to RAM, so multi-minute
80 MHz recordings (≈ 393 MB/s of ci16) replay without exhausting memory, and concurrent instances
replaying the same capture share the page cache.

If a channel's only job is to replay one capture — no mixing with other signals or noise — add
`"passthrough": true` to its signal (`simulator/scenarios/replay_passthrough_demo.json`, run with
`simulator/configs/instance_replay_passthrough.yaml`). The source then supplies one capture file
per channel sample rate (`passthrough_variants`); the renderer picks the variant matching the
channel's current rate and streams it straight into VITA-49 packets, skipping the general mixer
(no float mix bus, no noise floor, no other signals) — roughly **15x less render time per block**
than even the mixer's own fastest (direct-copy) path in local measurements. It **never resamples**:
retune the channel to a bandwidth with no matching variant and it renders silence rather than an
upsampled approximation, so you provide exactly the bandwidths you want to support. It's exclusive,
not an overlay: while the passthrough signal is active, any other signal visible to that channel is
not rendered. Use the general renderer (no `passthrough`, which resamples as needed) when you want
a replay capture to combine with other signals or a noise floor.

```sh
build/sdr-simulator \
  --config simulator/configs/instance_replay_passthrough.yaml \
  --scenario simulator/scenarios/replay_passthrough_demo.json
```

For an audio-modulated radio demo, create a mono 48 kHz PCM WAV asset first:

```sh
ffmpeg -i input.mp3 -ac 1 -ar 48000 -sample_fmt s16 simulator/assets/radio_clip.wav
```

Then run the audio scenario. It places WBFM, AM, USB, and LSB signals from the same WAV file inside channel 1:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/audio_radio_demo.json \
  --stream-block-samples 1536
```

Each audio-modulated signal is **pre-rendered to complex-baseband IQ once at startup**, at an
intermediate rate derived from its content bandwidth, and then streamed through the ordinary IQ
path. This trades memory for a simpler, deterministic hot path: budget roughly `duration_s x
prerender_rate_hz x 4` bytes per signal (e.g. a 15 s WBFM station at ~400 kHz ≈ 24 MB; AM ≈ 6 MB;
SSB ≈ 3 MB). The buffers are counted against `asset_cache_max_bytes` and one line per signal is
logged at startup (id, rate, sample count, MB, synth time). Two optional instance-YAML keys tune
it:

- `audio_prerender_oversample` (float, default `2.0`, min `1.25`): oversampling factor over the
  content bandwidth. Higher improves image rejection at a proportional memory cost.
- `audio_prerender_max_rate_hz` (int, default `4000000`): hard ceiling on the intermediate rate,
  bounding memory for wide deviations or high audio rates.

## Run

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/test_scenario_001.json
```

`stream_block_samples` in the instance YAML controls the CI16 IQ payload size in samples inside each VITA 49.2 UDP packet. The default is 1024 samples, and the allowed range is 1 to 4096 samples. `--stream-block-samples` overrides the YAML value for ad hoc runs. For high-rate receiver tests over jumbo-frame Ethernet, use `1536` samples; that produces a `6164` byte VITA/UDP payload and avoids IP fragmentation with MTU 9000.

Each receiver declares its supported `{bandwidth_hz, sample_rate_hz}` **profiles** and a list of **channels** in the instance YAML (see `docs/schemas.md`). Channel 0 in the sample config tracks the tuner at 80 MHz / 98.304 MS/s; channels 1-4 are fixed-center 20 MHz / 24.576 MS/s. Channel center frequency and bandwidth are settable at runtime via REST; selecting a bandwidth always selects its paired sample rate, like real DDC decimation stages.

Useful REST endpoints for receiver 0 in the sample config (see `docs/rest_api.md` for the full API):

```text
GET  http://127.0.0.1:8100/api/v1/health
GET  http://127.0.0.1:8100/api/v1/capabilities
GET  http://127.0.0.1:8100/api/v1/status
GET  http://127.0.0.1:8100/api/v1/config
GET  http://127.0.0.1:8100/api/v1/scenario/status
GET  http://127.0.0.1:8100/api/v1/metrics
GET  http://127.0.0.1:8100/api/v1/channels
GET  http://127.0.0.1:8100/api/v1/channels/1
PUT  http://127.0.0.1:8100/api/v1/channels/1
POST http://127.0.0.1:8100/api/v1/channels/1/stream
POST http://127.0.0.1:8100/api/v1/frequency-range
```

Retune channel 1 and switch it to the 5 MHz profile (the sample rate follows the profile):

```sh
curl -X PUT http://127.0.0.1:8100/api/v1/channels/1 \
  -d '{"center_frequency_hz": 10012000000, "bandwidth_hz": 5000000}'
```

To check whether the simulator is actually sending the configured sample rate, inspect `/api/v1/metrics` and look at the channel's `streams` entry:

```sh
curl -s http://127.0.0.1:8100/api/v1/metrics
```

Compare `sample_rate_hz` to `actual_sample_rate_sps`. `samples_sent` is the number of CI16 samples successfully handed to UDP. `samples_late` means the sender loop missed configured pacing slots. `samples_send_dropped` means the nonblocking UDP socket could not queue a datagram. `samples_dropped` is the total discarded before successful send, and `samples_missed` includes underruns plus pacing misses.

## Multicast UDP

Set `udp_output_host` to an IPv4 multicast group such as `239.10.10.10`. The simulator sends the same VITA 49.2 UDP packets to the multicast group, and receivers join the group explicitly.

For local-machine testing, set `udp_multicast_interface: "127.0.0.1"` in the simulator config and pass `--interface 127.0.0.1` to the receiver. Otherwise the kernel may route multicast over the default physical NIC, which cannot carry a 98 MS/s CI16 stream.

Terminal 1, join channel 1 on multicast:

```sh
build/sdr-waterfall-receiver \
  --control-url http://127.0.0.1:8100 \
  --channel 1 \
  --interface 127.0.0.1 \
  --fft-size 1024
```

With `--control-url` the receiver reads `udp_output_host` from the API and joins the multicast group automatically. (Manual mode: `--host 239.10.10.10 --port 50001 --sample-rate-hz 24576000`.) Terminal 2, start the multicast simulator:

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
e0bb3a76474fc8c2e3e984b62139e09f43104d1e0eda398c568b97f0249c5b42
```

### Determinism guarantees

Rendered output is a pure function of the scenario, the receiver/channel configuration, and the
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

For one real-time receiver with one 80-MHz channel plus four 20-MHz channels, use a release build with fast math, and run with larger UDP blocks:

```sh
meson setup build-perf --buildtype=release -Dfast_math=true
meson compile -C build-perf

build-perf/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/gnuradio_demo.json \
  --stream-block-samples 1536
```

On a Ryzen 7 5800XT test run, this delivered about `196.6 MSamples/s` total (`98.3 MSamples/s` on the 80-MHz channel and `24.58 MSamples/s` on each 20-MHz channel) with no UDP errors, overruns, or underruns over a short local-loopback metrics sample. This path trades strict IEEE floating-point behavior for throughput; keep the default build for deterministic correctness checks. `-Dnative_optimizations=true` is available for local experiments, but was slightly slower than fast-math-only on this test host.

For `stream_block_samples: 1536`, each VITA 49.2 UDP datagram is `20 + 1536 * 4 = 6164` bytes before UDP/IP headers. This profile is intended for MTU 9000 jumbo-frame links between simulator and receiver.

## Waterfall Receiver

The receiver is implemented in C with SDL2 and FFTW3f. The default waterfall history is 30 seconds and can be changed from 1 to 60 seconds with the `-` and `+` buttons in the top toolbar. By default, the receiver computes as many waterfall rows as fit in the current window height; `--rows N` is only a manual override.

```sh
build/sdr-waterfall-receiver --host 127.0.0.1 --port 50000
```

## Documentation

- [Architecture](docs/architecture.md)
- [Config and scenario schemas](docs/schemas.md)
- [REST API](docs/rest_api.md)
- [VITA 49.2 UDP output](docs/vita49_udp.md)
- [Waterfall receiver](receiver/README.md)
- [Release checklist](docs/release_checklist.md)

## Current Limits

- DSP path is correctness-first, with specialized direct/resampled and baseband/NCO render loops.
- Resampling uses a scalar Hann-windowed sinc FIR path by default; `-Dliquid_resampler=enabled` builds the optional liquid-dsp dot-product backend.
- VOLK is detected by Meson and used for direct NCO rotation when available.
- Channel windows use an explicit rectangular passband gain based on signal/window bandwidth overlap.
- UDP streaming uses a renderer-to-UDP ringbuffer pipeline with sample-rate pacing.
- Metrics are available per receiver and per stream.
- Frontend impairments, IQ imbalance, and high-performance SIMD kernels are not yet implemented.
