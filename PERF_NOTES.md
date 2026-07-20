# Render performance notes

Measured with `renderer_benchmark` on the dev machine, `--buildtype=release
-Dnative_optimizations=true`, VOLK enabled, one core (`taskset -c 0`), best of three runs. The
printed `realtime_ratio` is how much faster than real time a **single core** renders the given
load (the wideband stream defines the real-time span; with `--all-channels` the remaining channels are rendered
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

## File replay (range/shift modes) and mmap-backed assets

Range/shift replay reuses the existing dispatch (direct baseband / VOLK rotator for equal-rate
sources), so an 80 MHz capture at 98.304 MS/s renders on the cheap direct path; the loop seam
splits a block into two segments with the NCO phase anchored to the absolute sample index, so
there is no per-wrap cost beyond a second dispatch call.

IQ files over the `asset_cache_max_bytes` budget (default 16 GiB) are mmap-backed
(`PROT_READ`/`MAP_SHARED` + `MADV_WILLNEED`). Measured on the 393 MB / 1 s / 98.304 MS/s check
scenario: mmap and full-RAM loads pace identically (~78 MS/s on the dev machine in both debug and
release, the same ceiling the pre-existing wideband UDP path shows), i.e. no measurable mmap
penalty once the page cache is warm — the file is re-read every loop, which keeps it warm.
Cold-cache first passes fault pages in on the render thread (~96k faults/s at full rate);
`MADV_WILLNEED` at load hides most of this, and sustained misses only occur if the replayed file
set exceeds RAM (observable as `samples_late` growth). No per-block touch-ahead is implemented.

## Passthrough replay (bypassing the mixer for a dedicated single-file channel)

A channel whose only job is to replay one capture pays for the general mixer's float mix bus,
per-signal gain/passband machinery, and noise-floor pass on every block, even on the mixer's
already-cheapest direct-copy dispatch. `signal.passthrough: true` (range/shift replay modes only)
adds a check ahead of the mixer (`renderer_try_passthrough` in `renderer.c`). The source supplies
one capture per channel rate (`passthrough_variants`); the check selects the variant whose rate
matches the channel and writes its samples straight to the output block -- a literal `memcpy` at
unit gain, a scaled copy otherwise, or a rotated copy for shift mode away from its nominal center
-- with no float bus, no noise floor, and no other-signal bookkeeping. It never resamples: a
channel rate with no matching variant renders silence, not an upsampled approximation (matching
the "one file per bandwidth" model). The mixer is reached only when no passthrough signal is active
for the tune (all out of range), where the passthrough signals are skipped anyway.

Measured with a standalone harness driving `renderer_render_channel_block` directly (no UDP/pacing
noise) on a 1024-sample block at 1.536 MS/s, single-thread, `-O3`, comparing `passthrough: true`
against the identical scenario with `passthrough: false` (both take the mixer's direct-baseband
dispatch, i.e. the *best case* for the general path):

| Path | ns/block | Msamples/s (single thread) |
| --- | --- | --- |
| passthrough | ~110-140 | ~7500-9100 |
| general mixer (direct-baseband path) | ~1880-2020 | ~530-545 |

Roughly a 14-17x reduction in render time for the dedicated-channel case. The gap is the mix-bus
zero/accumulate/saturate round trip (int16->float->int16 per sample) and per-block signal-loop
bookkeeping the mixer always performs, none of which passthrough needs when it's simply copying
(or rotating) one file's bytes into the packet payload.

## Where an 80 MHz "replay looks slower than generation" actually comes from (receive side)

Symptom: an 80 MHz passthrough channel shows ~63-69 MS/s with growing `gaps` on the receiver's
waterfall, worse than a generated 80 MHz stream at ~98 MS/s. It is tempting to blame the replay
render path. Measurement says otherwise, and the loss is entirely on the *receive* side.

Render-only, single core, `-O3` native, 1536-sample blocks (`renderer_benchmark`):

| Scenario (channel 0) | Msamples/s | x real time |
| --- | --- | --- |
| Generator (`test_scenario_001`, synth wideband) | ~587 | 6.0 |
| 80 MHz passthrough (`replay_passthrough`) | **~10 300** | **105** |

Passthrough renders ~17x *faster* than generation and ~105x faster than real time -- the render is
nowhere near the bottleneck. Live sim metrics under the two-channel 80 MHz demo confirm the sim
*sends* both streams cleanly: `actual_sample_rate_sps` ~98.2 MS/s per channel, `udp_send_errors=0`,
`samples_send_dropped=0`.

The gaps are kernel UDP **receive-buffer overflows**. On the dev box `net.core.rmem_max` is ~208 KB;
an 80 MHz stream is 98.304 MS/s x 4 B = **393 MB/s**, so a full socket buffer holds ~0.3 ms. Any
receiver scheduling hiccup drops datagrams. `/proc/net/snmp` showed `Udp InErrors == RcvbufErrors`
(every drop is a buffer overflow) climbing at ~19 100 drops/s; at 1536 samples/packet that is
~29.4 MS/s lost, and 98.3 - 29.4 = **68.9 MS/s** -- matching the observed 68.97 MS/s exactly.

Two things make replay *look* worse than generation even though its render is faster:
1. The 80 MHz passthrough demo config originally streamed a *second* 80 MHz channel (the wideband
   `track_tuner` reference on ch0) alongside the viewed ch1. That doubles loopback to ~196 MS/s /
   ~786 MB/s and steals CPU from the receiver so its socket overflows sooner. `receiver_scanner`'s extra
   channels are 20M each, so the single viewed 80M generator stream keeps more headroom. Ch0 is now
   `stream_enabled: false` in `receiver_passthrough.yaml`.
2. The default OS receive buffer is ~50x too small for 80 MHz. Fixes, receiver side (not sim code):
   `sudo sysctl -w net.core.rmem_max=16777216` (and `net.core.rmem_default`), and the receiver must
   request it with `setsockopt(SO_RCVBUF, ~8 MB)` -- a socket only gets a big buffer if it asks *and*
   the ceiling allows it. With a single 80 MHz stream and an adequate receive buffer the waterfall
   sees the full ~98.3 MS/s.

## DDC sub-band channels from wideband recordings (ddc.c / ddc_cache.c)

Load: `renderer_ddc_channels` benchmark — `receiver_wideband_ddc.yaml` (one 80 MHz track-tuner channel +
20 narrow DDC channels, 500 kHz .. 1 kHz) extracting from a shift-mode 80 MHz loop replay
(`benchmark_ddc.json`, the committed 10 ms capture). This entry uses `--sum-rates`, so the
printed ratio credits every channel's own stream time (the right measure for mixed-rate loads),
unlike the wideband-only crediting of the older entries above.

Measured on a 2-core codespace, default build (`simrender` at -O3, no `-march=native`):

| Measurement | Value |
| --- | --- |
| Steady state, all 21 channels (`--sum-rates` ratio) | ~480x real time aggregate |
| Wideband channel alone | 3.6x real time |
| Per-DDC-channel block cost (steady state, cache hit) | ~0.6 ms per 4096-sample block |
| Cold cache build (front cascade over one full loop) | ~1.1 s of loop per wall-second per thread |

Steady state is dominated by the tail cascade's final-stage dot products (a few hundred taps at
the channel rate) — 20 dwelling DDC channels together cost a small fraction of the wideband
channel.

**Cold retunes do not stall the stream.** With background builds (the streamer's mode after
startup prewarm), a DDC channel retuned to an uncached area renders through the direct
full-rate cascade immediately — the same per-sample cost as the build itself, ~1.1x real time
single-threaded on this box, so one retuned channel keeps real-time output while its
intermediate builds — and switches to the cached path when the build lands. The build runs the
shifted front cascade over one entire source loop: ~1 second of wall time per second of
recording per thread here (scalar rotation dominates; expect 2-3x better with
`-Dnative_optimizations=true` on real hardware), sliced across up to 8 threads
(`ddc_cache_set_build_threads`, bit-identical content for any thread count). So a 60 s
recording needs roughly 60 / (threads x per-thread rate) seconds of *background* build per new
tune area; during that window the channel costs full-rate CPU instead of intermediate-rate.
Configured centers are prewarmed (blocking) at startup; revisits are cache hits. Future
headroom if the transition window needs to shrink: fold the shift into complex-bandpass
first-stage taps (removes the per-sample oscillator, the dominant cost of both the build and
the direct path) and/or a VOLK rotator on the gather paths.
