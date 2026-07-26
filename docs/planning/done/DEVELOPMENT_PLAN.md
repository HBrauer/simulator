# Development Plan: SDR Receiver Simulator

This plan is based on `Anforderungen.md`. The first milestone is a deterministic, testable C simulator skeleton that can load a shared RF scenario, load instance-specific receiver ports, expose per-receiver REST APIs, and produce CI16 UDP streams from known IQ sources.

## Key Decisions

- Language: C11.
- Build: Meson + Ninja for fast configure/build/test loops.
- Core architecture: deterministic timebase, immutable validated scenario model, per-receiver atomic runtime config snapshots, renderer workers, and UDP output workers connected by bounded ring buffers.
- Initial correctness target: sample-accurate scenario timing, frequency placement, CI16 layout, clipping behavior, REST validation, and deterministic replay across two simulator instances using the same scenario time.
- Initial performance target: real-time at reduced stream rates and/or selected active streams first, then benchmark toward 98.304 MS/s 80-MHz and 24.576 MS/s DDC rates.

## Library Research And Install Plan

Recommended fast/default stack:

| Area | Library/tool | Why | License note | Install package names to check |
| --- | --- | --- | --- | --- |
| Build | Meson + Ninja | Meson is designed for fast full/incremental builds; Ninja is a small build backend focused on speed. | Apache-2.0 | `meson`, `ninja-build`, `pkg-config` |
| HTTP REST | GNU libmicrohttpd 1.x | Small C HTTP server library, reentrant, HTTP/1.1, multiple ports, thread pool modes, epoll/poll/select support. | LGPL | `libmicrohttpd-dev` |
| JSON | Jansson | Stable C JSON parser/emitter, no dependencies, MIT, documented test suite. | MIT | `libjansson-dev` |
| YAML config | LibYAML | C YAML parser/emitter for instance config files. Keep YAML schema simple to avoid complex native object mapping. | MIT | `libyaml-dev` |
| DSP baseline | liquid-dsp | Lightweight SDR DSP library with filters, oscillators, resampling-style primitives, and benchmarks. Use as baseline/reference before custom hot loops. | MIT | may need source build if distro package is old |
| SIMD optional | VOLK | Runtime-selected SIMD kernels for portable vectorized math. Use only after baseline renderer is correct and profiling identifies hot kernels. | LGPL-3.0-or-later | `libvolk-dev` or source |
| Threads/atomics | POSIX pthread + C11 atomics | Portable enough on Linux, no extra dependency. | system | `libc6-dev` |
| Unit tests | Check | C unit test framework with isolated address-space test execution and TAP/XML outputs. | LGPL | `check` |
| API/integration tests | Python pytest + requests + numpy | Fast to write black-box REST/UDP/IQ validators and reference DSP checks. | third-party dev only | `python3-pytest`, `python3-requests`, `python3-numpy` |
| Static analysis | clang-tidy, cppcheck | Catch C correctness issues early. | dev only | `clang-tidy`, `cppcheck` |
| Sanitizers | ASan/UBSan/TSan via Clang/GCC | Memory, undefined behavior, and threading checks in CI/local. | compiler | `clang`, `gcc` |
| Coverage | gcovr/lcov | Unit/integration coverage reports from Meson test runs. | dev only | `gcovr` or `lcov` |
| Benchmarking | Meson benchmark target + perf | Renderer throughput and UDP backpressure measurements. | system | `linux-perf` |

Source notes researched:

- Meson describes itself as an open-source build system optimized for fast full and incremental builds.
- Ninja is explicitly focused on running builds as fast as possible.
- libmicrohttpd is a small C HTTP server library with multiple threading and polling modes.
- Jansson is a dependency-free C JSON library with stable API and MIT license.
- LibYAML is a C YAML parser/emitter under MIT.
- liquid-dsp is an SDR-focused DSP library with minimal runtime dependencies and built-in benchmarks.
- VOLK provides runtime-selected SIMD kernels, useful after profiling.

## Repository Structure To Create

```text
simulator/src/
  main.c
  config.c/.h
  rest_server.c/.h
  udp_output.c/.h
  receiver.c/.h
  receiver_manager.c/.h
  scenario.c/.h
  signal_asset.c/.h
  iq_file_reader.c/.h
  renderer_80mhz.c/.h
  renderer_ddc.c/.h
  nco.c/.h
  resampler.c/.h
  fir_filter.c/.h
  ringbuffer.c/.h
  timebase.c/.h
  logging.c/.h
  metrics.c/.h
simulator/tests/
  unit/
  integration/
  fixtures/
simulator/configs/
simulator/scenarios/
simulator/assets/
simulator/scripts/
docs/
```

## Step-By-Step Implementation Plan

### 1. Project Foundation

1. Add `meson.build`, compiler warnings, debug/release options, sanitizer options, and `meson test` wiring.
2. Add a minimal `simulator/src/main.c` with argument parsing:
   - `--config simulator/configs/receiver_scanner.yaml`
   - `--scenario simulator/scenarios/scanner_fsk.yaml`
   - `--scenario-time-ns` test override
3. Add logging and error-code conventions matching `Anforderungen.md`.
4. Add CI-style local commands:
   - `meson setup build`
   - `meson compile -C build`
   - `meson test -C build`
   - `meson test -C build --suite integration`

### 2. Data Model And Validation

1. Implement receiver config structs:
   - receiver ID, frequency range, scan rate, UDP output host/ports, 4 DDC configs.
2. Implement scenario structs:
   - sources, signals, units, scenario ID.
3. Implement YAML config parser with LibYAML.
4. Implement JSON scenario parser with Jansson.
5. Add validation:
   - 0 to 40 GHz frequency range.
   - `frequency_stop_hz > frequency_start_hz`.
   - fixed vs scan derived from 80 MHz range.
   - exactly 4 DDC channels per receiver.
   - DDC IDs 0..3.
   - unique receiver IDs and UDP ports.
   - IQ file format fields supported by start model.
   - source file size matches or can derive `sample_count`.
   - signal `sample_count / sample_rate_hz <= repeat_interval_s`.

### 3. Deterministic Timebase

1. Implement `timebase.c` with:
   - wall-clock scenario day time: seconds since 00:00:00 modulo 86400.
   - deterministic override for tests.
   - `scenario_time_ns` used by scanner and source playback.
2. Implement scanner center-frequency function:
   - fixed: midpoint of configured range.
   - scan: `frequency_start_hz + scan_rate_hz_per_s * t`, with defined wrap/restart behavior at stop frequency.
3. Unit test same scenario time across two simulated instances produces identical:
   - center frequency.
   - active signal list.
   - source sample offset.

### 4. IQ Asset Reader

1. Implement CI16 little-endian interleaved IQ reader.
2. Add file-position calculation from `start_time_s`, `repeat_interval_s`, and scenario day time.
3. Add short-read/end-of-event behavior.
4. Unit test:
   - sample-count derivation from file size.
   - byte-order decode.
   - active/inactive signal windows.
   - file offset for repeated events.

### 5. DSP Correctness Renderer

1. Implement scalar reference renderer first:
   - select visible signals intersecting receiver window.
   - convert CI16 source samples to float complex working buffer.
   - optional resample if source sample rate differs from output rate.
   - NCO frequency shift.
   - gain mapping and summation.
   - CI16 clipping and little-endian output.
2. Implement DDC renderer directly from RF scenario, not by slicing the 80-MHz output.
3. Keep block sizes configurable, default to practical UDP payload multiples.
4. Unit/golden tests compare C output to Python/numpy reference output from the generated test asset.

### 6. REST API

1. Start one libmicrohttpd daemon per configured receiver port.
2. Implement endpoints:
   - `GET /api/v1/health`
   - `GET /api/v1/scenario/status`
   - `GET /api/v1/config`
   - `POST /api/v1/frequency-range`
   - `GET /api/v1/status`
   - `POST /api/v1/ddc/{ddc_id}/configure`
   - `GET /api/v1/ddc/{ddc_id}/status`
3. Use Jansson for request/response JSON.
4. Runtime changes go through command queues or atomic config snapshots.
5. Integration test with pytest/requests:
   - success responses match schema.
   - invalid frequencies return `invalid_frequency`.
   - invalid DDC ID returns a clear error.
   - config changes are reflected in status.

### 7. UDP Streaming

1. Implement UDP sender with raw CI16 payloads and configurable payload size.
2. Implement bounded single-producer/single-consumer ring buffers.
3. Track:
   - `samples_rendered`
   - `samples_dropped`
   - `udp_bytes_sent`
   - buffer fill level
   - underrun/overrun counters
4. Integration test with local UDP receiver:
   - payload length is multiple of 4 bytes.
   - I/Q order is `I0,Q0,I1,Q1`.
   - deterministic first N samples with fixed scenario time.
   - DDC out-of-window behavior is visible and documented.

### 8. Performance Path

1. Benchmark scalar renderer for 80-MHz and 20-MHz block sizes.
2. Add liquid-dsp-backed NCO/filter/resampler paths only where it reduces risk or time.
3. Profile with `perf`.
4. Add VOLK or local SIMD kernels for:
   - complex multiply/NCO.
   - float-to-ci16 clipping.
   - summation/mixing.
5. Add benchmark gates:
   - reduced-rate correctness benchmark.
   - full-rate single-stream throughput benchmark.
   - multi-stream stress benchmark with documented hardware and dropped samples.

### 9. Multi-Instance Determinism

1. Run two instances with:
   - same scenario.
   - same receiver IDs.
   - different REST/UDP ports.
   - fixed `--scenario-time-ns`.
2. Capture first N UDP bytes from equivalent streams.
3. Assert byte-for-byte identity.
4. Repeat with scanner mode and DDC channels.

### 10. Documentation And Operating Notes

1. Document config schema and scenario schema.
2. Document CI16 scaling:
   - int16 full scale = +/-32767.
   - 0 dBFS = maximum digital level.
   - clipping behavior.
3. Document performance limits discovered by benchmarks.
4. Document expected commands for GNU Radio UDP Source compatibility.

## Test Plan

### Unit Tests

- `config_test`: YAML parsing, required fields, unique ports, fixed/scan mode.
- `scenario_test`: JSON parsing, source references, timing constraints.
- `timebase_test`: scenario day time, deterministic override, scan center frequency.
- `iq_file_reader_test`: CI16 decode, sample count, file offset.
- `nco_test`: phase continuity and known tone shift.
- `renderer_test`: tone placement, clipping, visibility selection, DDC output.
- `ringbuffer_test`: capacity, wraparound, overrun/underrun counters.
- `rest_json_test`: success/error JSON bodies.

### Integration Tests

- Start simulator with `simulator/configs/receiver_scanner.yaml` and `simulator/scenarios/scanner_fsk.yaml`.
- Probe REST health/config/status.
- Change frequency range and DDC frequency through REST.
- Capture UDP packets and validate raw CI16 framing.
- Run two instances and compare deterministic stream bytes.
- Run scanner case and confirm signal moves through expected baseband offset over scenario time.

### Data Correctness Tools I Can Use

- Python/numpy reference scripts:
  - generate deterministic CI16 IQ assets.
  - compute expected active signal state.
  - compute expected frequency offset and short golden output blocks.
  - inspect UDP captures.
- `pytest` integration tests for REST and UDP.
- `tcpdump` or a Python UDP capture helper for raw stream capture.
- `xxd` for quick CI16 byte inspection.
- `perf stat` and `perf record` for throughput profiling.
- Sanitizers:
  - ASan/UBSan for unit/integration runs.
  - TSan for command queue and config snapshot tests.
- Valgrind only for small reduced-rate tests, not real-time benchmarks.

## Sample Files Included Now

- `simulator/configs/receiver_scanner.yaml`: one receiver on ports 8100/50000..50004.
- `simulator/scenarios/scanner_fsk.yaml`: one repeated CI16 tone-like asset placed at 10.005 GHz.
- `simulator/scripts/generate_sample_iq.py`: deterministic CI16 IQ asset generator.
- `simulator/assets/fsk_20mhz.c16`: generated short sample IQ file for tests.

## First Build Commands After Dependencies Are Installed

```sh
meson setup build
meson compile -C build
meson test -C build
```

Dependency install command on Debian/Ubuntu-like systems:

```sh
sudo apt-get install meson ninja-build pkg-config clang gcc libmicrohttpd-dev libjansson-dev libyaml-dev check python3-pytest python3-requests python3-numpy cppcheck clang-tidy gcovr linux-perf
```

liquid-dsp and VOLK should be treated as optional acceleration dependencies at first. If distro packages are unavailable or too old, build liquid-dsp from its current release/source after the scalar renderer and tests are in place.
