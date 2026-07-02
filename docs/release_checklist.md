# Release Checklist

Run this checklist from a clean working tree.

## Build And Tests

- `meson compile -C build`
- `meson test -C build`
- `meson compile -C build-sanitize`
- `meson test -C build-sanitize`
- `cppcheck --enable=warning,style,performance,portability --std=c11 --inline-suppr --suppress=missingIncludeSystem simulator/src simulator/tests/unit simulator/tests/benchmarks`
- `meson compile -C build clang-tidy`
- `simulator/scripts/run_coverage.sh`

## Determinism

- Confirm the README deterministic render hash still matches:

```sh
build/sdr-simulator \
  --config simulator/configs/instance_001.yaml \
  --scenario simulator/scenarios/test_scenario_001.json \
  --scenario-time-ns 450000 \
  --render-once-samples 256 | sha256sum
```

- Run the multi-instance UDP determinism integration test as part of `meson test -C build`.

## Runtime Smoke

- Start one instance with `simulator/configs/instance_001.yaml`.
- Check `/api/v1/health`, `/api/v1/status`, `/api/v1/metrics`, and `/api/v1/streams`.
- Capture one UDP packet from the receiver stream and confirm it parses as VITA 49.2 with a CI16 payload length of `stream_block_samples * 4`.
- Run `meson test -C build` for simulator and C waterfall receiver tests.

## Benchmark

- Run `build/renderer_benchmark 1000 4096 --json build/renderer_benchmark.json`.
- Record host CPU model, compiler version, build type, and JSON output with the release notes.

## Documentation

- Update `README.md` if commands, endpoints, hashes, or limitations changed.
- Update `docs/schemas.md` for config/scenario field changes.
- Update `docs/vita49_udp.md` if UDP packet format or sample rates changed.
- Update `TODO.md` so completed and deferred work is explicit.

## Git

- Confirm `git status --short` contains only intended release metadata.
- Tag with an annotated version tag when creating a release artifact.
