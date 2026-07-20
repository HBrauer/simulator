# REST API

Each receiver runs its own HTTP server on `rest_bind_host:rest_port` (see
`docs/schemas.md`). All bodies are JSON. Errors use:

```json
{"error": {"code": "unsupported_channel_rate", "message": "requested {bandwidth_hz, sample_rate_hz} is not one of this channel's rates"}}
```

A client needs nothing but the base URL: `GET /api/v1/capabilities` describes the
receiver, its front-end bandwidth, and the channel count;
`GET /api/v1/channels` lists the channels (each with its active `bandwidth_hz`/`sample_rate_hz`
and its `rates` list of supported options) and their UDP ports. Every successful
mutation bumps `config_epoch` (visible in `/capabilities`, `/status`, `/config`), so
clients can poll it cheaply to detect changes made by others. Sample-rate and frequency
changes are additionally announced in-band via VITA 49.2 context packets
(`docs/vita49_udp.md`).

## Endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/health` | Liveness, version, uptime. |
| GET | `/api/v1/capabilities` | Frequency limits, front-end bandwidth, tuner state, channel count, IQ format, `config_epoch`. |
| GET | `/api/v1/channels` | Array of all channel states. |
| GET | `/api/v1/channels/{id}` | One channel state. |
| PUT | `/api/v1/channels/{id}` | Reconfigure a channel (partial update). |
| POST | `/api/v1/channels/{id}/stream` | `{"enabled": bool}` — start/stop the channel's UDP stream. |
| GET | `/api/v1/status` | Receiver-level status (tuner, mode, epoch) without channels. |
| GET | `/api/v1/config` | Status plus channels. |
| GET | `/api/v1/streams` | Per-channel stream status (ports, enabled/active, counters). |
| GET | `/api/v1/metrics` | Aggregate and per-channel throughput/error counters. |
| GET | `/api/v1/scenario/status` | Loaded scenario and current scenario time. |
| POST | `/api/v1/frequency-range` | Set the tuner: `frequency_min_hz`, `frequency_max_hz`, optional `scan_rate_hz_per_s`. |
| POST | `/api/v1/output-scale` | Set `output_scale` on the receiver and all channels. |

The pre-channel endpoints (`/api/v1/ddc/*`, `/api/v1/streams/80mhz`) were removed; use
`/api/v1/channels/{id}` and `/api/v1/channels/{id}/stream`.

## Channel object

Returned by `/channels`, `/channels/{id}`, and channel PUTs:

```json
{
  "channel_id": 1,
  "track_tuner": false,
  "center_frequency_hz": 10005000000,
  "configured_center_frequency_hz": 10005000000,
  "bandwidth_hz": 20000000,
  "sample_rate_hz": 24576000,
  "rates": [
    { "bandwidth_hz": 20000000, "sample_rate_hz": 24576000 },
    { "bandwidth_hz": 5000000, "sample_rate_hz": 6144000 }
  ],
  "in_frontend_window": true,
  "stream_enabled": true,
  "active": true,
  "udp_port": 50001,
  "output_scale": 1.0,
  "rf_reference_power_dbm": -55.0
}
```

`bandwidth_hz`/`sample_rate_hz` are the active pair; `rates` lists every `{bandwidth_hz,
sample_rate_hz}` option this channel can be tuned to. `center_frequency_hz` is the effective
center (for a tuner-tracking channel: the instantaneous tuner center);
`configured_center_frequency_hz` is the stored value. `in_frontend_window` reports whether the
channel currently carries signal; an out-of-window channel streams zeros.

## PUT /api/v1/channels/{id}

Any subset of:

| Field | Rules |
| --- | --- |
| `center_frequency_hz` | `0..simulator_frequency_max_hz`. Rejected (`invalid_request`) while the channel has `track_tuner: true` — clear it in the same request. |
| `bandwidth_hz` + `sample_rate_hz` | Set together (`invalid_request` otherwise). The pair must equal one of the channel's `rates` entries, else `unsupported_channel_rate`. |
| `track_tuner` | Boolean. |
| `output_scale` | Positive number. |

The update is validated against the full receiver configuration and applied atomically;
the response is the updated channel object. The streaming pipeline picks the change up
on the next block (ring depth ~8 blocks); a sample-rate change re-anchors the stream's
deterministic block grid and emits a context packet with the change indicator set.

Example — retune channel 1 and narrow it to 5 MHz / 6.144 MS/s:

```sh
curl -X PUT http://127.0.0.1:8100/api/v1/channels/1 \
  -d '{"center_frequency_hz": 10012000000, "bandwidth_hz": 5000000, "sample_rate_hz": 6144000}'
```
