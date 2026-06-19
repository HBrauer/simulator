# SDR Receiver Simulator

Software receiver simulator in C based on `Anforderungen.md`.

Current implementation includes:

- YAML instance configuration.
- JSON RF scenario loading.
- deterministic scenario-time override.
- per-receiver REST API.
- raw CI16 UDP output for 80-MHz and DDC streams.
- cached IQ assets.
- scalar sample-rate-aware renderer.
- DDC out-of-window empty stream behavior.
- runtime metrics endpoint.
- unit, integration, sanitizer, and benchmark targets.

## Build

```sh
meson setup build
meson compile -C build
```

## Run

```sh
build/sdr-simulator \
  --config configs/instance_001.yaml \
  --scenario scenarios/test_scenario_001.json
```

Useful REST endpoints for receiver 0 in the sample config:

```text
GET  http://127.0.0.1:8100/api/v1/health
GET  http://127.0.0.1:8100/api/v1/status
GET  http://127.0.0.1:8100/api/v1/config
GET  http://127.0.0.1:8100/api/v1/scenario/status
GET  http://127.0.0.1:8100/api/v1/metrics
GET  http://127.0.0.1:8100/api/v1/ddc/0/status
POST http://127.0.0.1:8100/api/v1/frequency-range
POST http://127.0.0.1:8100/api/v1/ddc/0/configure
```

## Deterministic Render Check

```sh
build/sdr-simulator \
  --config configs/instance_001.yaml \
  --scenario scenarios/test_scenario_001.json \
  --scenario-time-ns 450000 \
  --render-once-samples 256 | sha256sum
```

Expected hash at the current implementation state:

```text
a559960dd730f1adf536cd4410fca07561c1233edf2cc21c8ecb34a3ac1e5906
```

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
  src tests/unit tests/benchmarks
```

Sanitizer build:

```sh
meson setup build-sanitize -Db_sanitize=address,undefined
meson compile -C build-sanitize
meson test -C build-sanitize
```

## Benchmark

```sh
build/renderer_benchmark 1000 4096
```

The benchmark reports scalar 80-MHz renderer throughput in samples per second. It is intended for comparing renderer changes on the same machine, not as a final full-system throughput claim.

## Current Limits

- DSP path is scalar and correctness-first.
- Resampling is nearest-neighbor sample-rate mapping, not a production FIR/polyphase resampler.
- UDP streaming is paced by sample rate, but not yet a zero-copy ringbuffer pipeline.
- Metrics are per receiver, aggregated across that receiver's 80-MHz and DDC streams.
- Noise, gain, frontend impairments, IQ imbalance, and high-performance SIMD kernels are not yet implemented.
