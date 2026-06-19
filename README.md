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

`stream_block_samples` in the instance YAML controls the raw UDP datagram payload size in IQ samples. The default is 1024 samples, and the allowed range is 1 to 4096 samples. `--stream-block-samples` overrides the YAML value for ad hoc runs.

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
494dfabd5626254ea01357d1ace3f492574fc20d5af38343e52a1a73ecf7cf13
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
meson compile -C build clang-tidy
```

Coverage report:

```sh
scripts/run_coverage.sh
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
build/renderer_benchmark 1000 4096 --json build/renderer_benchmark.json
```

The benchmark reports scalar 80-MHz renderer throughput in samples per second. The optional JSON report records `benchmark`, `blocks`, `samples_per_block`, `total_samples`, `seconds`, and `samples_per_second`. It is intended for comparing renderer changes on the same machine, not as a final full-system throughput claim.

## Documentation

- [Architecture](docs/architecture.md)
- [Config and scenario schemas](docs/schemas.md)
- [GNU Radio UDP compatibility](docs/gnuradio.md)
- [Release checklist](docs/release_checklist.md)

## Current Limits

- DSP path is scalar and correctness-first.
- Resampling is nearest-neighbor sample-rate mapping, not a production FIR/polyphase resampler.
- UDP streaming uses a renderer-to-UDP ringbuffer pipeline with sample-rate pacing.
- Metrics are available per receiver and per stream.
- Noise, gain, frontend impairments, IQ imbalance, and high-performance SIMD kernels are not yet implemented.
