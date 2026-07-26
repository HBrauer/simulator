# VITA 49.2 Simulator And Waterfall Receiver Plan

## Goals

- Restructure this repository into separate simulator and receiver applications.
- Replace raw CI16 UDP output and the optional `SDR1` frame mode with VITA 49.2 UDP packets.
- Add a separate receiver application that consumes the VITA 49.2 UDP stream and displays a live spectrum and waterfall.
- Make sample rate and bandwidth configurable per receiver and per DDC stream.
- Use `vita49io` only as an external Python reference decoder in tests, not as receiver runtime code.

## Target Repository Layout

```text
.
├── simulator/
│   ├── src/
│   ├── configs/
│   ├── scenarios/
│   ├── assets/
│   ├── tests/
│   ├── scripts/
│   └── meson.build
├── receiver/
│   ├── README.md
│   ├── src/
│   └── tests/
├── docs/
├── meson.build
└── README.md
```

## Implementation Steps

1. Move simulator-specific source, configs, scenarios, assets, scripts, and tests under `simulator/`.
2. Keep a root Meson build that delegates to `simulator/` so existing build commands remain simple.
3. Update all paths in tests, docs, configs, and runtime defaults after the move.
4. Extend YAML config parsing and validation:
   - receiver `bandwidth_hz`
   - receiver `sample_rate_hz`
   - DDC `bandwidth_hz`
   - DDC `sample_rate_hz`
5. Preserve existing defaults:
   - receiver bandwidth: `80000000`
   - receiver sample rate: `98304000`
   - DDC bandwidth: `20000000`
   - DDC sample rate: `24576000`
6. Validate that sample rate and bandwidth are non-zero and compatible with existing renderer assumptions.
7. Replace the current UDP datagram format:
   - remove/deprecate raw CI16 as the default external format.
   - remove/deprecate the simulator-specific `SDR1` framed mode.
   - emit VITA 49.2 IF data packets carrying CI16 IQ payloads.
8. Add simulator-side VITA packet construction:
   - packet header.
   - stream ID per receiver stream.
   - packet sequence counter.
   - integer timestamp derived from scenario time.
   - CI16 payload copied without changing internal renderer format.
9. Add tests for:
   - configurable sample rate/bandwidth parsing and defaults.
   - VITA packet header fields.
   - sequence counter wrap.
   - UDP packet payload matching deterministic render output after VITA header removal.
10. Add a C receiver application:
    - UDP socket input.
    - VITA packet parsing.
    - CI16 IQ conversion to complex float.
    - decimated FFT/spectrum calculation.
    - SDL2 waterfall display.
11. Keep the receiver parser local to this repository and verify simulator packets with `vita49io` as an external reference when needed.
12. Add receiver tests:
    - parse simulator-generated VITA packets.
    - convert CI16 payload to complex samples.
    - compute waterfall frames.
13. Update documentation:
    - root README describes the two applications.
    - simulator README documents VITA 49.2 output and configurable rates/bandwidth.
    - receiver README documents running the waterfall viewer.

## Test Plan

- `meson setup build`
- `meson compile -C build`
- `meson test -C build`
- `meson test -C build`
- manual smoke test:
  - start simulator with local config.
  - start receiver on the configured UDP port.
  - confirm packets are received and FFT frames are produced.
