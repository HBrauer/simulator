# Config And Scenario Schemas

This document describes the schema implemented by `simulator/src/config.c` and `simulator/src/scenario.c`.

## Instance YAML

Top-level fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `schema_version` | integer | yes | Must be `1`. |
| `instance_id` | string | yes | Instance identifier for operators/logs. |
| `scenario_file` | string | yes | Default scenario path used when `--scenario` is omitted. |
| `log_path` | string | no | Reserved for runtime logging. |
| `stream_block_samples` | integer | no | CI16 IQ samples per VITA 49.2 IF-data packet. Defaults to `1024`; valid range is `1..4096`. Use `1536` for the MTU 9000 high-rate profile. |
| `stream_cpu` | integer | no | Linux CPU index used for stream render/UDP threads. `-1` disables pinning. Defaults to `-1`. |
| `asset_cache_max_bytes` | integer | no | Maximum total in-memory asset bytes. Defaults to 16 GiB. IQ files over the remaining budget are memory-mapped read-only (the OS pages them in lazily) instead of copied to RAM; audio sources must fit the budget. `0` means unlimited RAM loading (legacy). |
| `ddc_cache_max_bytes` | integer | no | Budget for precomputed DDC intermediate sub-bands (narrow channels extracting from wideband loop replays; see "DDC sub-band extraction" below). One cache entry holds a full source loop at the intermediate rate as CI16 (`loop_seconds * intermediate_rate * 4` bytes). Defaults to 2 GiB; `0` disables caching (the full-rate cascade then runs every block). |
| `receivers` | array | yes | At least one receiver, up to `SIM_MAX_RECEIVERS` (`12`). |

Receiver fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `receiver_id` | integer | yes | Unique per instance. |
| `rest_bind_host` | string | yes | REST bind address, for example `127.0.0.1`. |
| `rest_port` | integer | yes | Unique per instance. |
| `udp_output_host` | string | yes | UDP destination host. May be unicast, for example `127.0.0.1`, or IPv4 multicast, for example `239.10.10.10`. |
| `udp_multicast_interface` | string | no | Local IPv4 interface used for multicast sends, for example `127.0.0.1` for loopback-only testing. Empty/default lets the kernel choose. |
| `frequency_start_hz` | integer | yes | Inclusive RF tuner range start, `0..40000000000`. |
| `frequency_stop_hz` | integer | yes | RF tuner range stop, greater than start and `<= 40000000000`. |
| `frontend_bandwidth_hz` | integer | no | Instantaneous analog (ADC) window all channels extract from. Defaults to `80000000`; must be non-zero. |
| `scan_rate_hz_per_s` | number | yes | Used when the tuner range is wider than the front-end bandwidth. |
| `output_scale` | number | no | Default channel output multiplier. Defaults to `1.0`; must be positive when set. |
| `rf_reference_power_dbm` | number | no | RF power that preserves the source's `nominal_level_dbfs`. Defaults to `-55.0`. |
| `profiles` | array | no | Supported `{bandwidth_hz, sample_rate_hz}` pairs, up to `SIM_MAX_PROFILES` (`32`). Defaults to the built-in table `80/40/20/10/5/1 MHz` with the `1.2288x` rate family. Narrow profiles (for example `100 kHz / 128 kS/s`, down to `1 kHz / 2 kS/s`) are supported for DDC channels extracting from wideband replays. |
| `channels` | array | yes | `1..SIM_MAX_CHANNELS` (`24`) channel entries with IDs `0..N-1`. |

Profile fields (fixed bandwidth/sample-rate pairs, like hardware DDC decimation stages;
selecting a bandwidth always selects its paired sample rate):

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `bandwidth_hz` | integer | yes | Unique within the receiver; non-zero. |
| `sample_rate_hz` | integer | yes | Must be `>= bandwidth_hz`. |
| `name` | string | no | Label reported by the REST API, for example `"20M"`. |

Channel fields (every output stream is a channel; the former wideband stream is a
channel with `track_tuner: true` and the widest profile):

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `channel_id` | integer | yes | `0..N-1`, in order. |
| `track_tuner` | boolean | no | Channel center follows the receiver tuner (fixed center or scan sweep). Defaults to `false`. |
| `center_frequency_hz` | integer | when not tracking | Absolute RF center frequency. Ignored with `track_tuner: true`. |
| `bandwidth_hz` | integer | yes | Must exactly match a profile `bandwidth_hz` and be `<= frontend_bandwidth_hz`. The sample rate is the profile partner; specifying `sample_rate_hz` on a channel is an error. |
| `output_scale` | number | no | Channel output multiplier. Defaults to receiver `output_scale`. |
| `rf_reference_power_dbm` | number | no | Defaults to the receiver value. |
| `stream_enabled` | boolean | no | Enables this channel's UDP stream. Defaults to `true`. |
| `udp_output_port` | integer | yes | Unique across all channel UDP outputs of the instance. |

A channel whose span leaves the front-end window (tuner center ± `frontend_bandwidth_hz/2`)
keeps streaming, but empty — exactly like a hardware DDC tuned outside the digitised band.

The pre-channel keys (`bandwidth_hz`/`sample_rate_hz`/`stream_enabled`/`udp_80mhz_output_port`
at receiver level, and the `ddc` array) are rejected with a `legacy_key_...` error naming
the replacement.

Validation error codes include:

- `config_invalid`
- `too_many_receivers`
- `too_many_profiles`
- `too_many_channels`
- `invalid_stream_block_samples`
- `duplicate_receiver`
- `duplicate_rest_port`
- `duplicate_udp_port`
- `invalid_receiver`
- `invalid_frontend_bandwidth`
- `invalid_profiles` / `invalid_profile` / `duplicate_profile_bandwidth`
- `invalid_channels` / `invalid_channel`
- `channel_bandwidth_required`
- `unsupported_channel_bandwidth`
- `bandwidth_exceeds_frontend`
- `channel_sample_rate_comes_from_profile`
- `legacy_key_ddc_use_channels` (and the other `legacy_key_...` codes)
- `invalid_output_scale`

## Scenario JSON

Top-level fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `schema_version` | integer | yes | Must be `1`. |
| `scenario_id` | string | yes | Scenario identifier. |
| `description` | string | no | Human-readable description. |
| `sources` | array | yes | IQ or audio source definitions, up to `SIM_MAX_SOURCES` (`64`). |
| `signals` | array | yes | RF signal placements, up to `SIM_MAX_SIGNALS` (`256`). |

Source fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `id` | string | yes | Unique source ID. |
| `source_type` | string | yes | `iq_file` or `audio_file`. |
| `file` | string | yes¹ | Path to the IQ or audio asset. |
| `format` | string | yes | `ci16` for IQ files, `wav` for audio files. |
| `byte_order` | string | IQ only | Currently `little_endian`. |
| `iq_layout` | string | IQ only | Currently `interleaved_iq`. |
| `sample_rate_hz` | integer | yes¹ | Source sample rate. |
| `bandwidth_hz` | integer | yes¹ | Source bandwidth. |
| `center_frequency_hz` | integer | yes¹ | Source-relative center; current sample scenarios use `0`. |
| `sample_count` | integer | no | Number of complex/audio samples in the file. CI16 and WAV sources can derive this from file size. |
| `nominal_level_dbfs` | number | yes¹ | Source nominal digital level. |
| `passthrough_variants` | array | no | One capture per channel rate for passthrough replay (see below). When present, the top-level `file`/`sample_rate_hz`/`bandwidth_hz`/`center_frequency_hz`/`nominal_level_dbfs` are unused. |

¹ Not required when `passthrough_variants` is present.

`audio_file` sources currently support PCM16 WAV, mono or stereo. Stereo is folded to mono in the asset cache.

**`passthrough_variants`** — an IQ-file source that feeds a `passthrough` signal supplies one capture per channel sample rate instead of a single `file`. Each entry is `{ "sample_rate_hz": ..., "bandwidth_hz": ..., "file": ... }` (plus optional `sample_count`); the rates must be distinct. At render time the passthrough path selects the variant whose `sample_rate_hz` matches the channel's current rate and streams it verbatim; a channel bandwidth with no matching variant is silent. Example:

```json
{
  "id": "capture_fm",
  "source_type": "iq_file",
  "format": "ci16",
  "byte_order": "little_endian",
  "iq_layout": "interleaved_iq",
  "passthrough_variants": [
    { "sample_rate_hz": 1536000,  "bandwidth_hz": 1000000,  "file": "assets/cap_1m.c16" },
    { "sample_rate_hz": 24576000, "bandwidth_hz": 20000000, "file": "assets/cap_20m.c16" }
  ]
}
```

Signal fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `signal_id` | string | yes | Unique signal ID. |
| `source_reference` | string | yes | Must match an existing source `id`. |
| `modulation` | string | no | Defaults to `iq`. Use `iq` for `iq_file`; use `wbfm`, `am`, `usb`, or `lsb` for `audio_file`. |
| `center_frequency_hz` | integer | see notes | Absolute RF center frequency. Required except in `range` replay mode (where the content follows the tune and the field is ignored). |
| `bandwidth_hz` | integer | yes | Signal bandwidth. |
| `power_dbm` | number | yes | RF power. Digital level is `nominal_level_dbfs + (power_dbm - rf_reference_power_dbm)` before `output_scale`. |
| `fm_deviation_hz` | number | WBFM only | FM peak deviation. Defaults to `75000`. |
| `am_depth` | number | AM only | AM modulation depth from `0.0` to `1.0`. Defaults to `0.8`. |
| `start_time_s` | number | no | Scenario start time for playback. Defaults to `0`. |
| `repeat_interval_s` | number | unless `loop` | Burst repeat period. Required when `loop` is `false`; must not be combined with `loop: true` (`loop_repeat_conflict`). |
| `replay_mode` | string | no | `fixed` (default), `range`, or `shift`. See below. |
| `frequency_range` | object | range/shift | `{ "start_hz": ..., "stop_hz": ... }`. The tune interval in which the signal is active. |
| `loop` | bool | no | Continuous epoch-anchored looping of the file (no silent gap). Defaults to `true` for `range`/`shift`, `false` for `fixed`. IQ-file sources only. |
| `passthrough` | bool | no | Bypasses the mixer entirely for a channel dedicated to this one capture (see below). Requires `replay_mode` `range`/`shift`, `loop: true`, and a source with `passthrough_variants`. Defaults to `false`. |

Replay modes (IQ-file sources with `iq` modulation only):

- `fixed` — legacy behaviour: the signal sits at `center_frequency_hz` and is rendered whenever its passband overlaps the channel window.
- `range` — the file is played **centered at the tuned frequency** whenever the channel's effective center lies inside `frequency_range`; the output is identical anywhere in the range (like a capture that fills the channel), and silent outside it. With the noise floor enabled the noise still varies with the tune (its seed hashes the window center), so bit-identical output across the range requires `noise_floor` disabled.
- `shift` — the file content stays at its **absolute** RF position: while the tune is inside `frequency_range`, the IQ is rotated by `e^{j*2*pi*(center_frequency_hz - f_tune)*t}`. Tuning a channel to 110 MHz with the file configured at 100 MHz shows the content at −10 MHz offset. For channels whose rate is close to the source's this is a pure rotation with no band-limiting: content rotated past ±sample_rate/2 wraps around (accepted semantics for full-rate captures). For much narrower channels the DDC sub-band extraction below applies instead.

Looping playback position is an exact function of the absolute epoch-derived sample index (`position = output_sample_index * source_rate / output_rate mod file_length`), so independently started simulator instances emit identical samples at identical wall-clock times. The position wraps at midnight UTC together with the day-anchored timebase.

**DDC sub-band extraction** — a looping IQ replay rendered into a channel whose sample rate divides the source rate with an integer ratio **greater than 16** is treated as a hardware-style DDC: the source is frequency-shifted to place the channel center at baseband and decimated through an internally designed multi-stage Kaiser filter cascade (~80 dB alias rejection), so a 100 kHz channel tuned anywhere inside an 80 MHz recording carries exactly that sub-band. For `shift` mode at these ratios this **replaces** the wrap-around rotation semantics above — the channel receives the true band at its tune instead of a folded full-rate rotation. Ratios of 16 and below, non-integer ratios (startup prints a warning), burst (non-loop) signals, and audio sources keep the previous behaviour.

Per tune area the shifted-and-decimated front of the cascade is computed once over one full source loop and cached (see `ddc_cache_max_bytes`); retuning a DDC channel to a cold area stalls that one stream for the build (roughly a second per 10 s of loop at 98.304 MS/s), while retunes within ±10% of the intermediate rate of a previous tune reuse the cached entry immediately. Fixed-center DDC channels are prewarmed at startup. Caching requires the loop length to be divisible by the front decimation factor (startup warns otherwise, and the full cascade then runs every block).

**`passthrough`** — for a channel whose only job is to replay one capture, the general renderer (float mix bus, per-signal passband/gain machinery, noise floor) is overhead the use case doesn't need. A passthrough signal references a **variant source** (see `passthrough_variants` below): one capture file per supported channel sample rate. Whenever the signal is active for the tune, the renderer picks the variant whose `sample_rate_hz` equals the channel's current rate and streams it verbatim — a literal `memcpy` when `power_dbm == rf_reference_power_dbm` and `output_scale == 1.0`; a scaled copy otherwise; a frequency-rotated copy for shift mode away from its nominal center. It **never resamples**: passthrough is a copy, not a DSP path. This is meaningfully cheaper than even the mixer's own direct-copy fast path — roughly 15x less render time per block in local measurements — because it also skips the mix-bus zero/accumulate/saturate round trip and the noise-floor/other-signal bookkeeping the mixer always performs.

A channel whose current rate has **no matching variant** renders **silence** (all-zero payload), not a resampled approximation — so an 80 MHz channel replaying a capture set that only contains a 1 MHz and 20 MHz rendition simply goes quiet at 80 MHz. This makes bandwidth support explicit: you provide a file for each bandwidth you want to play back, and only those bandwidths produce output.

Because a passthrough channel bypasses the mixer completely, **any other signal that would otherwise be active for the same channel window is not rendered** while the passthrough signal is active — passthrough is exclusive by design, not an overlay. If you want to combine a replay capture with other signals or a noise floor, leave `passthrough` unset (or `false`) and use the general renderer instead (which resamples as needed); `range`/`shift` semantics and epoch-anchored looping work identically either way.

Optional `noise_floor` object:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `enabled` | bool | no | Defaults to `true` when the object is present. |
| `power_dbm_per_hz` | number | one of | Preferred. Noise power spectral **density**. In-window power is `power_dbm_per_hz + 10*log10(window_bandwidth_hz)`, so a narrow DDC and the wide stream carry the same density (constant dBm/Hz) rather than the same total power. |
| `power_dbm` | number | one of | Legacy. Total noise power in the window, independent of bandwidth. Kept for backward compatibility; prefer `power_dbm_per_hz`. |
| `seed` | integer | no | Selects the noise realization. Same seed + same scenario time yields identical noise across instances. Defaults to `1`. |

Exactly one of `power_dbm_per_hz` or `power_dbm` must be given when the noise floor is enabled; specifying both is an error (`noise_floor_conflicting_power`). The noise is Gaussian with unit crest factor, so the produced RMS matches the configured level (the earlier uniform noise ran ~4.8 dB low).

Validation error codes include:

- `scenario_invalid`
- `scenario_missing_id`
- `scenario_missing_arrays`
- `scenario_too_large`
- `source_invalid`
- `source_unsupported`
- `duplicate_source_id`
- `signal_invalid`
- `signal_modulation_invalid`
- `signal_modulation_source_mismatch`
- `duplicate_signal_id`
- `missing_source_reference`
- `signal_repeat_too_short`
- `signal_replay_mode_invalid`
- `signal_frequency_range_invalid`
- `replay_range_invalid`
- `replay_mode_source_mismatch`
- `replay_shift_missing_center`
- `loop_repeat_conflict`
- `loop_source_unsupported`
- `passthrough_requires_replay_mode`
- `passthrough_requires_loop`
- `passthrough_requires_variants`
- `variant_source_requires_passthrough`
- `passthrough_variants_invalid`
- `passthrough_variant_invalid`
- `passthrough_variant_duplicate_rate`
- `noise_floor_invalid`
- `noise_floor_conflicting_power`
