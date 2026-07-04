# Implementation Plan — Simulator Improvements

Source document: `IMPROVEMENTS.md` (review of `simulator/src`).
Audience: an AI developer implementing the changes without access to the original review discussion.
Build system: Meson (`meson.build` at repo root). Unit tests: `simulator/tests/unit/` (run via
`meson test -C build unit` or equivalent; a benchmark target `renderer_benchmark` exists).
Ignore all `build*` directories.

---

## 1. Executive Summary

**Goal:** make the simulator's output (a) *phase- and time-continuous* so demodulated audio and
digital signals are artifact-free, (b) *deterministic and channel-synchronous* — two instances
started with the same configuration must produce the same samples with the same VITA-49
timestamps (only noise may differ), and (c) *fast enough* to sustain 96 MS/s wideband output
plus several DDC channels simultaneously.

**Hard constraints (apply to every task):**
- The 96 MS/s wideband render path may not gain per-sample work beyond a few fused
  multiply-adds. Long FIR filters and per-sample `cos`/`sin` are forbidden on that path.
- Everything (block boundaries, mixer phase, noise seed, timestamps) must derive from
  *scenario time*, never from "when did this thread happen to run".
- Anything expensive but pure must be precomputed at startup.

**Main areas that change:**
| Area | Files |
|---|---|
| Render core (phase continuity, mix bus, resampler, noise) | `simulator/src/renderer.c/.h` |
| Signal timing (fractional offsets) | `simulator/src/iq_file_reader.c/.h` |
| Stream workers (block grid, ring payload+timestamp, pinning, SPSC ring) | `simulator/src/streamer.c`, `simulator/src/ringbuffer.c/.h` |
| Scenario schema & validation (noise density, base_dir, per-field errors) | `simulator/src/scenario.c/.h`, `docs/schemas.md` |
| Asset loading (normalization, single load, Hilbert) | `simulator/src/asset_cache.c/.h`, `simulator/src/wav_reader.c` |
| Timebase (midnight wrap) | `simulator/src/timebase.c` |
| Receiver validation (scan rate) | `simulator/src/receiver.c` |
| Build (VOLK hard dep for wideband, flags) | `meson.build`, `meson_options.txt` |
| Tests | `simulator/tests/unit/*`, `simulator/tests/benchmarks/renderer_benchmark.c` |

---

## 2. Requirement Extraction

### 2.1 Functional requirements (correctness)

| ID | Requirement (actionable form) | Source |
|---|---|---|
| F1 | The frequency-shift oscillator phase of every signal must be a pure function of absolute scenario time: `phase(t) = 2π·offset_hz·t`, continuous across block boundaries. No per-block reset to phase 0. | 1.1 |
| F2 | Render block start times must be quantized to a fixed grid: `block_index = floor(scenario_time / block_duration)`, `render_time = block_index · block_duration`. All instances of the same config render identical blocks. | 1.1 note |
| F3 | `iq_signal_active` must return the *fractional* source-sample offset in addition to the integer offset; the renderer must use it as the resampler start position so playback has no per-block timing jitter. | 1.2 |
| F4 | Recursive sin/cos rotators must be renormalized periodically (every ≤512 iterations: divide by `sqrt(c²+s²)`), in all scalar float and double paths. | 1.3 |
| F5 | When decimating (`source_per_output > 1`), the resampler kernel cutoff must be scaled to `1/source_per_output`. Kernels are precomputed at startup, tap count unchanged (8) on the wideband path. Optionally, DDC-only half-band cascades / liquid-dsp `msresamp_crcf` on low-rate paths. | 1.4 |
| F6 | A signal whose *shifted* band lies entirely beyond `±output_rate/2` must be skipped (scalar check per signal per block). Partial-straddle attenuation approximation stays and is documented. | 1.5 |
| F7 | Each ring-buffer entry must carry the scenario render timestamp of its payload; VITA-49 packets are stamped from that, never from send-time `now`. | 1.6 |
| F8 | Ring depth reduced from 256 to a small value (default 8 packets, configurable constant), and the ring is flushed when the effective receiver/DDC config changes (config generation counter). | 1.7 |
| F9 | Noise floor is specified as a density (`power_dbm_per_hz`); rendered noise total power = density + 10·log10(window_bandwidth). Amplitude corrected for the distribution's RMS (uniform: ÷√3) or noise made Gaussian-ish. Keep backward compat for old `power_dbm` (see §7). | 1.8 |
| F10 | AM output normalized by `1/(1+am_depth)` so peaks never exceed the nominal amplitude. Audio assets normalized to a known RMS at load so `power_dbm` is content-independent for AM/SSB. | 1.9 |
| F11 | Audio-modulated signals must stop contributing per-sample when `audio_position >= sample_count` (no dead FM/AM carrier after the file ends within a block). | 1.10 |
| F12 | `scenario_validate` must resolve relative asset paths against `base_dir`. | §2 |
| F13 | Validation: reject/flag signal `bandwidth_hz` inconsistent with the source (SSB/AM vs audio bandwidth; WBFM vs Carson's rule) — warning-level is acceptable, see A5. | §2 |
| F14 | Assets loaded exactly once at startup (validation must not fully load WAV/IQ files that the cache loads again). | §2 |
| F15 | WAV reader: accept `WAVE_FORMAT_EXTENSIBLE` (0xFFFE) wrapping PCM16; produce clear errors otherwise; treat `data` size `0xFFFFFFFF` as "read to EOF". | §2 |
| F16 | Hilbert kernel radius increased from 31 to 63–127 (startup-only cost). | §2 |
| F17 | Timebase midnight wrap handled: when `now_ns` wraps below `render_time_ns` by ~a day, the stream worker resets its block clock instead of free-running. | §2 |
| F18 | Dead-stream diagnosability: thread-startup/alloc/socket failures increment an error metric and/or log; no silent `return NULL`. | §2 |
| F19 | `receiver_validate` rejects `scan_rate_hz_per_s <= 0` in scan mode; the `1e11` magic fallback in `receiver_center_frequency_hz` is removed. | §4 |
| F20 | Scenario parse errors identify the failing element and field (e.g. `source[2] id=radio1: missing sample_rate_hz`). | §4 |
| F21 | `nco.c` either becomes the shared phase-from-time oscillator helper or is deleted (no dead code). | §4 |

### 2.2 Performance requirements

| ID | Requirement | Source |
|---|---|---|
| P1 | Per-block rendering accumulates into a per-worker float32 mix bus; conversion+clip to ci16 happens once per block in a vectorizable pass. Intermediate clipping removed. | §5 |
| P2 | VOLK float path extended: rotator for all NCO mixes, multiply/accumulate, `volk_32f_s32f_convert_16i` for the final conversion. Scratch buffers allocated once per worker, not per block (`volk_malloc` hoisted out of `render_direct_nco`). | §5 |
| P3 | All pure tables (quarter-phase weights, decimation kernels) computed once at startup, not per block. | §5 |
| P4 | Noise generation cost reduced: precomputed noise pool (1–4 M float samples) generated at startup; each block reads at a deterministic offset derived from `(seed, window, block_index)`. | §5 |
| P5 | CPU pinning fixed: workers distributed over a CPU *set*, wideband render thread on its own core; UDP send threads not sharing render cores. | §5 |
| P6 | Ring buffer converted to lock-free SPSC (atomic head/tail with acquire/release). | §5 |
| P7 | Optional (flagged): `SCHED_FIFO` for wideband send thread; `mlock`/huge pages for asset cache; UDP GSO (`UDP_SEGMENT`); `SO_SNDBUF` raised; jumbo-frame awareness. | §5 |
| P8 | Build: render TU compiled with `-O3` and `-fno-math-errno`; VOLK becomes required (or strongly defaulted) for the wideband build. No global `-ffast-math`. | §5 |
| P9 | Benchmark target proves: 1 receiver at 96 MS/s + all DDCs, with ≥4 active signals + noise, renders faster than real time with ≥30% headroom on the target machine. | §5 intro |

### 2.3 Determinism requirements (cross-cutting)

| ID | Requirement |
|---|---|
| D1 | Two processes started with identical config/scenario produce byte-identical payloads per (stream, block_index), except noise which may differ but must be statistically identical. (In fact with P4+F2 the noise is also identical — acceptable and preferred.) |
| D2 | VITA-49 timestamps for a given (stream, block_index) are identical across instances and equal to the scenario time of the first sample in the payload. |
| D3 | All channels of one instance are sample-synchronous: blocks with equal timestamps contain samples for the same scenario time span. |

### 2.4 Edge cases to handle explicitly

- Block spanning the end of an audio/IQ asset (F11; IQ path already zero-fills — keep).
- Block spanning a repeat boundary of a signal (`occurrence_elapsed` wraps mid-block): acceptable to truncate to silence until the next block (current behavior), but must be deterministic — document it.
- Scenario time wrap at 86 400 s (midnight): F17; also `sample_index_from_time_ns` and block grid must stay consistent across the wrap (block grid restarts at index 0 — acceptable, document).
- `offset_hz` exactly at ±output_rate/2 (F6 boundary: use strict `>` for skip).
- Stream disabled → re-enabled: block grid means re-enable snaps to the grid (no arbitrary phase); ring flushed.
- Ring full / underrun paths must still update metrics exactly as today (tests exist in `test_streamer.c`).
- Zero-signal scenario: mix bus of pure noise still deterministic.
- `sample_rate_hz` not dividing 1e9 evenly (e.g. 96 MS/s): block grid arithmetic must use integer math on sample counts, not accumulated ns (see Task 2 guidance).

---

## 3. Assumptions and Clarifications

Marked **[ASSUMPTION]** = implementer may proceed, but flag in the PR for user confirmation.

- **A1 [ASSUMPTION]** Target platform is Linux x86_64 with AVX2-class SIMD; `-march=x86-64-v3` for the render TU is acceptable. If binaries must run on older CPUs, use function multi-versioning instead.
- **A2 [ASSUMPTION]** "Same timestamps across instances" refers to the VITA-49 timestamp field (scenario time of day), and both instances run against the same wall clock (NTP-synced). No cross-host PTP requirement.
- **A3 [ASSUMPTION]** Identical noise across instances (deterministic pool + time-derived offset) is acceptable and even desirable ("different noise is fine" = permission, not requirement).
- **A4 [ASSUMPTION]** Breaking the scenario JSON schema is acceptable if a backward-compatible fallback is kept (see §7: `power_dbm` still accepted, reinterpreted). `schema_version` should bump.
- **A5 [ASSUMPTION]** F13 bandwidth-consistency checks are *warnings* (stderr log), not hard validation failures — existing scenarios must keep loading. Hard-fail only on the already-hard-failing cases.
- **A6 [ASSUMPTION]** `SIM_DDC_COUNT`, `SIM_MAX_RECEIVERS`, block size defaults come from `config.h` / `sim_types.h`; the implementer must read those before starting (values not restated here to avoid drift).
- **A7 [ASSUMPTION]** GSO/AF_XDP/`SCHED_FIFO` (P7) are *optional stretch tasks*, off by default, behind config/meson options. Core deliverable is P1–P6, P8, P9.
- **A8** Unclear in the source doc: whether the audio-asset RMS normalization (F10) should target a specific RMS. **Decision:** normalize audio assets to RMS = 1/√2 (−3 dBFS sine-equivalent) at load; scale factor stored in the cached asset for debugging. Flag for confirmation.
- **A9** Conflict in the source doc: §3 suggests a polyphase FFT channelizer as long-term architecture, while the constraints forbid heavy DSP. **Resolution:** channelizer is explicitly OUT of scope for this plan; F5/F6 cheap fixes only. Mention in code comments where a channelizer would slot in is unnecessary — keep it in docs only.
- **A10** Conflict: F8 (flush ring on config change) vs D1 determinism. Flushing at an arbitrary instant introduces a run-dependent gap. **Resolution:** acceptable — config changes are user actions and inherently non-deterministic; determinism guarantee applies only between config changes. Document this.

---

## 4. Current-State Impact Analysis

How to locate everything (paths relative to repo root):

- **Renderer:** `simulator/src/renderer.c`. Key functions: `render_audio_modulated` (per-block phase reset at the `shift_c = 1.0` init), `render_direct_nco` (per-block `volk_malloc`, oscillator init), `render_resampled_nco` / `render_resampled_baseband` (per-block `build_quarter_phase_weights`, oscillator init), `render_noise_floor` (xorshift per sample), `renderer_render_window_block` (per-signal loop, `signal_passband_gain`, `iq_signal_active` call), `mix_accumulate_sample` (per-sample clip — to be removed in favor of mix bus).
- **Signal timing:** `simulator/src/iq_file_reader.c` — `iq_signal_active` (floors fractional offset).
- **Streamer:** `simulator/src/streamer.c` — `stream_render_thread_main` (block clock: `render_time_ns` init from `timebase_now_ns`, clamp `render_time_ns < now_ns`, advance by `block_duration_ns`), `stream_udp_thread_main` (stamps `scenario_time_ns = timebase_now_ns()` at send — the F7 bug), `RINGBUFFER_PACKET_CAPACITY 256` (F8), `apply_stream_affinity` + single shared `stream_cpu` (P5), `pace_or_record_late`, metrics recording helpers.
- **Ring buffer:** `simulator/src/ringbuffer.c/.h` — byte-oriented, mutex-guarded by callers. Becomes SPSC record ring (P6, F7).
- **Scenario:** `simulator/src/scenario.c/.h` — parse (`scenario_load_json`), `scenario_validate` (unused `base_dir`, double asset load), noise schema.
- **Assets:** `simulator/src/asset_cache.c/.h` — `build_audio_helpers` (Hilbert radius 31, integral), load paths. `simulator/src/wav_reader.c` (PCM16-only).
- **Timebase:** `simulator/src/timebase.c` — `timebase_now_ns` (REALTIME mod day), override mechanism (tests use it).
- **Receiver:** `simulator/src/receiver.c` — scan-rate fallback, `receiver_validate`.
- **UDP:** `simulator/src/udp_output.c` — `udp_output_send_batch` (sendmmsg); GSO would go here (P7).
- **VITA-49:** `simulator/src/vita49_packet.c/.h` — packet writer; consumes `timestamp_ns`; no changes expected beyond what F7 feeds it.
- **Config/limits:** `simulator/src/config.c/.h`, `simulator/src/sim_types.h` (`SIM_MAX_*`, `iq_ci16_t`), `sim_config.h` is *generated* by Meson (`config_data` in `meson.build`) — new compile-time feature flags go there.
- **REST:** `simulator/src/rest_server.c` — where receiver config changes originate; F8's generation counter must be incremented wherever the receiver struct is mutated under `receiver_lock` (search for mutations of `receiver_config_t` fields).
- **Tests:** `simulator/tests/unit/test_renderer.c`, `test_streamer.c`, `test_scenario.c`, `test_asset_cache.c`, `test_ringbuffer.c`, `test_timebase.c`; integration pytest under `simulator/tests/integration`; benchmark `simulator/tests/benchmarks/renderer_benchmark.c`.
- **Docs:** `docs/schemas.md` (scenario schema — update for F9), `README.md`.

Note: `README.md`, `docs/schemas.md`, `meson.build`, `renderer.c`, `scenario.c`, `asset_cache.*`, `sim_types.h` and the renderer/scenario tests have uncommitted modifications in the working tree (audio/WAV feature just added). **Read the working-tree state, not git HEAD, as the baseline.**

---

## 5. Target Architecture / Desired Behavior

### 5.1 Time and block model (the core invariant)

```
block_duration_ns = block_samples * 1e9 / sample_rate    (per stream)
block_index(t)    = t / block_duration_ns                (integer)
block_start_ns(k) = k * block_duration_ns
```
- The render thread renders block k for scenario span `[block_start(k), block_start(k+1))`,
  where k starts at `block_index(timebase_now_ns())` and increments by 1 per block. It never
  renders "at now"; it renders the *grid block containing now* (and may run ahead by ring depth).
- To avoid drift when `1e9 % sample_rate != 0` (96 MS/s: block_duration is non-integral ns),
  the canonical clock is the **absolute sample index**: `start_sample(k) = k * block_samples`,
  `block_start_ns(k) = start_sample(k) * 1e9 / sample_rate` computed in 128-bit integer math
  (pattern already exists in `sample_index_from_time_ns`). Never accumulate ns additively.
- Every per-signal quantity derives from `start_sample(k)`:
  - Mixer phase: `phase0 = 2π · offset_hz · start_sample / fs`, reduced with `fmod` in double
    (or a 64-bit turns-based fixed-point accumulator — preferred, see Task 3).
  - Source position: `(day_seconds(block_start) − start_time) mod repeat`, as source samples
    *with fraction* (F3).
  - Noise pool offset: `hash(seed, window_center, window_bw, block_index)`.

### 5.2 Render data flow (after changes)

```
per stream worker (once): float32 mix_bus[block_samples], scratch bufs, precomputed kernels
per block k:
  1. zero mix_bus
  2. noise: copy from noise pool at deterministic offset, scaled to density·bandwidth (F9/P4)
  3. per signal: skip checks (source/asset/passband/active/F6 Nyquist check)
       → resample (kernel with decimation-aware cutoff, fractional start offset)
       → rotate by NCO with phase0 from absolute time (VOLK rotator, float)
       → multiply-accumulate into mix_bus (no clipping)
  4. one pass: mix_bus → ci16 with saturation (VOLK convert), into ring entry payload
  5. ring entry = { timestamp_ns = block_start_ns(k), payload }
```

### 5.3 Streaming flow

- Ring: SPSC, fixed-size records `{uint64 timestamp_ns, iq_ci16_t payload[block_samples]}`,
  depth 8 records. Producer = render thread, consumer = UDP thread, atomic indices.
- UDP thread: pops up to batch-size records, builds VITA-49 packets with the record's own
  timestamp (F7), sends via `sendmmsg` (existing), paces on the timestamp grid.
- Config change (REST): increments `atomic_uint config_generation` in the receiver struct;
  render thread compares snapshot generation per block, on change resets its block clock to
  `block_index(now)` and signals the UDP thread to drain/flush (or producer writes a flush
  marker). Retune latency target: ≤ ring_depth · block_duration (≪ 1 s).
- Error handling: any thread abort path (`calloc` fail, `udp_output_open` fail) increments a
  new `worker_errors` metric and writes one stderr line with stream id (F18).

### 5.4 Validation rules (after changes)

- Paths: `source.file` joined with scenario `base_dir` when relative (F12).
- Hard failures (unchanged plus): duplicate ids, unknown source refs, bad modulation/source
  combos, zero rates, scan_rate ≤ 0 in scan mode (F19), noise config invalid.
- New warnings (stderr, non-fatal, A5): signal bandwidth vs source rate mismatch; WBFM
  bandwidth < Carson estimate `2·(fm_deviation + audio_bw)` where `audio_bw ≈ source_rate/2`;
  `am_depth == 0`.
- Error strings include element index and id (F20).

---

## 6. Detailed Implementation Plan

Ordered; tasks within a phase are sequential unless noted. Run `meson test -C <builddir> --suite unit`
after every task; the benchmark after Phase C.

### Phase A — Determinism & continuity (F1–F4, F7, F8, F17, F21)

**Task A1 — Block grid in the stream workers (F2, F17)**
- Objective: render times come from a deterministic grid.
- Files: `simulator/src/streamer.c` (+ small helper in `timebase.h` or `streamer.h`).
- Change: in `stream_render_thread_main`, replace the `render_time_ns` init/clamp/advance
  logic with a `uint64_t block_index` derived as in §5.1; recompute `block_index` (a) at
  start, (b) when the stream was disabled, (c) when `now` indicates midnight wrap
  (`now + day/2 < block_start` → wrap: reset), (d) when the render has fallen more than
  ring-depth blocks behind `now` (catch-up: jump forward, count skipped blocks as late/missed
  via existing metrics helpers).
- Implementation guidance: add `uint64_t streamer_block_start_ns(uint64_t block_index, size_t block_samples, uint32_t sample_rate_hz)`
  using `__uint128_t`, mirroring `sample_index_from_time_ns`. Export it (needed by tests and
  by renderer phase computation).
- Dependencies: none.
- Acceptance: new unit test — two simulated workers with the same config and timebase override
  produce identical block start sequences regardless of when the loop "starts"; block starts at
  96 000 000 S/s with block_samples from config show zero cumulative drift over ≥1e6 blocks
  (compare against 128-bit exact computation).

**Task A2 — Ring buffer carries timestamps, SPSC, depth 8 (F7, F8-part, P6)**
- Objective: ring entries are `{timestamp_ns, payload}` records; lock-free SPSC.
- Files: `simulator/src/ringbuffer.c/.h`, `simulator/src/streamer.c`, `simulator/tests/unit/test_ringbuffer.c`, `test_streamer.c`.
- Change: redesign ringbuffer API to fixed-record SPSC: `ringbuffer_init(rb, record_bytes, record_capacity)`,
  `ringbuffer_try_push(rb, const void*)`, `ringbuffer_try_pop(rb, void*)`, `ringbuffer_clear`
  (consumer-side only, or via a flush flag — see guidance), `ringbuffer_fill_records`.
  Head/tail are `_Atomic size_t`; producer writes payload then releases head; consumer acquires.
  Remove `worker->ringbuffer_lock` entirely. Set capacity to `#define RINGBUFFER_PACKET_CAPACITY 8U`.
- Guidance: `ringbuffer_clear` from a third thread is unsafe in SPSC — implement flush as an
  `_Atomic uint32_t flush_requested` the consumer honors (drops records until empty), or have
  the *render* thread (producer) stop pushing and the consumer drain naturally; given depth 8
  the natural drain is ≤ 8 blocks — simplest correct option: **just rely on the small depth,
  plus reset of the producer's block clock** (see A4). Record struct layout: put `timestamp_ns`
  first, payload inline; `record_bytes = sizeof(uint64_t) + block_samples*sizeof(iq_ci16_t)`.
- Change in `streamer.c`: render thread pushes records (timestamp = block start from A1);
  UDP thread pops records and uses `record.timestamp_ns` for the VITA packet (delete the
  `scenario_time_ns + i*block_duration` extrapolation and the send-time `timebase_now_ns()`
  stamping). Overrun = `try_push` false; underrun = `try_pop` false — wire to existing metrics.
- Dependencies: A1.
- Acceptance: `test_ringbuffer.c` rewritten for record API incl. a two-thread stress test
  (1e6 records, verify order and no tearing via sequence numbers in payload);
  `test_streamer.c` verifies emitted VITA timestamp == block-grid timestamp of the payload
  content (use timebase override); ThreadSanitizer build of the unit tests passes
  (`meson setup build-tsan -Db_sanitize=thread`).

**Task A3 — Phase-continuous NCO from absolute time (F1, F4, F21)**
- Objective: mixer phase derives from absolute sample index; rotators renormalized.
- Files: `simulator/src/renderer.c/.h`, `simulator/src/nco.c/.h`, `simulator/tests/unit/test_renderer.c`.
- Change:
  1. Extend `renderer_render_window_block` (and both public wrappers) to accept the block's
     absolute start sample index `uint64_t start_sample` (or keep passing `scenario_time_ns`
     but *require* it to be a grid time and derive `start_sample` internally via the exact
     128-bit conversion — pick this if signature churn is large; document the precondition).
  2. Compute per signal: `phase0_turns = fmod(offset_hz * (double)start_sample / fs, 1.0)`.
     Precision note: `offset_hz * start_sample` overflows double precision after long uptimes;
     compute in integer/fixed-point: represent phase step per sample as a 64-bit fixed-point
     "turns" value `step_q64 = round(offset_hz / fs * 2^64)` and `phase0_q64 = start_sample * step_q64`
     (wrapping multiply is exactly the mod-1 arithmetic needed). Convert `phase0_q64` to
     radians only once per block. This is exact, drift-free, and deterministic.
  3. Initialize `shift_c/shift_s` (all paths: `render_audio_modulated`, `render_direct_nco`
     scalar+VOLK — VOLK path: initial `phase` = `cos(phase0)+i·sin(phase0)` instead of `1+0i`,
     `render_resampled_nco` all three sub-paths) from `phase0`.
  4. Renormalize scalar rotators every 256 iterations (`inv = 1/sqrt(c²+s²)`).
  5. Move the fixed-point phase helper into `nco.c/.h` (`nco_phase_q64_step`, `nco_phase_q64_at`)
     and delete the now-dead `nco_mix_ci16`/`nco_init` if nothing else uses them (grep first).
- Dependencies: A1 (grid times exist).
- Acceptance: unit test renders N consecutive blocks and one single N·block render of the same
  span; outputs match within ±1 LSB for a CW tone at a non-block-periodic offset (e.g.
  offset 1234.5 Hz). Second test: rotator magnitude after 1e6 float iterations stays within
  1e-4 of 1.0. Existing renderer tests updated for the new signature/precondition.

**Task A4 — Fractional source offset (F3) and config-change flush (F8-rest)**
- Objective: no per-block source-timing jitter; retunes take effect within ring depth.
- Files: `simulator/src/iq_file_reader.c/.h`, `simulator/src/renderer.c`, `simulator/src/streamer.c`, `simulator/src/receiver.h`, `simulator/src/rest_server.c`, tests.
- Change:
  1. `iq_signal_active(..., uint64_t *sample_offset, double *offset_fraction)` — compute
     `exact = occurrence_elapsed * sample_rate` (double), `sample_offset = floor(exact)`,
     `offset_fraction = exact − floor(exact)`. Update the one renderer call site and header.
     Better determinism variant (preferred): compute `occurrence_elapsed` from the block's
     *sample-exact* start time (`start_sample`), not from `day_s` double, so the fraction is
     identical across instances: pass block start ns (grid value) instead of `day_s`.
  2. Renderer: resampled paths start at `source_position = offset_fraction + i·source_per_output`;
     direct (same-rate) path: if `offset_fraction ≥ 1e-9`, route through the resampled path
     (rare; only when rates match but grid start doesn't land on a source sample).
     `render_audio_modulated`: `audio_position = sample_offset + offset_fraction + i·source_per_output`.
  3. Config generation: add `_Atomic uint32_t generation` to the receiver runtime struct
     (find where `receiver_config_t` is mutated under `receiver_lock` — REST handlers in
     `rest_server.c`); increment on every mutation. Render thread keeps last seen generation;
     on change: reset block clock (recompute `block_index` from now). With ring depth 8 the
     wire latency is ≤ 8 blocks. No explicit ring flush needed (A2 guidance); document.
- Dependencies: A1–A3.
- Acceptance: renderer unit test — a 1 kHz tone from an IQ asset rendered as consecutive blocks
  has no discontinuity at boundaries (max inter-sample delta at the boundary ≤ max delta inside
  blocks). Streamer test: after toggling a config field under timebase override, the first
  packet with new-config content appears within 8 blocks.

### Phase B — DSP correctness (F5, F6, F9, F10, F11, F16)

**Task B1 — Nyquist skip check (F6)** — trivial, do first.
- Files: `simulator/src/renderer.c`.
- Change: in the per-signal loop of `renderer_render_window_block`, after computing `offset_hz`:
  skip the signal if `fabs(offset_hz) − signal_bw/2 > output_sample_rate/2` (entire band beyond
  Nyquist; strict `>`). Keep `signal_passband_gain` as is; add a code comment documenting the
  straddle approximation (per A9/1.5).
- Acceptance: unit test — signal centered outside window ±fs/2 renders exact zeros; signal
  straddling the edge still renders (attenuated).

**Task B2 — Decimation-aware resampler kernels, precomputed (F5, P3)**
- Files: `simulator/src/renderer.c/.h` (+ init call from `streamer.c` or lazy-once).
- Change:
  1. New struct `resampler_kernel_t { float weights[PHASES][TAPS]; double cutoff; }`.
     Kernel formula: `w(d) = cutoff · sinc(cutoff·d) · hann(d/RADIUS)` with
     `cutoff = min(1.0, 1.0/source_per_output)`, normalized per phase (as today).
  2. Since `source_per_output` is a per-(signal-source, stream) constant, precompute one kernel
     per unique ratio at worker start (ratios enumerable: sources × the worker's own output
     rate — a small array in the worker; build in `stream_worker_start` and pass into the
     render calls, or a tiny cache keyed by ratio inside the renderer guarded to init once
     before threads spawn). **Do not** rebuild `build_quarter_phase_weights` per block —
     replace those call sites with the precomputed kernel (quarter-rate is just the ratio-4
     kernel with cutoff 0.25).
  3. Generalize the quarter-phase fast path: with cutoff-scaled kernels the "quarter" special
     case becomes "integer ratio with ≤64 phases" — implement a polyphase table with
     `N_PHASES = 64` fractional phases and nearest-phase lookup (error ≤ 1/128 sample —
     fine at these kernel lengths); keep the exact per-position sinc fallback for edges.
  4. Optional (only if time permits, behind existing `SIM_USE_LIQUID_RESAMPLER`): route
     DDC-only paths with `source_per_output > 8` through liquid `msresamp_crcf`.
- Dependencies: A3/A4 (source positions final).
- Acceptance: unit test — render a full-scale tone at 0.4×source_Nyquist from a source with
  `source_per_output = 4` and verify the alias image is ≥ 40 dB below the (absent) in-band
  level, versus ~0 dB suppression before. Existing quarter-rate tests still pass (update
  tolerances if the cutoff-corrected kernel shifts values slightly). Benchmark: wideband path
  cost unchanged within 5%.

**Task B3 — Noise as density + precomputed pool (F9, P4)**
- Files: `simulator/src/renderer.c/.h`, `simulator/src/scenario.c/.h`, `docs/schemas.md`, tests.
- Change:
  1. Schema (see §7): accept `power_dbm_per_hz` (new, preferred) or legacy `power_dbm`
     (deprecated; interpret as total power in the *80 MHz reference bandwidth*? — **No**:
     ambiguous. Interpret legacy exactly as today per-window and emit a deprecation warning;
     new field gives correct density behavior. Both set → error).
  2. Noise pool: at startup (once per process, e.g. in `asset_cache_load` or a new
     `renderer_global_init`), fill a `float` array of `NOISE_POOL_SAMPLES = 1<<21` (2M) I/Q
     pairs with unit-variance Gaussian-ish noise: sum of 4 uniforms from splitmix64-seeded
     xorshift, scaled — deterministic given `scenario->noise_floor.seed`.
  3. Per block: `offset = splitmix64(seed ^ window_center ^ window_bw ^ block_index) % (POOL − count)`;
     amplitude `A = 32767 · output_scale · 10^((density + 10·log10(window_bw) − ref)/20)`;
     copy `A · pool[offset+i]` into the mix bus (after P1 lands this is a vectorized
     scale-add; before P1, write into `out` as today with correct RMS calibration — the pool
     is unit-RMS so no √3 issue).
  4. Delete `xorshift32_next`/`scale_noise_i16` per-sample generation.
- Dependencies: A1 (block_index); ideally after C1 (mix bus) — if C1 is done first, write
  directly to the bus. Order chosen here: do B3 *after* C1 (see phase ordering note below).
- Acceptance: unit test — measured RMS power of rendered noise matches
  `density + 10·log10(bw)` within 0.3 dB for two different window bandwidths (e.g. 80 MHz vs
  25 kHz configs); histogram roughly Gaussian (kurtosis sanity check, loose bounds);
  same (seed, window, block_index) → identical samples; different block_index → different.

**Task B4 — AM/SSB power normalization (F10) + end-of-asset gating (F11) + Hilbert radius (F16)**
- Files: `simulator/src/asset_cache.c/.h`, `simulator/src/renderer.c`, tests.
- Change:
  1. In `asset_cache_load` (audio branch): compute asset RMS; if > 0, scale all
     `audio_samples` by `(1/√2)/rms` (A8) *before* building Hilbert/integral; store
     `normalization_gain` in `cached_asset_t`.
  2. `HILBERT_RADIUS` 31 → 95 (odd taps only, still startup-only O(N·R)).
  3. Renderer AM: `envelope = (1 + depth·audio) / (1 + depth)`.
  4. `render_audio_modulated`: per sample, if `audio_position >= asset->sample_count`, stop
     contributing (break — positions are monotonic).
- Dependencies: none within phase (independent of B2/B3).
- Acceptance: asset test — loaded asset RMS == 1/√2 ±1e-3; SSB image rejection test (render
  USB of a 1 kHz tone, measure LSB image) improves to ≥ 45 dB; AM peak sample with depth 1.0
  and gain 1.0 ≤ 32767 (no saturation); FM render of a block spanning file end contains zeros
  after the end position.

### Phase C — Performance (P1–P5, P8, P9)

**Task C1 — Float32 mix bus, clip-once (P1)** — the keystone; do before B3 if resequencing.
- Files: `simulator/src/renderer.c/.h`, `simulator/src/streamer.c`, benchmark, tests.
- Change:
  1. `renderer_render_window_block` gains an internal float bus: either caller-provided
     scratch (`render_scratch_t { float *bus_i; float *bus_q; ... }` created once per worker
     in `stream_worker_start`, freed in join) — preferred, no hidden state — or static
     per-call allocation is forbidden (hot path).
     Consider interleaved `float complex` layout to match VOLK `lv_32fc_t` (preferred: one
     `lv_32fc_t *bus`).
  2. All `render_*` inner loops accumulate into the bus (plain `+=`, no clip). Delete
     `mix_accumulate_sample`, `accumulate_direct_baseband`, `clip_i16_f` accumulate usage.
  3. Final pass: bus → ci16 with saturation. Scalar fallback + VOLK path
     (`volk_32fc_deinterleave_32f_x2` need not be used — a simple loop with `lrintf` and
     clamp auto-vectorizes; measure, use VOLK `volk_32f_s32f_convert_16i` on split planes
     only if it wins).
  4. Noise (until B3 reworks it) writes into the bus first.
  5. Precision note: float accumulation of ≤ SIM_MAX_SIGNALS full-scale signals stays well
     inside float range; document that intermediate values may exceed ±32767 and only the
     final conversion saturates (this is the desired order-independence fix).
- Dependencies: A3 (signatures already touched — coordinate to avoid churn: **implement A3
  and C1 signature changes together** if convenient).
- Acceptance: all renderer unit tests pass (expected small numeric diffs: update tolerances,
  not expectations, except where old clipping made results order-dependent — add a new test:
  two strong signals summed in either order give identical output). Benchmark shows wideband
  block render cost reduced (record before/after numbers in the PR).

**Task C2 — VOLK everywhere it pays + hoisted scratch (P2)**
- Files: `simulator/src/renderer.c`, `meson.build`.
- Change: move the `volk_malloc` buffers of `render_direct_nco` into the per-worker scratch
  (C1); apply the VOLK rotator to `render_resampled_nco`'s output stage (resample into
  scratch, then one rotator + one accumulate pass over the block instead of per-sample
  rotate); `render_audio_modulated`: generate baseband into scratch then rotate+accumulate
  the same way. Meson: change `volk_accel` default from `auto` to `enabled` (still
  disableable for exotic platforms) — per P8/A7.
- Dependencies: C1.
- Acceptance: unit parity tests (VOLK vs scalar builds produce outputs within ±1 LSB —
  build both: `-Dvolk_accel=disabled` job in CI/test script); benchmark improvement recorded.

**Task C3 — CPU pinning scheme (P5)**
- Files: `simulator/src/streamer.c`, `simulator/src/config.c/.h`, configs in `simulator/configs/*.yaml`, docs.
- Change: replace single `stream_cpu` with a CPU list/range config (`stream_cpus: "2-7"` or
  YAML list). Assignment policy: wideband render threads first, each on its own CPU from the
  set; then DDC render threads round-robin; UDP threads round-robin over the *remaining* CPUs
  (or interleaved but never sharing a wideband render CPU). Empty/absent list = no pinning
  (current default behavior when `stream_cpu < 0` — preserve). Keep `stream_cpu` (single int)
  parsing as a deprecated alias mapping to a one-element list.
- Dependencies: none.
- Acceptance: config unit test for parsing (list, range, legacy single, absent); manual check
  with `ps -eLo pid,psr,comm` that threads land on distinct CPUs per policy.

**Task C4 — Build flags + benchmark gate (P8, P9)**
- Files: `meson.build`, `simulator/tests/benchmarks/renderer_benchmark.c`.
- Change: add per-file `c_args` for the renderer (and streamer) TU: `-O3 -fno-math-errno`
  (Meson: build a static lib `sim_render` with `c_args`); optional `-Dnative=true` meson
  option adding `-march=native` (default off, per A1 flag it). Extend the benchmark to a
  realistic load: 96 MS/s wideband + all DDCs, 4 signals (1 WBFM audio, 1 AM, 2 IQ) + noise,
  print MS/s achieved and realtime ratio; add a `--assert-realtime <ratio>` mode used
  manually (not in CI, machine-dependent).
- Dependencies: C1/C2 (measure final state).
- Acceptance: benchmark builds and reports; documented result in PR showing ≥1.3× realtime
  on the dev machine for the P9 load. If not achieved, escalate (see §10 R1).

### Phase D — Robustness & validation (F12–F15, F18–F20)

**Task D1 — base_dir resolution + single asset load (F12, F14)**
- Files: `simulator/src/scenario.c`, `simulator/src/asset_cache.c`, `simulator/src/main.c` (find where `scenario_validate` and `asset_cache_load` are called), tests.
- Change: in `scenario_validate`, resolve each `source->file` against `base_dir` when the path
  is relative (write the joined path back into `source->file`; ensure buffer size — check
  `sizeof(source->file)`; fail with `source_path_too_long` if it doesn't fit). Replace the
  full WAV load in validation with a new `wav_reader_probe(path, &rate, &frames, err, ...)`
  (header-only parse, no sample read/alloc); IQ files already only stat via open — keep.
  `asset_cache_load` then does the single full load and the existing
  `wav_cache_mismatch` check remains the consistency guard.
- Acceptance: scenario with relative paths loads when CWD ≠ scenario dir (new unit test using
  a temp dir); startup loads each WAV exactly once (probe ≠ load; verifiable via a counter in
  tests or by function separation alone).

**Task D2 — WAV extensible/float robustness (F15)**
- Files: `simulator/src/wav_reader.c`, `simulator/tests/unit/test_asset_cache.c` (or a new `test_wav_reader.c` + register in `test_suites.h`/`test_main.c` and `meson.build`).
- Change: accept `audio_format == 0xFFFE` when the fmt chunk is ≥ 40 bytes and the
  SubFormat GUID's first two bytes are PCM (0x0001); read `valid_bits` sanity. Treat
  `data_bytes == 0xFFFFFFFF` as read-to-EOF (`frame_count` from file size − data_offset).
  Error strings per failure mode (`wav_unsupported_format:3` etc.).
- Acceptance: unit tests with synthesized headers: extensible PCM16 accepted; float32
  rejected with a clear message; 0xFFFFFFFF data size handled.

**Task D3 — Validation warnings + error context (F13, F19, F20)**
- Files: `simulator/src/scenario.c`, `simulator/src/receiver.c`, tests.
- Change: `snprintf` error messages gain `[index] id=<id> field=<field>`; add stderr warnings
  per A5 (bandwidth consistency, Carson, depth 0); `receiver_validate` rejects
  `scan_rate_hz_per_s <= 0` **only when** scan mode is effective (`receiver_effective_mode == SCAN`);
  remove the `100000000000.0` fallback in `receiver_center_frequency_hz` (validation now
  guarantees positivity; keep a defensive `return start` if ≤0).
- Acceptance: existing validation tests updated for new messages; new tests: scan-mode with
  scan_rate 0 fails, fixed-mode with scan_rate 0 passes.

**Task D4 — Worker failure diagnostics (F18)**
- Files: `simulator/src/streamer.c`, `simulator/src/metrics.h`, `simulator/src/rest_server.c` (expose in metrics JSON — find where `receiver_metrics_t` is serialized).
- Change: add `atomic_uint worker_errors` to `receiver_metrics_t` and `stream_metrics_t`;
  every early-return path in both thread mains increments it and `fprintf(stderr, ...)` with
  receiver id / stream kind / ddc index / reason. Expose in the REST metrics output.
- Acceptance: unit-level: force `udp_output_open` failure (invalid host) and assert metric
  increments; REST metrics JSON contains the field.

### Phase E — Optional stretch (P7) — only after A–D are merged and benchmarked
- E1: UDP GSO (`UDP_SEGMENT` cmsg) in `udp_output.c` behind a config flag, fallback to
  `sendmmsg` when `setsockopt` fails. E2: `SO_SNDBUF` from config with warning if kernel
  clamps. E3: `mlock`/`MADV_HUGEPAGE` for asset cache behind config flag. E4: `SCHED_FIFO`
  for send threads behind config flag (requires CAP_SYS_NICE — degrade gracefully).
- Acceptance: each behind a flag, off by default, degrades gracefully, documented in README.

**Recommended execution order:** A1 → A2 → A3+C1 (coordinate signatures) → A4 → C2 → B1 → B2 →
B3 → B4 → C3 → C4 → D1 → D2 → D3 → D4 → (E).

---

## 7. Data Model / API Changes

No database. The "data model" is the scenario JSON, instance YAML, and the wire/REST formats.

### Scenario JSON (`docs/schemas.md` must be updated)
- `noise_floor.power_dbm_per_hz` (number, new, preferred). Semantics: noise density; rendered
  in-window power = `power_dbm_per_hz + 10·log10(window_bandwidth_hz)` relative to
  `rf_reference_power_dbm` calibration, Gaussian amplitude distribution.
- `noise_floor.power_dbm` (legacy): still parsed; behavior unchanged from today (total
  in-window power) **plus** the √3 RMS correction so the number is finally honest — note this
  as a behavior change in the changelog. Specifying both fields → validation error
  `noise_floor_conflicting_power`.
- `schema_version`: bump (read current value from `docs/schemas.md`/example scenarios; accept
  previous version with legacy noise semantics).
- No changes to `sources`/`signals` fields; `file` paths now documented as relative-to-scenario-dir.

### Instance YAML config
- `stream_cpu` (int, deprecated) → `stream_cpus` (list or "a-b" range string). Legacy key maps
  to a one-element list with a stderr deprecation note.
- Optional new keys (Phase E, all default off/0): `udp_gso: bool`, `udp_sndbuf_bytes: int`,
  `lock_assets_memory: bool`, `realtime_send_priority: bool`.

### Wire format (VITA-49)
- Packet layout unchanged. Timestamp semantics *corrected*: timestamp = scenario time of first
  payload sample (was: send-time). Consumers relying on the old (broken) semantics: none known;
  note in changelog. Sequence numbering unchanged.

### REST API
- Metrics response: add `worker_errors` per receiver and per stream. Additive → backward
  compatible.
- No new endpoints. Config-mutation handlers additionally bump the generation counter
  (internal, not exposed; optionally expose `config_generation` in status for debugging —
  nice-to-have).

### C-internal API changes (for reviewer awareness)
- `ringbuffer_*`: byte API → record API (breaking, internal only).
- `renderer_render_*_block`: gains scratch/context parameter and grid-time precondition.
- `iq_signal_active`: extra out-param for fractional offset (or block-start-ns input variant).
- New: `streamer_block_start_ns`, `nco_phase_q64_*`, `wav_reader_probe`, `resampler_kernel_t`.

---

## 8. UI/UX Implementation Details

Not applicable — headless service (REST + UDP). The equivalent "UX" surfaces:
- **Error messages** (F20): format `"<check>[<index>] id=<id>: <detail>"`, stable prefixes so
  scripts can match; keep the `"ok"` convention on success.
- **Startup log**: one line per receiver/stream at start (rates, ports, CPU assignment), one
  line per validation warning (A5), deprecation notes for legacy config keys. All to stderr,
  single-line, greppable prefixes (`warning:`, `deprecated:`, `error:`).
- **Metrics** (REST): new `worker_errors`; ensure zero-value fields still serialized so
  dashboards don't break on absence.
- **Docs**: README section "Determinism guarantees" (what is identical across instances, what
  isn't, effect of config changes per A10); `docs/schemas.md` for noise density; comment block
  at top of `renderer.c` describing the block-grid/phase model (§5.1) — future maintainers
  must not reintroduce wall-clock coupling.

---

## 9. Testing Plan

Run everything via `meson test -C <builddir>` (suites: `unit`, `integration`); benchmark
target `renderer_benchmark`. Use `timebase_set_override` for all determinism tests.

### Unit tests (per task — listed in §6 acceptance; consolidated highlights)
1. **Block grid:** grid start times exact vs 128-bit reference over ≥1e6 blocks @96 MS/s; independence from thread start time; midnight wrap resets cleanly.
2. **Phase continuity (the flagship test):** for each render path (direct-NCO, resampled-NCO ratio 4 and ratio 0.1, audio WBFM/AM/USB/LSB): render 16 consecutive blocks and compare against a single 16-block render — max abs diff ≤ 1 LSB; boundary-sample deltas statistically indistinguishable from interior deltas.
3. **Determinism:** two independently constructed worker/render contexts, same config, same override times → byte-identical payloads and timestamps, including noise.
4. **Rotator stability:** float rotator magnitude within 1e-4 after 1e6 steps.
5. **Resampler:** alias suppression ≥ 40 dB at decimation 4 and 16; passband tone amplitude error ≤ 0.5 dB; upsampling paths unchanged within tolerance.
6. **Nyquist skip:** out-of-window signal → exact zeros; straddling → attenuated nonzero.
7. **Noise:** density scaling across two bandwidths within 0.3 dB; unit-RMS pool; per-(seed,window,block) reproducibility; distribution sanity.
8. **Mix bus:** order-independence of two-signal sum; saturation only at final conversion (two half-scale signals in-phase → exactly ±32767, not double-clipped artifacts).
9. **AM/SSB/FM:** AM no clipping at depth 1; asset RMS normalization; SSB image ≥ 45 dB; end-of-asset silence (F11).
10. **Ring buffer:** SPSC stress (2 threads, 1e6 records, sequence-stamped payloads, order+integrity); overrun/underrun metric paths; TSan-clean.
11. **Streamer:** VITA timestamp == payload block-grid time; retune visible within 8 blocks; enable/disable snaps to grid; worker_errors increments on forced socket failure.
12. **Scenario/WAV/receiver:** relative-path resolution; probe-not-load; extensible WAV; data=0xFFFFFFFF; error-message format; scan-rate validation; noise field conflict; legacy fields with deprecation behavior.

### Integration tests (pytest, `simulator/tests/integration`)
- Start the simulator with `audio_radio_demo.json` + an IQ scenario, capture UDP for N seconds,
  parse VITA-49: (a) timestamps advance by exactly block_duration per packet per stream,
  (b) all streams' timestamps come from the same grid, (c) demodulate the WBFM channel in
  Python (numpy) and assert audio SNR above a threshold and **no periodic click energy at the
  block rate** (FFT of demodulated audio: no spike at fs_audio·block_rate ratio) — this is the
  end-to-end proof of F1/F3.
- Determinism run: start two simulator instances (different ports) with the same scenario and
  a timebase override (or synchronized start); capture both; compare payloads per timestamp —
  byte-identical.
- Regression: existing integration tests must pass unmodified except for asserted timestamp
  semantics.

### Manual QA
- Run with the real receiver app (`receiver/`) against 96 MS/s config; listen to demodulated
  FM audio for clicks; retune via REST and confirm sub-second wire response; observe
  `htop`/`ps -eLo psr` for pinning; watch metrics for overruns/underruns at steady state (should be ~0).

### Definition of done per feature
- Phase A done = tests 1–4, 10, 11 green + TSan clean + integration determinism run passes.
- Phase B done = tests 5–7, 9 green + integration demodulation test passes.
- Phase C done = tests 8 green, VOLK/scalar parity, benchmark ≥1.3× realtime on P9 load recorded.
- Phase D done = tests 12 green + docs updated.

---

## 10. Risks and Dependencies

- **R1 (high): 96 MS/s might not fit even after C1/C2** on the target machine (memory
  bandwidth: the mix bus adds a float pass over 96 MS/s × 8 bytes). Mitigation: benchmark
  after C1 *before* polishing; if short, add intra-block slicing across threads (§5 of
  IMPROVEMENTS.md, deliberately not scheduled — simplest escape hatch) or reduce per-block
  passes by fusing rotate+accumulate. Escalate with numbers rather than silently shipping slow.
- **R2 (medium): numeric drift in tests.** Moving double→float and clip-once changes LSBs
  everywhere; existing test expectations in `test_renderer.c` will need tolerance updates.
  Risk: masking a real bug behind loosened tolerances. Rule: tolerances may widen to ≤ 2 LSB;
  anything larger needs investigation.
- **R3 (medium): SPSC ring + flush semantics.** Cross-thread `ringbuffer_clear` was mutex-safe,
  SPSC is not. The plan removes third-party clears (drain-by-depth instead) — verify the
  disabled-stream path in `stream_render_thread_main` (which currently clears the ring) is
  reworked accordingly (producer stops pushing; consumer drains; both idle).
- **R4 (medium): fixed-point phase subtleties.** `step_q64 = round(offset_hz/fs · 2^64)`
  overflows uint64 when |offset| ≥ fs/2 — but F6 guarantees |offset| ≤ fs/2 + bw/2 for rendered
  signals; clamp/verify at the call site, and handle negative offsets via two's-complement
  wrap (use `int64` turns or add 1.0 turn). Unit-test ±offsets explicitly.
- **R5 (low): VITA timestamp semantics change** could surprise an existing consumer that
  compensated for the old behavior. Check `receiver/` (in-repo consumer) for timestamp
  handling and update it in the same PR if needed.
- **R6 (low): schema changes** — other tooling (scripts in `simulator/scripts/`, example
  scenarios) referencing `noise_floor.power_dbm` keeps working (legacy path), but examples
  should be migrated to the new field.
- **R7 (low): liquid-dsp optional path** (B2 step 4) adds a second code path to keep in parity;
  skip it if schedule pressure — the cutoff-scaled kernel alone meets the acceptance bar.
- **Missing information:** target CPU (A1), exact acceptable retune latency (assumed ≤8 blocks),
  asset RMS target (A8), whether legacy `power_dbm` RMS correction is wanted (§7) — confirm all
  four with the user before or during Phase B/C.

---

## 11. Final Checklist for the Implementing AI

Setup
- [ ] Read `IMPROVEMENTS.md` fully; read `sim_types.h`, `config.h`, `receiver.h`, `metrics.h` for actual constants/structs (A6).
- [ ] Baseline: build (`meson setup build && meson compile -C build`), run `meson test -C build --suite unit`, run `renderer_benchmark`, record numbers.
- [ ] Note working tree already has uncommitted audio/WAV changes — branch from current tree state.

Phase A — determinism & continuity
- [ ] A1 block grid (128-bit sample-index math; wrap + catch-up handling) + tests.
- [ ] A2 SPSC record ring with timestamps, depth 8; streamer wiring; VITA stamps from records; TSan run.
- [ ] A3 phase-from-time NCO (q64 fixed point; all 5+ oscillator init sites incl. VOLK initial phase); rotator renorm; nco.c repurposed/cleaned; continuity tests.
- [ ] A4 fractional source offsets everywhere; config generation counter + reset-on-change; retune-latency test.

Phase B/C interleaved (order: C1 → C2 → B1 → B2 → B3 → B4 → C3 → C4)
- [ ] C1 float mix bus, clip-once, per-worker scratch; order-independence test; delete per-sample clip helpers.
- [ ] C2 VOLK rotate/accumulate/convert paths + hoisted scratch; scalar-parity CI job (`-Dvolk_accel=disabled`); meson default `enabled`.
- [ ] B1 Nyquist skip check + straddle comment.
- [ ] B2 precomputed cutoff-scaled polyphase kernels; alias-suppression test; no per-block table builds remain (grep `build_quarter_phase_weights` call sites).
- [ ] B3 noise density field (+ legacy path + conflict error), Gaussian unit-RMS pool, per-block deterministic offset; schema docs + examples updated.
- [ ] B4 asset RMS normalization (target 1/√2 — flagged A8), AM `/(1+depth)`, end-of-asset gating, HILBERT_RADIUS 95.
- [ ] C3 `stream_cpus` set-based pinning with policy + legacy alias.
- [ ] C4 per-TU `-O3 -fno-math-errno`, optional `-Dnative`; benchmark extended to P9 load; record ≥1.3× realtime or escalate (R1).

Phase D — robustness
- [ ] D1 base_dir path resolution + `wav_reader_probe` (no double load).
- [ ] D2 WAVE_FORMAT_EXTENSIBLE, data=0xFFFFFFFF, specific error strings.
- [ ] D3 error messages with index/id/field; bandwidth/Carson/depth-0 warnings; scan-rate validation; remove 1e11 fallback.
- [ ] D4 `worker_errors` metric + stderr diagnostics + REST exposure.

Verification & docs
- [ ] Full unit suite green; integration suite green including new demod-click and two-instance determinism tests.
- [ ] TSan build of unit tests clean; ASan/UBSan run of unit tests clean.
- [ ] Benchmark numbers (before/after) in PR description.
- [ ] `docs/schemas.md`, README (determinism section, config keys, deprecations), `renderer.c` header comment (block-grid/phase model) updated.
- [ ] Changelog notes: VITA timestamp semantics fix, noise RMS correction, schema_version bump, deprecated keys.
- [ ] Flag open questions for user: A1 (target CPU/`-march`), A8 (RMS target), legacy `power_dbm` correction, retune latency budget.
- [ ] Do NOT commit unless the user asks.
