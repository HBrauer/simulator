# Simulator TODO

## Correctness

- [ ] Add a calibrated RF power model that maps `power_dbm` plus source `nominal_level_dbfs` to digital dBFS.
- [ ] Replace linear interpolation with a production resampler path:
  - [ ] FIR/polyphase scalar reference.
  - [ ] optional liquid-dsp backend.
  - [ ] golden tests against Python/numpy.
- [ ] Add explicit receiver/DDC passband filtering.
- [ ] Add scanner swept-signal golden tests:
  - [ ] fixed RF signal moves through 80-MHz baseband over scenario time.
  - [ ] same scenario time across instances gives identical baseband offset.
- [ ] Add multi-signal mixing tests with clipping and non-clipping cases.
- [x] Add scenario validation for duplicate IDs and missing source references with exact error codes.

## Runtime And Performance

- [x] Replace direct render-send loop with renderer-to-UDP ringbuffer pipeline.
- [x] Add per-stream underrun/overrun/drop counters.
- [ ] Add configurable stream enable/disable switches to avoid rendering unused streams.
- [ ] Add batch asset reads/cache memory limit handling for larger IQ files.
- [ ] Add SIMD/VOLK or hand-vectorized hot loops for:
  - [ ] interpolation.
  - [ ] NCO complex multiply.
  - [ ] gain/clipping.
  - [ ] summation.
- [ ] Add CPU affinity/thread configuration from YAML.
- [ ] Add 1, 4, and 12 receiver stress benchmarks.
- [ ] Add long-running soak test.

## REST/API

- [ ] Add `/api/v1/streams` or per-stream status endpoint.
- [x] Add per-stream metrics instead of only receiver-aggregated metrics.
- [ ] Add JSON schema-style response validation in integration tests.
- [ ] Add runtime stream enable/disable API if stream switches are implemented.
- [ ] Add REST tests for malformed paths/methods and concurrent runtime updates.

## UDP And Interop

- [ ] Add GNU Radio compatibility notes with exact UDP Source settings.
- [ ] Add optional packet-size configuration.
- [ ] Add packet pacing tests with tolerance.
- [ ] Add optional framed/timestamped mode as future extension, keeping raw mode default.

## Tooling And Documentation

- [ ] Add coverage target/report.
- [ ] Add clang-tidy configuration and target.
- [ ] Add benchmark result logging format.
- [ ] Add architecture diagram for threads/data flow.
- [ ] Add config and scenario schema docs.
- [ ] Add release checklist.
