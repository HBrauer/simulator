# Render performance notes

Measured with `renderer_benchmark` on the dev machine, `--buildtype=release
-Dnative_optimizations=true`, VOLK enabled, one core (`taskset -c 0`), best of three runs. The
printed `realtime_ratio` is how much faster than real time a **single core** renders the given
load (the wideband stream defines the real-time span; with `--with-ddc` the four DDCs are rendered
per block but only the wideband duration is credited, so those rows are ~5x lower structurally).
`>= 1.3` is the P9 target of 30% headroom.

## After the audio pre-render + polyphase-resampler rework (IMPLEMENTATION_PLAN_AUDIO)

| Load | before | after |
| --- | --- | --- |
| Wideband, 1 IQ signal, no noise (`test_scenario_001`) | 7.74 | **9.04** |
| Wideband + 4 DDCs, 1 IQ signal (`test_scenario_001`) | 1.70 | **1.86** |
| Wideband, 4 audio signals + noise (`audio_radio_demo`) | 0.24 | **0.19** |
| Wideband + 4 DDCs, mixed IQ+audio+noise (`benchmark_load`) | 0.08 | **0.10** |

("before" = commit 9018fb7, the old per-sample audio synthesis + quarter-rate-only precomputed
resampler kernels.)

## What the rework achieved — and what it did not

**IQ resampling got faster and simpler (the unambiguous win).** The general resampler no longer
computes `sinc()`+`hann()` per tap per output sample; every non-quarter ratio now uses a
precomputed 256-phase polyphase table, and the resampled-NCO path fills an aligned float scratch
(fixed-point phase accumulator, single-precision 8-tap dot product) and applies one VOLK rotator
pass. That lifts pure-IQ loads (7.74 -> 9.04 wideband; 1.70 -> 1.86 with DDCs) and, more
importantly, makes *any* non-quarter IQ source fast rather than only the hardcoded quarter ratio.

**The audio pre-render did not deliver the projected speedup, and regresses AM/SSB.** The plan's
budget math assumed the old audio path was uniformly expensive and that resampling a pre-rendered
buffer would be "≈ the IQ path, a few ns/sample." Measurement contradicts both:

- The old `render_audio_modulated` only used a transcendental (`sincos`) for **WBFM**. AM, USB and
  LSB synthesised their carrier with a cheap per-sample *linear* audio lookup and a rotation — no
  transcendentals. Replacing that with an 8-tap polyphase resample of the pre-rendered buffer is
  *more* expensive for those three modulations, so the AM/SSB-heavy `audio_radio_demo` regresses
  (0.24 -> 0.19). WBFM alone improves; the mix does not.
- Placing a 200 kHz signal into the 98.304 MS/s wideband intrinsically requires 98.304 M resample
  operations per second per signal. One active audio signal costs ~0.76x wideband on this core
  regardless of the modulation math — the resample + rotate + accumulate passes dominate, not the
  synthesis. Pre-rendering removes the `sincos` (a real WBFM win) but cannot make audio "as cheap
  as a rate-matched IQ source," because audio is never rate-matched.

**`benchmark_load` at 0.10x (target 1.3x) is dominated by the noise floor, not audio.** With this
scenario's noise enabled, a **signal-free** wideband block already renders at only ~1.0x (vs ~9.4x
with noise off). Across the wideband + 4 DDCs that noise cost alone caps the ratio near ~0.2x
*before any signal is rendered*. No amount of audio-path optimisation reaches 1.3x here; the
remaining headroom is in the noise-floor generator and in the DDC-stream multiplication, both
outside this plan's scope.

## R1 escalation — resolved

The original R1 escalation ("synthesise audio at a low intermediate rate, then upsample + shift")
is **implemented**: audio is pre-rendered to complex baseband at ~content-bandwidth rate and
streamed through the IQ path (see `asset_cache.c` and `renderer.c`). It removed the per-sample
`sincos` from WBFM and unified audio onto the (now faster) IQ dispatch, with determinism preserved
by construction and quality verified end-to-end (parity vs full-rate synthesis within 0.5 dB, SSB
image rejection >= 44 dB, FM-demod SNR >= 40 dB across >= 16 grid blocks). It did **not** reach the
1.3x P9 target for `benchmark_load`, because that target is set by the noise floor across five
streams rather than by the audio synthesis the plan targeted.

## Practical guidance

- Pure-IQ scenarios run with large single-core headroom and got faster.
- For real-time 96 MS/s with audio, the per-signal audio cost (~0.76x wideband each) still bounds
  how many simultaneous audio stations one core sustains; it is not materially better than before
  for AM/SSB, and modestly better for WBFM.
- Reaching the mixed-load P9 target would next require optimising the **noise-floor** generator
  (the measured bottleneck) and/or intra-block thread slicing across cores (the mix bus already
  makes this safe) — neither is part of this change.

## Startup pre-render cost (AP2)

Pre-rendering the four-station `audio_radio_demo` (a 7.67 s, 48 kHz clip) takes ~0.18 s total at
startup (WBFM 396 kHz ~0.12 s, AM 96 kHz ~0.03 s, USB/LSB 48 kHz ~0.016 s each) — well under the
2 s budget.
