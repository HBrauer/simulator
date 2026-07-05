# Render performance notes (C4 benchmark results)

Measured with `renderer_benchmark` on the dev machine, `--buildtype=release
-Dnative_optimizations=true`, VOLK enabled, 500 blocks of 4096 samples. The printed
`realtime_ratio` is how much faster than real time a **single core** renders the given load
(the wideband stream defines the real-time span). `>= 1.3` is the P9 target of 30% headroom.

| Load | realtime_ratio | samples/s |
| --- | --- | --- |
| Wideband only, 1 IQ signal, no noise (`test_scenario_001`) | **4.49** | 441 M |
| 1 receiver, wideband + 4 DDCs, 1 IQ signal | **1.47** | 724 M |
| Wideband only, 4 audio signals + noise (`audio_radio_demo`) | **0.14** | 14 M |
| 1 receiver, wideband + 4 DDCs, mixed IQ+audio+noise (`benchmark_load`) | **0.07** | 35 M |

## What the numbers say

- **The IQ path meets the 96 MS/s goal comfortably** (4.49x wideband, 1.47x with all DDCs). The
  mix-bus + VOLK + precomputed-kernel work (C1/C2/B2) is doing its job: a single core sustains a
  full receiver of IQ content with headroom.
- **The audio-modulation path is the bottleneck** and does **not** currently reach real time
  (0.14x for the wideband alone). Root cause: `render_audio_modulated` synthesises the modulated
  carrier one output sample at a time at the **full output rate** (98.304 MS/s) — a `sincos`,
  an integral/interpolation lookup, and a complex rotation per output sample, per signal. Four
  audio stations means ~400M per-sample transcendental evaluations per second, which no amount of
  micro-optimisation (the `sincos` fusion applied here barely moved it) will bring to real time.

## Escalation (plan R1)

The correct fix is architectural and was deliberately left out of this pass because it is a
large, higher-risk change: **synthesise each narrowband audio signal at a low intermediate rate
(≈ a small multiple of its bandwidth), then upsample + frequency-shift into the wideband**, so
the expensive per-sample modulation math runs at ~0.5–1 MS/s instead of 98 MS/s. This is the
"generate baseband into scratch, then rotate/accumulate" idea from the plan's C2/§3. Expected
speedup for the audio path is roughly the rate ratio (50–100x), which would bring the mixed load
well above real time.

Secondary option if a single core still falls short after that: **intra-block thread slicing**
(split each block across cores) — the float mix bus already makes this safe.

Until the audio path is rearchitected, deployments that need real-time 96 MS/s should keep the
number of simultaneous audio-modulated signals per wideband stream small; pure-IQ scenarios are
unaffected and run with large headroom.
