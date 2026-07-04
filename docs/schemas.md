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
| `asset_cache_max_bytes` | integer | no | Maximum total in-memory IQ asset bytes. `0` means unlimited. Defaults to `0`. |
| `receivers` | array | yes | At least one receiver, up to `SIM_MAX_RECEIVERS` (`12`). |

Receiver fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `receiver_id` | integer | yes | Unique per instance. |
| `rest_bind_host` | string | yes | REST bind address, for example `127.0.0.1`. |
| `rest_port` | integer | yes | Unique per instance. |
| `udp_output_host` | string | yes | UDP destination host. May be unicast, for example `127.0.0.1`, or IPv4 multicast, for example `239.10.10.10`. |
| `udp_multicast_interface` | string | no | Local IPv4 interface used for multicast sends, for example `127.0.0.1` for loopback-only testing. Empty/default lets the kernel choose. |
| `frequency_start_hz` | integer | yes | Inclusive RF range start, `0..40000000000`. |
| `frequency_stop_hz` | integer | yes | RF range stop, greater than start and `<= 40000000000`. |
| `bandwidth_hz` | integer | no | Receiver RF window bandwidth. Defaults to `80000000`; must be non-zero. |
| `sample_rate_hz` | integer | no | Receiver IQ output sample rate. Defaults to `98304000`; must be non-zero. |
| `scan_rate_hz_per_s` | number | yes | Used when range is wider than the receiver bandwidth. |
| `output_scale` | number | no | Receiver output multiplier. Defaults to `1.0`; must be positive when set. |
| `rf_reference_power_dbm` | number | no | RF power that preserves the source's `nominal_level_dbfs`. Defaults to `-55.0`. |
| `stream_enabled` | boolean | no | Enables the 80-MHz UDP stream. Defaults to `true`. |
| `udp_80mhz_output_port` | integer | yes | Unique across all receiver/DDC UDP outputs. |
| `ddc` | array | yes | Four DDC entries with IDs `0..3`. |

DDC fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `ddc_id` | integer | yes | `0..3`. |
| `center_frequency_hz` | integer | yes | Absolute RF center frequency. |
| `bandwidth_hz` | integer | no | DDC RF bandwidth. Defaults to `20000000`; must be non-zero. |
| `sample_rate_hz` | integer | no | DDC IQ output sample rate. Defaults to `24576000`; must be non-zero. |
| `output_scale` | number | no | DDC output multiplier. Defaults to receiver `output_scale`. |
| `stream_enabled` | boolean | no | Enables this DDC UDP stream. Defaults to `true`. |
| `udp_output_port` | integer | yes | Unique across all receiver/DDC UDP outputs. |

Validation error codes include:

- `config_invalid`
- `too_many_receivers`
- `invalid_stream_block_samples`
- `duplicate_receiver`
- `duplicate_rest_port`
- `duplicate_udp_port`
- `invalid_receiver`
- `invalid_ddc`
- `invalid_sample_rate_or_bandwidth`
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
| `file` | string | yes | Path to the IQ or audio asset. |
| `format` | string | yes | `ci16` for IQ files, `wav` for audio files. |
| `byte_order` | string | IQ only | Currently `little_endian`. |
| `iq_layout` | string | IQ only | Currently `interleaved_iq`. |
| `sample_rate_hz` | integer | yes | Source sample rate. |
| `bandwidth_hz` | integer | yes | Source bandwidth. |
| `center_frequency_hz` | integer | yes | Source-relative center; current sample scenarios use `0`. |
| `sample_count` | integer | no | Number of complex/audio samples in the file. CI16 and WAV sources can derive this from file size. |
| `nominal_level_dbfs` | number | yes | Source nominal digital level. |

`audio_file` sources currently support PCM16 WAV, mono or stereo. Stereo is folded to mono in the asset cache.

Signal fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `signal_id` | string | yes | Unique signal ID. |
| `source_reference` | string | yes | Must match an existing source `id`. |
| `modulation` | string | no | Defaults to `iq`. Use `iq` for `iq_file`; use `wbfm`, `am`, `usb`, or `lsb` for `audio_file`. |
| `center_frequency_hz` | integer | yes | Absolute RF center frequency. |
| `bandwidth_hz` | integer | yes | Signal bandwidth. |
| `power_dbm` | number | yes | RF power. Digital level is `nominal_level_dbfs + (power_dbm - rf_reference_power_dbm)` before `output_scale`. |
| `fm_deviation_hz` | number | WBFM only | FM peak deviation. Defaults to `75000`. |
| `am_depth` | number | AM only | AM modulation depth from `0.0` to `1.0`. Defaults to `0.8`. |
| `start_time_s` | number | yes | Scenario start time for playback. |
| `repeat_interval_s` | number | yes | Must be positive enough to repeat the source without invalid wrapping. |

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
- `noise_floor_invalid`
- `noise_floor_conflicting_power`
