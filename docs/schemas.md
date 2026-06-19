# Config And Scenario Schemas

This document describes the schema implemented by `src/config.c` and `src/scenario.c`.

## Instance YAML

Top-level fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `schema_version` | integer | yes | Must be `1`. |
| `instance_id` | string | yes | Instance identifier for operators/logs. |
| `scenario_file` | string | yes | Default scenario path used when `--scenario` is omitted. |
| `log_path` | string | no | Reserved for runtime logging. |
| `stream_block_samples` | integer | no | UDP payload size in IQ samples. Defaults to `1024`; valid range is `1..4096`. |
| `receivers` | array | yes | At least one receiver, up to `SIM_MAX_RECEIVERS` (`12`). |

Receiver fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `receiver_id` | integer | yes | Unique per instance. |
| `rest_bind_host` | string | yes | REST bind address, for example `127.0.0.1`. |
| `rest_port` | integer | yes | Unique per instance. |
| `udp_output_host` | string | yes | UDP destination host. |
| `frequency_start_hz` | integer | yes | Inclusive RF range start, `0..40000000000`. |
| `frequency_stop_hz` | integer | yes | RF range stop, greater than start and `<= 40000000000`. |
| `scan_rate_hz_per_s` | number | yes | Used when range is wider than 80 MHz. |
| `output_scale` | number | no | Receiver output multiplier. Defaults to `1.0`; must be positive when set. |
| `stream_enabled` | boolean | no | Enables the 80-MHz UDP stream. Defaults to `true`. |
| `udp_80mhz_output_port` | integer | yes | Unique across all receiver/DDC UDP outputs. |
| `ddc` | array | yes | Four DDC entries with IDs `0..3`. |

DDC fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `ddc_id` | integer | yes | `0..3`. |
| `center_frequency_hz` | integer | yes | Absolute RF center frequency. |
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
- `invalid_output_scale`

## Scenario JSON

Top-level fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `schema_version` | integer | yes | Must be `1`. |
| `scenario_id` | string | yes | Scenario identifier. |
| `description` | string | no | Human-readable description. |
| `sources` | array | yes | IQ source definitions, up to `SIM_MAX_SOURCES` (`64`). |
| `signals` | array | yes | RF signal placements, up to `SIM_MAX_SIGNALS` (`256`). |

Source fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `id` | string | yes | Unique source ID. |
| `source_type` | string | yes | Currently `iq_file`. |
| `file` | string | yes | Path to the IQ asset. |
| `format` | string | yes | Currently `ci16`. |
| `byte_order` | string | yes | Currently `little_endian`. |
| `iq_layout` | string | yes | Currently `interleaved_iq`. |
| `sample_rate_hz` | integer | yes | Source sample rate. |
| `bandwidth_hz` | integer | yes | Source bandwidth. |
| `center_frequency_hz` | integer | yes | Source-relative center; current sample scenario uses `0`. |
| `sample_count` | integer | yes | Number of complex samples in the file. |
| `nominal_level_dbfs` | number | yes | Source nominal digital level. |

Signal fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `signal_id` | string | yes | Unique signal ID. |
| `source_reference` | string | yes | Must match an existing source `id`. |
| `center_frequency_hz` | integer | yes | Absolute RF center frequency. |
| `bandwidth_hz` | integer | yes | Signal bandwidth. |
| `power_dbm` | number | yes | RF power value; calibrated RF-to-dBFS modeling is still pending. |
| `start_time_s` | number | yes | Scenario start time for playback. |
| `repeat_interval_s` | number | yes | Must be positive enough to repeat the source without invalid wrapping. |

Validation error codes include:

- `scenario_invalid`
- `scenario_missing_id`
- `scenario_missing_arrays`
- `scenario_too_large`
- `source_invalid`
- `source_unsupported`
- `duplicate_source_id`
- `signal_invalid`
- `duplicate_signal_id`
- `missing_source_reference`
- `signal_repeat_too_short`
