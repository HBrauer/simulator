# Simulator Review — Findings & Improvement Suggestions

Scope: `simulator/src` (renderer, scenario, asset cache, WAV/IQ readers, streamer, receiver, timebase). Build folders ignored.

**Design constraints considered throughout:**
- Must sustain 96 MS/s output, with several synchronized channels in parallel → heavy DSP
  (long FIRs, per-sample trig) is off the table on the wideband path.
- Multiple instances started with the same configuration must produce the same data with the
  same timestamps (noise may differ). Everything must therefore derive from *scenario time*,
  never from "when did this thread happen to run".

## 1. Correctness issues (likely audible / measurable artifacts)

### 1.1 NCO phase resets at every block boundary (renderer.c)
`render_audio_modulated`, `render_direct_nco` and `render_resampled_nco` all start their
frequency-shift oscillator at phase 0 (`shift_c = 1, shift_s = 0`) for every rendered block
(`renderer.c:288`, `renderer.c:462`, `renderer.c:583`). Unless `offset_hz * block_duration`
is an integer number of cycles, every block boundary introduces a phase jump. The result is
periodic clicks in demodulated audio and spectral spurs spaced at the block rate.

**Fix:** carry the mixer phase across blocks. Compute the starting phase deterministically from
absolute scenario time: `phase0 = 2π · offset_hz · (block_start_sample / fs)` (use `fmod` in
double or a 64-bit fixed-point phase accumulator). Prefer this over mutable per-worker oscillator
state: deriving the phase from scenario time keeps independent instances and independent
channels bit-identical and phase-aligned, which mutable state does not.

**Determinism note:** block start times currently come from `timebase_now_ns()` clamped forward
(streamer.c:257), so block boundaries land at arbitrary wall-clock instants and differ per run —
which also makes the phase-reset artifacts differ per run. Quantize render times to a fixed
block grid (`render_time = floor(now / block_duration) * block_duration`) so every instance
renders the exact same blocks. Combined with time-derived phase this makes the output
reproducible across instances (modulo noise), which is currently only approximately true.

### 1.2 Fractional sample offset is dropped between blocks (iq_file_reader.c:80)
`iq_signal_active` floors `occurrence_elapsed * sample_rate` to get `sample_offset`. Each block
therefore restarts source playback on an integer source sample, losing up to one source-sample
of timing per block. Combined with 1.1 this makes IQ/audio playback jitter at block boundaries.

**Fix:** return the fractional part too and feed it into the resampler start position
(`source_position = fraction + i * source_per_output`), or derive the offset from an absolute
sample counter instead of recomputing from wall time per block.

### 1.3 Recursive sin/cos oscillators drift in amplitude
The rotators update via `next_c = c·step_c − s·step_s` with no renormalization
(e.g. renderer.c:314, 466, 538, 572, 598). In float (the quarter-rate and low-rate paths) the
magnitude drifts noticeably over long blocks; in double it is slow but nonzero.

**Fix:** renormalize every ~512 iterations: `mag = 1/sqrt(c²+s²); c *= mag; s *= mag;` (the VOLK
rotator already does this internally — only the scalar paths need it).

### 1.4 Decimation without anti-alias filtering (renderer.c resampled paths)
`resample_ci16` / the quarter-rate kernels are *interpolation* kernels with cutoff at the source
Nyquist. When `source_per_output > 1` (source rate higher than output rate — the normal case for
a narrow DDC over a wideband IQ source) all energy above the output Nyquist folds back into the
band. A 96 MS/s source rendered into a 48 kHz DDC will be dominated by aliases.

**Fix (performance-aware):** a full polyphase decimator on the 96 MS/s path is too expensive
and unnecessary. Cheap options that fit the budget:
- Scale the existing sinc kernel cutoff to `min(1, 1/source_per_output)` — same 8 taps, just a
  different (precomputed) kernel; near-zero extra cost, removes the worst of the aliasing.
- For integer power-of-two ratios (the common DDC case), a cascade of half-band decimators is
  the cheapest correct structure: each stage is a short FIR where half the taps are zero, and
  each stage runs at successively lower rate, so almost all work happens at low sample rates.
- liquid-dsp `msresamp_crcf` (already an optional dependency) does exactly this internally;
  use it only on DDC paths (low output rates), never on the 80 MHz path.

### 1.5 Partial-overlap signals are attenuated, not filtered (renderer.c:226)
`signal_passband_gain` scales a signal whose band only partially overlaps the window by
`sqrt(overlap/bw)`, but the *whole* signal is still mixed in. Content outside the window then
sits beyond the output Nyquist and aliases back inside. A signal straddling the DDC edge shows
up mirrored inside the DDC band instead of cut off.

**Fix (performance-aware):** a per-window brickwall filter at 96 MS/s is not affordable, and
the attenuation approximation is a reasonable engineering trade-off for the wideband stream.
The cheap correctness fix: skip (or hard-zero) any signal whose shifted band would land beyond
`±output_rate/2` — that's a per-signal scalar check, not per-sample work. Accept the softened
edge for signals straddling the window boundary and document it as a known approximation. If
edge fidelity ever matters on *narrow* DDCs, filter only there (low rate → cheap).

### 1.6 VITA-49 timestamps don't match the scenario time of the payload (streamer.c:353)
Requirement restated: the timestamp must fit the *scenario* content of the packet (a constant
lead/lag against wall clock is irrelevant). That requirement is currently violated, for a
different reason than wall-clock accuracy:

The render thread renders content for scenario time `render_time_ns`, which runs *ahead* of
`timebase_now_ns()` by however much of the 256-packet ring is filled. The UDP thread stamps
with `now` at send time. So a packet carrying samples rendered for scenario time `T` is stamped
roughly `T − ring_fill`, and the error is not constant:
- it varies with ring fill (startup, underruns, backpressure), and
- it differs *per stream*, because different sample rates mean different block durations and
  different pre-render depth — which breaks alignment between the synchronized channels, and
  makes two instances of the same configuration stamp the same content differently.

Concretely: a signal scheduled at `start_time_s` appears in the payload at scenario time `T`
but under a timestamp earlier than `T`, by a stream-dependent, run-dependent amount.

**Fix:** carry `render_time_ns` through the ring buffer alongside each payload (ring entries
become `{timestamp_ns, samples[]}`) and stamp packets from that. Zero runtime cost, and
together with grid-quantized block starts (see 1.1) it makes timestamps identical across
instances and consistent across channels.

### 1.7 Ring buffer depth creates large retune latency (streamer.c:20)
256 pre-rendered packets means a REST retune/config change only becomes visible on the wire
after the whole buffer drains — with typical block sizes that is seconds of stale spectrum.
Also `render_time_ns` may run far ahead of the timebase.

**Fix:** shrink the ring to a few packets (2–8 is enough to decouple render jitter from send
pacing), and/or flush the ring buffer when the receiver/DDC config changes (compare a config
generation counter in the render thread).

### 1.8 Noise floor power calibration is inconsistent and bandwidth-independent (renderer.c:362)
- The noise is uniform, and `amplitude` is used as the *peak*; RMS of uniform noise is
  `amp/√3`, so the produced noise power is ~4.8 dB below the configured `power_dbm`
  (signals, by contrast, are calibrated by amplitude).
- `power_dbm` is treated as total in-window power regardless of window bandwidth: an 80 MHz
  window and a 25 kHz DDC get the same total noise power, i.e. wildly different noise
  *densities*. Real receivers see constant dBm/Hz; a DDC should carry
  `power_dbm/Hz + 10·log10(bandwidth)`.

**Fix:** define noise as a density (`power_dbm_per_hz` in the schema), scale amplitude with
window bandwidth, and correct for the uniform-distribution RMS factor (or better, generate
Gaussian noise, e.g. sum of a few uniforms or Box–Muller — uniform noise has the wrong crest
factor for a noise floor).

### 1.9 AM envelope can clip before gain is applied (renderer.c:302)
For AM, `base_i = 1 + depth·audio` reaches 2.0, and `amplitude = 32767·source_gain`, so the
mixed sample reaches `2·32767·source_gain`. With `source_gain` near 1 this saturates via
`sim_clip_i16`, distorting the modulation peaks. Also, AM/SSB average power depends entirely on
the audio content, so the `power_dbm` scenario value doesn't correspond to measured power the
way it does for FM (constant modulus).

**Fix:** normalize per modulation: for AM scale by `1/(1+depth)`; for SSB normalize the audio
asset to a known RMS at load time (asset_cache) so `power_dbm` means the same thing everywhere.

### 1.10 FM keeps radiating a dead carrier past the end of the audio file
`audio_integral_at` clamps to the final integral value (renderer.c:267), so if a block spans the
end of the asset the WBFM signal continues as an unmodulated carrier for the rest of the block
(`iq_signal_active` only gates at block granularity). AM similarly emits a bare carrier
(`envelope = 1`). Fix by ending signal contribution per-sample when
`audio_position >= sample_count`.

## 2. Robustness / validation gaps

- **`scenario_validate` never uses `base_dir`** (scenario.c:226): asset paths are resolved
  relative to the process CWD, so the same scenario file works or fails depending on where the
  binary is launched. Join `source->file` with `base_dir` when relative.
- **Signal bandwidth is never validated** against the source: nothing stops
  `bandwidth_hz = 200 kHz` on a 48 kHz SSB signal, which silently skews `signal_passband_gain`.
  For WBFM you can sanity-check Carson's rule (`2·(deviation + audio_bw)`) against
  `bandwidth_hz`.
- **Assets are loaded twice**: `scenario_validate` fully loads each WAV (scenario.c:261) just to
  check the rate, then `asset_cache_load` loads it again. For large files this doubles startup
  time; validate header-only, or validate through the cache.
- **WAV reader** (wav_reader.c): only PCM16 is supported — `WAVE_FORMAT_EXTENSIBLE` (0xFFFE),
  which many editors emit for plain PCM, and float32 WAVs are rejected. Also a `data` chunk with
  `chunk_size = 0xFFFFFFFF` (streamed WAVs) will be misread. Worth at least clearer error text
  and extensible-format support.
- **Hilbert transformer radius 31** (asset_cache.c:15) gives roughly 30–40 dB opposite-sideband
  suppression only, and the O(N·31) precompute is fine but the short kernel limits SSB quality.
  Consider radius 63–127 (still cheap, done once at load).
- **`timebase_now_ns` wraps at UTC midnight** (timebase.c): `render_time_ns` in the stream
  worker keeps incrementing past the wrap while `now_ns` jumps to ~0, so the
  `render_time_ns < now_ns` catch-up clamp stops working for a while and day-scheduled signals
  glitch across midnight. Handle the wrap explicitly (detect `now << render_time` and reset).
- **Thread start failures leak silently** (streamer.c:425): if `pthread_create` for the UDP
  thread fails, `stream_worker_start` returns false and `streamer_manager_stop` joins, but the
  render thread of that worker is already running against a partially initialized worker — OK
  today, but fragile; also `stream_render_thread_main`/`stream_udp_thread_main` return silently
  on `calloc`/`udp_output_open` failure with no metric or log. Add an error metric/log so a dead
  stream is diagnosable.

## 3. Performance / architecture

- **Per-sample function-call mixing** (`mix_accumulate_sample`, clip on every accumulate):
  clipping each intermediate sum means the result depends on signal iteration order and loses
  headroom. Accumulate all signals into a float/int32 mix bus per block and clip once at the
  end — cheaper and order-independent.
- **Quarter-phase weights rebuilt every block** (renderer.c:496, 581): `build_quarter_phase_weights`
  is pure; compute once at startup (static init) instead of per block per signal.
- **One render + one UDP thread per stream** (`SIM_MAX_RECEIVERS · (1+SIM_DDC_COUNT)` × 2
  threads) all optionally pinned to the *same* CPU (`stream_cpu` shared by every worker,
  streamer.c:479/500). Pinning many busy threads to one core serializes them; either give each
  worker its own CPU from a set, or don't pin the render threads.
- **Ring buffer uses a mutex per packet** — fine at current rates, but a single-producer/
  single-consumer lock-free ring (atomic head/tail) would remove the lock entirely; the
  producer/consumer pattern here is exactly SPSC.
- **Noise + every signal rendered independently per window**: the 80 MHz stream and each DDC
  re-render the same signals. That's correct, but a cheaper long-term architecture is to render
  the wideband once and derive DDCs by channelizer (polyphase FFT filter bank) — also solves
  1.4/1.5 for free.

## 4. Smaller cleanups

- `nco.c` (`nco_init`, `nco_mix_ci16`) appears unused by the renderer — either use it as the
  shared, phase-continuous oscillator (fixes 1.1) or delete it.
- `receiver_center_frequency_hz` fallback scan rate of `1e11 Hz/s` when `scan_rate_hz_per_s <= 0`
  (receiver.c:27) is a surprising magic default; reject non-positive scan rates in
  `receiver_validate` instead.
- `resample_linear_ci16_f` accumulates `source_position` in float (renderer.c:480, 524); fine at
  current block sizes, but switch to `position = i * step` (double) to be safe if blocks grow.
- Error strings like `"source_invalid"` don't say *which* source/field failed — include the
  index/id (`"source[2] radio1: missing sample_rate_hz"`); scenario debugging is much faster.
- `scenario.c` accepts `am_depth == 0` and `fm_deviation_hz` only checked for WBFM; harmless,
  but consider warning on depth 0 (produces a bare carrier).

## 5. High-rate performance (96 MS/s, multiple synchronized channels)

Budget check: 96 MS/s of ci16 is 384 MB/s of payload per receiver on the wire, and the render
loop has ~10.4 ns per output sample per signal. Anything per-sample that isn't a handful of
fused multiply-adds is too slow. Suggestions, roughly in order of expected payoff:

### Hot-loop / DSP
- **Mix into a wide accumulator, clip once.** Today every signal accumulation does a
  double-precision multiply, add, and `lrint`-based clip *per sample per signal*
  (`mix_accumulate_sample`). Render each block into a `float` (or `int32`) mix bus, accumulate
  all signals and noise there, and convert+clip to ci16 in one final SIMD-friendly pass. This
  is the single biggest hot-loop win, removes the order-dependence of intermediate clipping,
  and makes the whole path vectorizable.
- **Move the wideband path to float32 SIMD end-to-end.** The scalar paths mix in double with
  `cos`/`sin` recursions per signal. In float with VOLK this is one `volk_32fc_s32fc_x2_rotator2_32fc`
  (already used in `render_direct_nco`) plus one `volk_32fc_x2_multiply_32fc`/accumulate per
  signal. Extend the VOLK path to the resampled cases and to the final float→ci16 conversion
  (`volk_32f_s32f_convert_16i`). Avoid the per-block `volk_malloc`/`volk_free` in
  `render_direct_nco` (renderer.c:441) — allocate the scratch buffers once per worker.
- **Precompute everything pure at startup:** quarter-phase resampler weights are rebuilt per
  block per signal (renderer.c:496, 581); NCO phase increments per block are fine, but tables/
  kernels should be computed once.
- **Noise generation is 2 xorshift calls + scaling per sample across an 80 MHz block** — that's
  ~200M PRNG calls/s per receiver. Options: generate noise in float with a vectorized PRNG
  (xoshiro SIMD, or VOLK + a precomputed noise pool that is re-seeded/permuted per block), or
  precompute a large noise ring (e.g. 1–4 M samples) at startup and enter it at a
  seed+time-derived offset — deterministic-enough, near-zero cost, and the spectrum stays white.
- **Parallelize the wideband render across cores per block** (split the block into slices, one
  thread each) if one core can't hold 96 MS/s with several signals. Signals are independent and
  the mix bus makes slicing trivial. Do this *inside* the render step so block timing and
  determinism are unaffected.

### Threading / scheduling
- **Fix the CPU pinning:** every worker (2 threads per stream, per receiver, per DDC) is pinned
  to the *same* `stream_cpu` (streamer.c:479/500) — that serializes all streams on one core.
  Give the 80 MHz render thread its own core (or cores), group DDC workers on others, and keep
  UDP send threads separate from render threads.
- **SPSC lock-free ring buffer:** each ring has exactly one producer and one consumer; replace
  the mutex with atomic head/tail indices (acquire/release). At 96 MS/s the mutex is contended
  every ~85 µs per stream.
- Consider `SCHED_FIFO`/`SCHED_RR` for the wideband send thread and locking asset memory with
  `mlock`/huge pages (`madvise(MADV_HUGEPAGE)`) so the 96 MS/s path never page-faults.

### Network path (384 MB/s per receiver)
- Already batching with `sendmmsg` — good. Next steps, in order:
  - **UDP GSO (`UDP_SEGMENT`)**: hand the kernel one large buffer per batch and let it split
    into datagrams; typically 2–3× fewer syscalls/CPU than `sendmmsg` at these rates.
  - Raise `SO_SNDBUF` explicitly and verify against `net.core.wmem_max`.
  - If the kernel stack tops out: **AF_XDP** (via libxdp) gives near-line-rate UDP without the
    full DPDK operational burden. DPDK only if this must saturate >10 GbE reliably.
- Check MTU: at 96 MS/s use jumbo frames (9000 MTU) if the network allows — 4× fewer packets.

### External libraries worth adopting (all C-friendly, permissive licenses)
- **VOLK** (already optional): make it a hard dependency for the wideband build; it covers the
  rotator, multiply-accumulate, and type-conversion kernels needed above.
- **liquid-dsp** (already optional): use for DDC decimation chains (half-band cascades,
  `msresamp_crcf`) at low rates only.
- **FFTW3 or KFR**: only if you later adopt a channelizer (§3) — a polyphase FFT channelizer
  produces *all* DDCs from the wideband signal in one pass, which is the right architecture if
  DDC count grows, and it keeps channels sample-synchronous by construction.
- **xoshiro/SplitMix SIMD PRNG** (public domain, single header) for vectorized noise.
- **libxdp/AF_XDP** if the UDP path becomes the bottleneck.
- Build flags: ensure the render TU is built with `-O3 -march=<target>` (or a function-level
  `target` attribute); avoid global `-ffast-math`, but `-fno-math-errno` is safe and lets the
  compiler inline `lrintf`/`sqrtf`.

## 6. Suggested priority order

1. Phase continuity + grid-quantized block starts (1.1, 1.2) — audible in every demodulated
   signal, and prerequisite for cross-instance determinism.
2. VITA timestamp carried from render time + smaller/flushable ring (1.6, 1.7) — required for
   channel synchronization and reproducible timestamps across instances.
3. Mix-bus + float/VOLK hot loop + pinning fix + SPSC ring (§5) — headroom for 96 MS/s with
   multiple channels; do this before adding any DSP quality work.
4. Cheap anti-alias fixes for DDC rendering (1.4, 1.5 — kernel cutoff scaling / half-band on
   low-rate paths only).
5. Noise as density + pooled/vectorized noise generation (1.8, §5) — SNR correctness and a big
   wideband CPU win in one change.
6. Power normalization for AM/SSB (1.9) and end-of-asset gating (1.10).
7. Validation & robustness items in §2, network-path scaling in §5 as data rates demand.
