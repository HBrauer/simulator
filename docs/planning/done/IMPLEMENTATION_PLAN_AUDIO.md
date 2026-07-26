# Implementation Plan — Audio Pre-Rendering & Resampler Generalization

Goal: make audio-modulated signals (WBFM/AM/USB/LSB) as cheap to render as IQ signals, so a
full receiver (wideband 98.304 MS/s + 4 DDCs) with mixed IQ + audio + noise sustains real time
with ≥30% headroom on one core. Follow-up to `IMPLEMENTATION_PLAN.md` (all A–D tasks landed);
resolves the R1 escalation recorded in `PERF_NOTES.md`.

User decision that shapes this plan: **pre-computing IQ from audio is acceptable.** We therefore
modulate audio into complex-baseband IQ **once at startup** and let the existing (fast) IQ path
render it, instead of streaming low-rate synthesis per block.

---

## 1. Executive Summary

### The problem (measured)
`render_audio_modulated` synthesizes the modulated carrier **per output sample at the full
output rate**: for every one of 98.304 M samples/s it does an interpolated table lookup, a
`sincos` (WBFM), and a complex rotation — per signal. A `sincos` alone (~20–40 ns) exceeds the
~10 ns/sample budget. Result: 0.14× real time for the audio demo scenario vs 4.49× for IQ.

### The fix (two parts, both required)
1. **Pre-render audio → IQ at startup (the headline change).** For each signal that references
   an audio source, synthesize its complex baseband at a low intermediate rate matched to the
   signal bandwidth (e.g. ~400 kHz for a 200 kHz WBFM station instead of 98.304 MHz — ~250×
   fewer transcendental evaluations, done once, off the streaming path). Store as ci16 in the
   asset cache. The hot path then treats audio signals exactly like IQ signals.
2. **Make the general-ratio resampler actually fast (the general improvement).** Today only the
   hardcoded ratio-4 (quarter) path uses precomputed kernels; every other ratio computes
   `sinc()`+`hann()` per tap per output sample (~16 transcendentals/sample — nearly as bad as
   the audio path!), and ratios ≤ 1/8 drop to linear interpolation (poor image rejection,
   ~−24 dB). A pre-rendered 400 kHz asset upsampled to 98.304 MHz (ratio ~1/246) would hit
   exactly these paths. Replace them with a **precomputed polyphase kernel table** (N phases ×
   8 taps) + a **float scratch → VOLK rotate → accumulate** pipeline. This also speeds up *any*
   non-quarter-ratio IQ source, not just pre-rendered audio.

### Why this preserves everything already built
- **Determinism:** pre-rendering is a pure function of (audio samples, modulation params);
  identical across instances. Block rendering then reuses the A1–A4 machinery (grid times,
  Q0.64 phase, fractional offsets) unchanged — the phase-continuity and determinism tests
  keep passing by construction.
- **Calibration:** FM is constant-modulus (exact at full scale); AM stays normalized by
  `1/(1+depth)`; SSB is peak-normalized into ci16 with the scale factor recorded and folded
  into `source_gain`, so `power_dbm` keeps meaning what it means today.
- **Windows/DDCs:** the pre-rendered buffer is baseband around the signal center, independent
  of any window — wideband and every DDC consume the same buffer.

### Expected outcome
Per-signal hot-path cost drops to the IQ-path level (8 MACs resample + vectorized rotate +
accumulate ≈ few ns/sample). Target: `benchmark_load` (2 IQ + WBFM + AM + noise, wideband +
4 DDCs) ≥ **1.3× real time on one core** (currently 0.07×).

---

## 2. Requirements

### Functional
| ID | Requirement |
|---|---|
| AF1 | At asset-cache load, every signal referencing an `audio_file` source gets a pre-rendered complex-baseband ci16 buffer at an intermediate rate derived from the signal's bandwidth. Modulation math (FM integral phase, AM envelope with `/(1+depth)`, SSB via Hilbert) is reused **verbatim at the low rate** — same formulas, same normalization semantics as today. |
| AF2 | Pre-rendered buffers are keyed **per signal** (modulation, `fm_deviation_hz`, `am_depth` are per-signal; two signals may share one WAV with different params). |
| AF3 | Intermediate rate: `prerender_rate = oversample × content_bw`, where `content_bw` = Carson (`2·(deviation + audio_rate/2)`) for WBFM, `2 × audio_rate/2` for AM (DSB), `audio_rate/2` for SSB — clamped to at least the declared `signal.bandwidth_hz` and to a configurable ceiling. `oversample` default **2.0** (configurable, see §5). Rate rounded up so the value is exact/deterministic. |
| AF4 | ci16 storage with **peak normalization**: scan the synthesized buffer, scale so max |I|,|Q| ≤ 32767, record `prerender_gain` in the cached entry; renderer folds `1/prerender_gain` … actually the recorded scale … into `source_gain` so output amplitude is bit-for-bit calibrated (FM: scale is exactly 32767; AM: ≤ 32767 by construction; SSB: data-dependent). |
| AF5 | The renderer routes audio signals through the standard IQ dispatch using the pre-rendered buffer, rate, and count. `render_audio_modulated` is **removed from the hot path** (retained only as the reference implementation for parity tests — see AT2). |
| AF6 | End-of-asset gating, repeat playback (`iq_signal_active`), start fraction (A4), and mixer phase (A3) work unchanged, because the pre-rendered signal *is* an IQ source. Duration in seconds is preserved: `prerender_count = round(audio_frames × prerender_rate / audio_rate)`. |
| AF7 | Pre-render memory is counted against `asset_cache_max_bytes` (existing limit machinery, existing `asset_cache_limit_exceeded` error). One startup log line per pre-rendered signal: signal id, rate, sample count, MB, synth time. |

### Resampler generalization (the "in general" part)
| ID | Requirement |
|---|---|
| AR1 | Polyphase kernel table: for a given (ratio ⇒ cutoff), precompute `N_PHASES × RESAMPLER_TAPS` weights (N_PHASES = 64; positional error ≤ 1/128 sample — negligible at 8 taps). Built once per distinct ratio at worker/scenario setup (small cache; ratios are enumerable: sources × stream rates). Replaces per-sample `sinc()`/`hann()` in the general path. The hardcoded quarter-rate special case becomes just another table entry. |
| AR2 | The `render_resampled_nco` pipeline becomes: polyphase-resample the block into a float scratch buffer, then one VOLK rotator pass (`volk_32fc_s32fc_x2_rotator2_32fc`, initial phase from A3's Q0.64 machinery), then accumulate into the mix bus. Scalar fallback preserved for `-Dvolk_accel=disabled`; parity within ±2 LSB. |
| AR3 | The low-rate **linear** shortcut (ratio ≤ 1/8) is **not used for pre-rendered audio** (image rejection too poor: linear = sinc² ≈ −24 dB at 2× oversampling). Pre-rendered assets always go through the polyphase path. Plain IQ-file sources keep the linear path for now (behavior + existing exact-value tests unchanged); flag as a candidate to revisit once AR1 lands (the polyphase path may simply be fast enough to delete the linear one — decide by benchmark). |

### Performance
| ID | Requirement |
|---|---|
| AP1 | `benchmark_load` wideband + 4 DDCs on one core: **≥ 1.3× real time** (release + native build). Record before/after in `PERF_NOTES.md`. |
| AP2 | Startup pre-render budget: ≤ 2 s for the demo scenarios (measured; ~6 M `sincos` for a 15 s clip at 400 kHz ≈ 0.2 s — ample margin). |
| AP3 | No allocation, locking, or table construction on the per-block hot path (tables and scratch built at worker start). |

### Quality / determinism
| ID | Requirement |
|---|---|
| AQ1 | WBFM end-to-end fidelity: render a pre-rendered FM tone through the wideband path, FM-demodulate in the test, recover the tone with SNR ≥ 40 dB and no block-rate artifacts. |
| AQ2 | SSB opposite-sideband rejection ≥ 45 dB and AM envelope accuracy preserved (existing B4-level tests keep passing against the new path). |
| AQ3 | ci16 quantization of the pre-render adds noise ≈ −90 dBFS — accept and document (far below the scenario noise floor). |
| AQ4 | Two renders of the same block (and two instances) produce byte-identical output — existing determinism tests extended to a pre-rendered audio signal. |

### Edge cases
- Signal `bandwidth_hz` absurdly small vs content (e.g. WBFM declared 1 kHz): rate clamps to the
  content-based minimum; keep the existing Carson warning.
- Very long WAV × high rate exceeding `asset_cache_max_bytes`: hard error at startup with the
  existing message + the new per-signal log making the culprit obvious.
- Same WAV referenced by N signals: N pre-renders (correct — params differ); if params are
  identical, a dedupe key (source_id + modulation + deviation + depth + rate) shares the buffer
  (nice-to-have, do only if trivial).
- Prerender of a zero-length or silent asset: zero-length buffer → signal renders silence
  (matches current end-of-asset behavior); silent SSB peak-normalization must not divide by
  zero (keep gain 1.0, as `normalize_audio_rms` already does).

---

## 3. Options considered (and why pre-render wins)

| | A: Pre-render at startup (chosen) | B: Streaming low-rate synthesis per block |
|---|---|---|
| Hot-path cost | identical to IQ path | low, but adds a per-block synthesis stage + state |
| Complexity | modulation code runs once, single-pass, no cross-block state | must carry FM phase/Hilbert state across blocks deterministically, per stream |
| Determinism | trivial (pure function at load) | achievable but adds a second grid/phase mechanism to keep correct |
| Memory | duration × rate × 4 B per signal (see table below) | negligible |
| Startup | +0.1–2 s | none |
| Long/streamed audio | bounded by RAM | unbounded |

Memory examples (ci16, complex): 15 s WBFM @ 400 kHz ≈ **24 MB**; 15 s AM @ 96 kHz ≈ 5.8 MB;
15 s SSB @ 48 kHz ≈ 2.9 MB. A 10-minute WBFM clip ≈ 960 MB — that is the real limit of option A
and why AF7 counts it against `asset_cache_max_bytes` and logs it. If someone later needs
hour-long audio, option B can be added behind the same cached-asset interface without touching
the renderer again — note this in the code where the prerender is produced.

---

## 4. Current-State Notes (verified against the code)

- `render_audio_modulated` (renderer.c ~line 300): the per-output-sample synthesis loop to be
  removed from the hot path.
- `resample_sinc_ci16` / `resample_liquid_ci16`: compute `sinc_value()` (a `sin()`) and
  `hann_window()` (a `cos()`) **per tap per output sample** — 8 taps ⇒ ~16 transcendentals per
  sample. Only ratio 0.25 avoids this via `g_quarter_weights` (pthread_once). AR1 replaces this.
- `LOW_RATE_LINEAR_MAX_SOURCE_PER_OUTPUT 0.125`: ratios ≤ 1/8 use linear interpolation
  (`resample_linear_ci16_f`). A 400 kHz prerender → 98.304 MHz is ratio ~0.004 ⇒ would hit this;
  AR3 routes prerendered assets around it. Test
  `renderer_low_rate_upsample_uses_linear_path` asserts exact linear values for an IQ file —
  leave that behavior alone.
- `cached_asset_t` (asset_cache.h): keyed by `source_id`, already has `normalization_gain`.
  Pre-rendered entries need a **new per-signal array** (signals × modulation params), see §6 T3.
- `renderer_render_window_block` dispatch: `source->source_kind == SCENARIO_SOURCE_AUDIO_FILE`
  branches to the audio path; this is the switch point (T4).
- A3/A4 already provide grid-derived `start_sample`, Q0.64 phase, and fractional source offsets
  for the IQ path — pre-rendered signals inherit all of it.
- Unit tests that hand-build audio cache entries (`renderer_renders_audio_modulation_modes`,
  `renderer_am_envelope_is_normalised…`, `renderer_audio_stops_at_end_of_asset…`,
  `setup_dc_audio_signal`) construct `audio_samples`/`audio_hilbert`/`audio_integral` directly —
  they must be reworked to build (or invoke) pre-rendered entries (T5).

---

## 5. Data Model / Config Changes

- **No scenario JSON changes.** Modulation stays declarative; pre-rendering is an internal
  implementation detail.
- Instance YAML (optional new keys, defaults chosen so nothing must be set):
  - `audio_prerender_oversample` (float, default `2.0`, min 1.25): oversampling factor over the
    content bandwidth. Higher = better image margin, more memory.
  - `audio_prerender_max_rate_hz` (int, default `4000000`): safety ceiling on the intermediate
    rate.
- `cached_asset_t` gains nothing; a new `cached_prerender_t { signal_id, sample_rate_hz,
  sample_count, samples (ci16*), gain }` array lives in `asset_cache_t` (sized `SIM_MAX_SIGNALS`),
  plus `asset_cache_find_prerender(cache, signal_id)`.
- `audio_samples`/`audio_hilbert`/`audio_integral` in `cached_asset_t` become inputs to the
  pre-render only; after load they can be **freed** (memory win) — unless tests need them; free
  them and let tests build their own (decide in T5; prefer freeing).

---

## 6. Detailed Tasks (ordered; build + full test suite after each; commit each)

**T1 — Polyphase kernel table (AR1).**
- Files: `renderer.c/.h`.
- Add `resampler_table_t { float w[64][RESAMPLER_TAPS]; }` built from the existing
  `sinc(cutoff·d)·hann(d)` formula per phase, normalized per phase (same math as
  `build_quarter_phase_weights`, generalized to 64 phases and arbitrary cutoff). Small
  fixed-size cache keyed by quantized ratio, built via `pthread_once`-style init or at
  `stream_worker_start`; the render call selects `phase = round(frac·64) & 63`.
- Replace the general `resample_ci16` per-sample computation in `render_resampled_baseband` /
  `render_resampled_nco` with table lookups; keep the exact per-position path only for the
  first/last `RESAMPLER_RADIUS` samples at buffer edges (as the quarter path does today).
- Delete the now-redundant quarter-rate special case (it is table ratio 0.25).
- Acceptance: existing resampler tests pass (tolerances ≤ 2 LSB where phase quantization
  shifts values — investigate anything larger); alias-suppression test still ≥ 25 dB;
  benchmark shows the general-ratio path no longer ~10× slower than quarter-rate.

**T2 — Float-scratch + VOLK pipeline for resampled NCO paths (AR2).**
- Files: `renderer.c`.
- `render_resampled_nco`: polyphase-resample the whole block into `_Alignas(64)` float-complex
  scratch (stack, capped at `SIM_MAX_STREAM_BLOCK_SAMPLES` like C2), one VOLK rotator pass with
  initial phase from `phase0`, then accumulate to the bus. Scalar fallback = current loop.
- Acceptance: VOLK vs scalar parity ±2 LSB (run full suite with `-Dvolk_accel=disabled`);
  phase-continuity test still passes on a resampled-ratio signal.

**T3 — Pre-render engine (AF1–AF4, AF7).**
- Files: `asset_cache.c/.h`, `config.c/.h` (new keys), `sim_types.h` (config fields).
- After loading audio sources, iterate `scenario->signals`; for each audio-referencing signal:
  compute `prerender_rate` (AF3, exact integer), synthesize `prerender_count` complex samples
  by evaluating the **existing** modulation formulas at `audio_position = i · audio_rate /
  prerender_rate` (reuse `audio_integral_at`/`audio_linear_at`/Hilbert data), peak-normalize
  into ci16 recording `gain`, account memory, log one line.
- Free `audio_samples`/`audio_hilbert`/`audio_integral` after all signals are pre-rendered.
- Acceptance: unit tests — rate selection per modulation (WBFM Carson-based, AM, SSB); gain
  recorded such that reconstructed amplitude matches the direct formula within 1 LSB at a few
  probe points; memory limit enforcement (`asset_cache_load_limited` with a small cap fails
  cleanly); determinism (two loads → memcmp-equal buffers).

**T4 — Renderer switchover (AF5, AF6, AR3).**
- Files: `renderer.c`, `asset_cache.h`.
- In `renderer_render_window_block`: for `SCENARIO_SOURCE_AUDIO_FILE`, look up the signal's
  prerender entry and fall through to the **IQ dispatch** with `source_rate = prerender_rate`,
  `samples = prerender buffer`, `source_gain *= recorded gain factor`; force the polyphase path
  (never `low_rate_linear`) for prerendered assets via a flag on the entry.
- Move `render_audio_modulated` (and its helpers `audio_integral_at`, `audio_linear_at`) out of
  the hot path: either `#ifdef SIM_TESTING` or relocate the reference implementation into the
  test suite for T5's parity test. `iq_signal_active` duration math now uses the prerender
  rate/count for audio signals (duration in seconds is identical by AF6).
- Acceptance: `renders_burst…`, audio-mode smoke tests pass through the new path; the audio
  end-of-asset and AM-normalization tests pass unchanged in expectation (values may shift
  ≤ 2 LSB from resampling — inspect, don't blindly widen).

**T5 — Test rework + quality gates (AQ1–AQ4, AT2).**
- Rework the hand-built audio cache entries in `test_renderer.c` to construct prerender entries
  (small helper that calls the T3 engine on synthetic audio).
- **Parity test (AT2):** for each modulation, render one block with the retained reference
  implementation (full-rate synthesis) and with the prerender+resample path; assert spectra
  match: tone frequency exact, level within 0.5 dB, spurs ≤ −40 dBc.
- **Demod test (AQ1):** 1 kHz tone WAV → WBFM prerender → wideband render across ≥ 16
  consecutive grid blocks → FM demodulate in the test → tone SNR ≥ 40 dB, no spectral line at
  the block rate.
- Extend the two-instance/two-render determinism test to include a prerendered audio signal.
- Acceptance: all listed tests green + full suite + sanitizers (ASan/UBSan, TSan) clean.

**T6 — Benchmark + docs (AP1, AP2).**
- Re-run `benchmark_load` (wideband + DDCs, one core, release+native): require ≥ 1.3×; also
  re-measure `audio_radio_demo`. Update `PERF_NOTES.md` (resolve the R1 escalation with the
  new numbers), README (memory note for pre-rendering, new config keys), and the renderer
  header comment.
- If < 1.3×: profile; the designed fallback is intra-block thread slicing (mix bus makes it
  safe) — but per the budget math (8 MACs + vectorized rotate ≈ IQ path, which measures 4.49×
  for one signal), the target should be met without it.

**T7 (optional, only if T1 makes it free) — retire the linear low-rate path** for IQ files too,
updating `renderer_low_rate_upsample_uses_linear_path` to assert polyphase values. Decide by
benchmark; skip if any regression risk.

---

## 7. Testing Plan Summary

1. Unit: polyphase table vs analytic reference; VOLK/scalar parity; prerender rate/gain/
   determinism/memory-cap; audio-path parity vs reference synthesis; end-of-asset & AM
   normalization through the new path; phase continuity on a prerendered signal.
2. End-to-end: FM demod SNR ≥ 40 dB with no block-rate line (AQ1) — this is the test that
   proves the whole point of the original effort survives the rearchitecture.
3. Perf gates: `benchmark_load` ≥ 1.3× single core; startup prerender ≤ 2 s for demos.
4. Regression: full unit + integration suites, VOLK and scalar builds, ASan/UBSan + TSan.
5. Determinism: render-once sha256 (README) re-pinned after the change (values will shift —
   once, deliberately, documented in the commit).

## 8. Risks

- **R-A1 Memory** for long clips (dominant real risk). Mitigated by AF7 accounting + logging +
  the configurable rate ceiling; documented option-B escape hatch for streaming synthesis.
- **R-A2 Quality regression** vs direct synthesis (resampler images, phase quantization of the
  64-phase table, ci16 quantization). Mitigated by AT2 parity spectra + AQ1 demod SNR gate;
  oversample factor is the tuning knob.
- **R-A3 Test churn**: audio unit tests build cache internals by hand; T5 reworks them against
  the engine instead — treat any needed tolerance > 2 LSB as a bug, not a tolerance update.
- **R-A4 Startup time** if someone loads many long clips: linear in total prerendered samples;
  the per-signal log makes it visible; acceptable for a simulator (and parallelizable later if
  ever needed).

## 9. Open questions (flag in PR, proceed with defaults)

- `oversample` default 2.0 vs 4.0 (quality vs memory) — start at 2.0; AT2/AQ1 gates will tell.
- Keep the reference synthesis under a test-only build flag vs moving it into test code —
  prefer moving to test code (keeps the production TU clean).
- Dedupe identical (source, params) prerenders — only if it falls out naturally in T3.
