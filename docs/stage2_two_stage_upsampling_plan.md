# Stage 2 plan: two-stage upsampling for narrowband signals in wideband channels

## Status

Not started. Stage 1 (parallel per-channel render, the `render_threads` config knob) already
makes the 80 MHz synthesis channel sustain real time, so this is a **CPU-efficiency
optimization, not a correctness fix**. Its value is cutting the cores a busy wideband scene
needs (roughly 5 render threads → 2–3), which matters when running several receivers or many
signals at once.

## Problem being optimized

A narrowband IQ capture mixed into a wideband channel is upsampled by a huge ratio — e.g. the
default POCSAG signals: 15 kHz content in a 32 ksps file upsampled to 98.304 MS/s (~3072×).

Commit `b512005` (correctly) moved such captures off the cheap 2-tap linear interpolation onto
the polyphase resampler to kill imaging ghosts (linear left the first image at only ~-22 dB).
The polyphase kernel is `2·RESAMPLER_RADIUS` complex taps **per output sample**, so at
98.304 MS/s × 3 signals it dominates render cost.

Measured on this repo (native build, one render thread, ch0 @466 MHz):

| Path                                   | Send rate | vs realtime |
|----------------------------------------|-----------|-------------|
| Noise floor only                       | ~92 MS/s  | ~95%        |
| + 3 POCSAG, **polyphase (current)**    | ~55 MS/s  | ~56%        |
| + 3 POCSAG, **linear (pre-b512005)**   | ~83 MS/s  | ~84%        |

The gap between 55% and 84% is the polyphase tap cost. Two-stage upsampling recovers most of
it **while keeping polyphase-grade image rejection**.

## Key idea

The reason a huge-ratio linear upsample images badly is that the *source* is barely oversampled
relative to its content (32 ksps for 15 kHz ≈ 2×). Linear interpolation's images sit near the
source rate, only ~-22 dB down. But if the signal is first brought to a rate where it is heavily
oversampled (≥ 8× its bandwidth), linear interpolation from *there* pushes the images far into
the stopband (measured ≥ ~50 dB in `b512005`'s notes), which is below any usable channel SNR.

So split the one expensive resample into two cheap stages:

1. **Stage A (few samples, good filter):** polyphase-resample the source (32 ksps) up to an
   intermediate rate `R_int` chosen so `R_int ≥ LINEAR_MIN_SOURCE_OVERSAMPLE · bandwidth`
   (i.e. reuse the existing 8× gate). This runs the costly kernel, but only produces a handful
   of samples per block, so its cost is negligible.
2. **Stage B (many samples, cheap filter):** linear-interpolate `R_int → output_rate` plus the
   NCO frequency shift. This is the per-output-sample hot loop, now 2-tap instead of
   `2·RESAMPLER_RADIUS`-tap.

Net: polyphase image rejection at roughly linear cost.

## Recommended approach: reuse the existing pre-render path (lowest risk)

The audio-modulated signal path **already** does exactly Stage A at load: it pre-renders each
audio signal to an intermediate complex-baseband ci16 buffer (`cached_prerender_t`), and the
mixer then reads from `prerender->samples / sample_rate_hz / gain`. See
`renderer_render_window_block` around [renderer.c:1084-1091](../simulator/src/renderer.c#L1084)
and `asset_cache_prerender` / `asset_cache_prerender_from_audio` in
[asset_cache.h](../simulator/src/asset_cache.h).

Plan: extend that mechanism to also pre-upsample **narrowband IQ captures** (not just audio) to
an intermediate rate at load, then let the hot path take the linear shortcut from the
now-oversampled buffer.

### Concrete steps

1. **Identify which sources need it.** A plain IQ source qualifies when its content is a large
   fraction of its own rate, i.e. exactly the condition that currently forces polyphase:
   `bandwidth_hz · LINEAR_MIN_SOURCE_OVERSAMPLE > source_rate_hz`
   (from the `allow_linear` gate at [renderer.c:1098-1099](../simulator/src/renderer.c#L1098)).
   Only these pay the polyphase penalty; well-oversampled captures already take linear and need
   no change.

2. **Pre-render at load.** In `asset_cache_load_limited`
   ([asset_cache.h:65](../simulator/src/asset_cache.h#L65)), for a qualifying IQ source, produce
   a `cached_prerender_t` by polyphase-resampling the file from `source_rate_hz` to
   `R_int = next convenient rate ≥ LINEAR_MIN_SOURCE_OVERSAMPLE · bandwidth`. Prefer an
   `R_int` that divides the wideband output rate for clean Stage-B positions
   (98 304 000 / 384 000 = 256, / 256 000 = 384 — both integer). Reuse the existing polyphase
   `resample_*` routines (the same ones `render_signal_segment` uses) so Stage A and the old
   direct path are bit-identical in the filter sense.

3. **Route the mixer through the pre-render.** Generalize the `is_audio ? prerender : asset`
   selection at [renderer.c:1084-1091](../simulator/src/renderer.c#L1084) to
   `has_prerender ? prerender : asset`, so a pre-upsampled IQ source is dispatched exactly like
   an audio pre-render: `samples_base`, `total_samples`, `source_rate_hz` all come from the
   prerender.

4. **Allow the linear shortcut for the pre-rendered buffer.** The `allow_linear` gate then sees
   `R_int` as the source rate, which by construction is ≥ 8× the bandwidth, so it enables the
   cheap 2-tap path automatically. Double check the audio carve-out: audio must *stay*
   polyphase (`allow_linear = !is_audio && …`) — keep audio excluded; only newly-pre-rendered
   IQ captures flip to linear-eligible. Cleanest is a per-source/per-signal flag like
   `prerender_is_oversampled` rather than overloading `is_audio`.

### Why this is the low-risk route

- No new hot-loop DSP: Stage B is the existing linear path, already tested.
- Stage A is the existing polyphase resampler, run once at load.
- Determinism (a block is a pure function of its index, identical across thread counts and
  instances — a load-bearing property, see the streamer grid comments) is preserved because the
  pre-render is a fixed function of the file, computed once before streaming.

## Alternative approach: per-block two-stage (no load-time cache)

If pre-rendering the whole file to `R_int` is too much memory for very long captures (~`R_int /
source_rate` × file size; ~12× for 32 ksps→384 kHz), do the two stages per block instead:
Stage A polyphase-resamples just the intermediate samples this block needs (≈ `block_samples ·
R_int / output_rate` + kernel radius — a handful) into a scratch buffer, then Stage B linear +
NCO fills the block. More code and careful phase/history bookkeeping across block seams and loop
wraps, but O(1) memory. Prefer the pre-render route unless memory forces this.

## Validation (must pass before merging)

1. **Image rejection.** Reproduce `b512005`'s measurement: render the POCSAG scene into a
   channel, FFT the output, confirm the first upsampling image stays ≥ ~50 dB down (i.e. no
   regression versus today's polyphase). This is the whole point — do not ship if images creep
   back toward the linear-path ~-22 dB.
2. **Throughput.** Re-run the single-thread synthesis benchmark; expect ~55% → ~80% (near the
   linear-path number) with `render_threads: 1`, which then needs far fewer threads for realtime.
3. **Determinism.** Byte-identical output for `render_threads` = 1 vs 4 vs 8, and across two
   instances started apart (the existing replay-sync integration test covers the cross-instance
   case).
4. **Sanitizers.** TSan + ASan clean on the wideband synthesis config (as Stage 1 was).
5. **Unit test.** Add a renderer test that a narrowband capture upsampled into a wide channel
   images ≥ ~50 dB down (mirror the existing linear-path image test in
   `simulator/tests/unit/test_renderer.c`).

## Files likely touched

- `simulator/src/asset_cache.c` / `.h` — pre-render narrowband IQ captures to `R_int`.
- `simulator/src/renderer.c` — generalize the prerender dispatch and the `allow_linear` gate;
  choose `R_int`.
- `simulator/src/sim_types.h` — possibly a per-source/signal `prerender_oversampled` flag.
- `simulator/tests/unit/test_renderer.c` — image-rejection test.
- `docs/configuration_manual.md` — note the intermediate-rate pre-render if it gains a tunable.

## Open questions

- Exact `R_int` policy: fixed multiple of bandwidth, or snapped to an integer divisor of the
  channel output rate? Integer divisor makes Stage B positions regular and cheap.
- Memory budget: cap `R_int` (as audio already caps its pre-render rate via
  `audio_prerender_max_rate_hz`) and fall back to per-block Stage A for captures that would blow
  the asset-cache budget.
- Does any non-POCSAG scenario rely on the current polyphase-of-raw-file output being
  bit-identical? If so, gate the new path behind the same `LINEAR_MIN_SOURCE_OVERSAMPLE`
  condition so only already-affected signals change.
