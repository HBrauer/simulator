# Implementation Plan: Unified Channels with REST-Configurable Bandwidth/Frequency

Status: implemented (see README, docs/rest_api.md, docs/schemas.md, docs/vita49_udp.md)
Date: 2026-07-06

Implementation notes that differ from the draft below:

- The determinism hash did **not** change: channel 0 (track_tuner, 80 MHz profile)
  reaches `renderer_render_window_block` with parameters identical to the old wideband
  path, and channel ids map 1:1 onto the old VITA stream numbering.
- The waterfall receiver's REST client is a small hand-rolled HTTP/1.1-over-socket
  client plus jansson (already a project dependency) instead of libcurl, removing the
  new-dependency risk entirely.
- T5 (VITA context packets) landed together with the streamer rework; the receiver
  consumes them to follow external retunes without polling REST.

## 1. Goal

Replace the hardcoded "one 80-MHz stream + 4 fixed DDCs" model with a unified **channel**
model:

- A receiver has N channels. Every channel is the same kind of object; the former
  wideband stream is just a channel configured with the widest profile.
- Each receiver declares a set of **supported profiles**, i.e. fixed
  `{bandwidth_hz, sample_rate_hz}` pairs (like real DDC decimation stages). Setting a
  channel's bandwidth selects the paired sample rate automatically.
- Channel **center frequency and bandwidth are settable at runtime via REST**.
- Everything a client needs is **discoverable via REST**: supported profiles, frequency
  limits, channel list, UDP ports, stream format. The waterfall receiver uses only the
  REST API to configure itself (no more `--sample-rate-hz` guessing).
- The waterfall receiver gets a control UI: select a channel, retune it, change its
  bandwidth.

## 2. Design decisions (agreed)

| Decision | Choice |
| --- | --- |
| BW/rate coupling | Fixed pairs (profiles); API sets bandwidth, rate follows |
| Where profiles live | Per-receiver in the instance YAML, with built-in defaults |
| Channel model | One channel type; no wideband/DDC distinction |
| Receiver app | Full control UI (REST client + toolbar controls) |

### 2.1 One channel type — how the wideband case fits

The only real differences between the current 80-MHz stream and a DDC are:

1. The 80-MHz stream's center follows the receiver tuner (fixed center or scan sweep).
2. DDCs render an empty stream when their span leaves the receiver window.

Both are preserved with two small, uniform concepts:

- **Front end (per receiver):** keeps today's `frequency_start_hz` / `frequency_stop_hz`
  / `scan_rate_hz_per_s` tuner plus an instantaneous analog window
  `frontend_bandwidth_hz` (today's receiver `bandwidth_hz`, e.g. 80 MHz). This models the
  ADC: all channels extract from within this window.
- **Channel flag `track_tuner`:** if true, the channel's center is the tuner center at
  render time (this is the old wideband stream; it scans with the tuner). If false, the
  channel has its own fixed `center_frequency_hz` (old DDC behavior).

The in-window rule becomes uniform for all channels: a channel renders signal iff its
span fits inside the front-end window, else it streams empty blocks (today's DDC
out-of-window behavior). A `track_tuner` channel with `bandwidth_hz <=
frontend_bandwidth_hz` is always in-window by construction. Validation rejects any
profile selection wider than the front end.

Default config reproduces today's behavior exactly: 5 channels — channel 0 =
`track_tuner`, 80 MHz / 98.304 MS/s profile, port 50000; channels 1–4 = fixed-center
20 MHz / 24.576 MS/s, ports 50001–50004.

## 3. Data model changes (`sim_types.h`)

```c
#define SIM_MAX_CHANNELS 8
#define SIM_MAX_PROFILES 16

typedef struct {
    uint32_t bandwidth_hz;      /* passband, key for selection */
    uint32_t sample_rate_hz;    /* fixed pair partner */
    char     name[24];          /* optional label, e.g. "20M" */
} channel_profile_t;

typedef struct {
    uint32_t id;
    bool     track_tuner;            /* center follows the receiver tuner */
    uint64_t center_frequency_hz;    /* used when !track_tuner */
    size_t   profile_index;          /* into receiver profiles[] */
    double   output_scale;
    double   rf_reference_power_dbm;
    bool     stream_enabled;
    udp_output_config_t udp_output;
} channel_config_t;
```

`receiver_config_t` changes:

- remove `ddc[SIM_DDC_COUNT]`, `udp_80mhz_output`, receiver-level `sample_rate_hz`;
- rename receiver `bandwidth_hz` → `frontend_bandwidth_hz` (semantics unchanged);
- add `size_t profile_count; channel_profile_t profiles[SIM_MAX_PROFILES];`
- add `size_t channel_count; channel_config_t channels[SIM_MAX_CHANNELS];`
- add `uint64_t config_epoch;` — incremented under `receiver_lock` on every successful
  runtime change; exposed via REST so clients can cheaply detect changes.

Profiles are sorted and validated at load: unique bandwidths, `sample_rate_hz >=
bandwidth_hz`, at least one profile `<= frontend_bandwidth_hz`. Built-in default profile
table when the YAML omits `profiles`: `{80 MHz, 98.304 MS/s}`, `{40 MHz, 49.152 MS/s}`,
`{20 MHz, 24.576 MS/s}`, `{10 MHz, 12.288 MS/s}`, `{5 MHz, 6.144 MS/s}`,
`{1 MHz, 1.536 MS/s}`.

### 3.1 Instance YAML (new format)

```yaml
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: 8100
    udp_output_host: "127.0.0.1"
    frequency_start_hz: 9960000000
    frequency_stop_hz: 10040000000
    frontend_bandwidth_hz: 80000000
    scan_rate_hz_per_s: 100000000000
    rf_reference_power_dbm: -55.0
    profiles:
      - { bandwidth_hz: 80000000, sample_rate_hz: 98304000, name: "80M" }
      - { bandwidth_hz: 20000000, sample_rate_hz: 24576000, name: "20M" }
      - { bandwidth_hz: 5000000,  sample_rate_hz: 6144000,  name: "5M" }
    channels:
      - { channel_id: 0, track_tuner: true,  bandwidth_hz: 80000000, udp_output_port: 50000, stream_enabled: true }
      - { channel_id: 1, center_frequency_hz: 10005000000, bandwidth_hz: 20000000, udp_output_port: 50001, stream_enabled: true }
      # ...
```

Channels reference profiles by `bandwidth_hz` (must match a profile exactly). Old-format
YAML keys (`ddc:`, `udp_80mhz_output_port`, receiver `sample_rate_hz`/`bandwidth_hz`)
produce a **clear config error naming the replacement key** — clean break, no silent
remapping; in-repo configs are migrated in the same commit.

## 4. REST API

New endpoints (all JSON, same error envelope as today):

```text
GET  /api/v1/capabilities
GET  /api/v1/channels
GET  /api/v1/channels/{id}
PUT  /api/v1/channels/{id}
POST /api/v1/channels/{id}/stream     # {"enabled": bool} (kept separate, cheap toggle)
```

`GET /api/v1/capabilities` — everything a client needs before touching a channel:

```json
{
  "receiver_id": 0,
  "frequency_min_hz": 0,
  "frequency_max_hz": 40000000000,
  "frontend_bandwidth_hz": 80000000,
  "tuner": { "frequency_start_hz": ..., "frequency_stop_hz": ..., "mode": "fixed|scan", "scan_rate_hz_per_s": ... },
  "channel_count": 5,
  "profiles": [ { "bandwidth_hz": 80000000, "sample_rate_hz": 98304000, "name": "80M" }, ... ],
  "iq_format": "vita49_2_ci16",
  "udp_output_host": "127.0.0.1",
  "config_epoch": 17
}
```

`GET /api/v1/channels` — array of full channel status:

```json
[ { "channel_id": 0, "track_tuner": true, "center_frequency_hz": 10000000000,
    "bandwidth_hz": 80000000, "sample_rate_hz": 98304000, "profile": "80M",
    "in_frontend_window": true, "stream_enabled": true, "active": true,
    "udp_port": 50000, "output_scale": 1.0, "rf_reference_power_dbm": -55.0 }, ... ]
```

`PUT /api/v1/channels/{id}` — partial update; any subset of:

```json
{ "center_frequency_hz": 10012000000, "bandwidth_hz": 20000000,
  "track_tuner": false, "output_scale": 0.5 }
```

Rules:

- `bandwidth_hz` must exactly match a supported profile → else `400
  unsupported_bandwidth` listing supported values in the message.
- selected profile bandwidth must be `<= frontend_bandwidth_hz` → `400 bandwidth_exceeds_frontend`.
- `center_frequency_hz <= frequency_max_hz`; ignored (400) together with `track_tuner: true`.
- Applied atomically under `receiver_lock`, validated on a copy first
  (`receiver_validate` pattern already used by `/frequency-range`), `config_epoch++`.
- Response: the updated channel object (same shape as GET), so a client never needs a
  follow-up read.

Existing endpoints: `/health`, `/status`, `/config`, `/metrics`, `/scenario/status`,
`/frequency-range`, `/output-scale` stay; `/status`, `/config`, `/streams` and
`/metrics` are reworked to report `channels[]` instead of `iq_80mhz` + `ddc[]`.
`/api/v1/ddc/*` and `/api/v1/streams/80mhz` are **removed** (the in-repo receiver and
tests are the only clients; both are updated in the same change). `stream_type` in
metrics becomes `"channel"` with `stream_id` = channel id.

## 5. Simulator runtime changes

### 5.1 Renderer unification

`renderer_render_80mhz_block()` and `renderer_render_ddc_block()` merge into one:

```c
bool renderer_render_channel_block(const scenario_t *sc, const asset_cache_t *cache,
    const receiver_config_t *rx, const channel_config_t *ch,
    uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats);
```

It computes the effective center (tuner center if `track_tuner`, else channel center),
applies the uniform in-window gate, and renders — both existing bodies are already the
same "window at (center, rate)" operation, so this is mostly a merge, not new DSP.
`--render-once` in `main.c` renders channel 0. **The README determinism hash stays valid
only if the merged path is bit-identical; verify with the render-once hash and the
resampler golden test, and update the README hash if ordering changes.**

### 5.2 Streamer / runtime reconfiguration

One worker pair (render + UDP) per channel, `channel_count` of them (today: fixed
`1 + SIM_DDC_COUNT`). Runtime changes ride the existing per-block snapshot mechanism:

- **Rate change:** the block-grid re-anchor on rate change already exists
  (`streamer.c:301`); pacing (`sleep_for_block`) reads the snapshot rate. Add an
  integration test that flips a channel between two profiles and checks
  `actual_sample_rate_sps` converges to the new rate and VITA timestamps stay on the new
  grid.
- **Buffers:** ring entries are `block_samples * sizeof(iq_ci16)` — rate-independent, no
  change. Renderer resampler scratch/kernels must be sized for the **max profile rate**
  at startup (profiles are a bounded set, so this is cheap and avoids hot-path
  allocation).
- **Atomicity:** REST writes `{profile_index, center, track_tuner}` in one locked
  update; a block is rendered entirely under one snapshot, so a retune takes effect on a
  block boundary within ring depth (~8 blocks), matching the documented live-retune
  semantics.

### 5.3 VITA 49.2

`vita49_stream_id(receiver_id, is_ddc, ddc_id)` → `vita49_stream_id(receiver_id,
channel_id)`. Update `docs/vita49_udp.md` with the new numbering (old wideband id maps
to channel 0's id).

**Improvement (T5): VITA 49.2 context packets.** Emit a context packet on each channel
stream at stream start, on every configuration change, and periodically (~1 s), carrying
sample rate, bandwidth, and RF reference frequency. This is how real VITA 49.2 devices
announce retunes, and it lets any UDP consumer detect rate/frequency changes without
polling REST. The waterfall receiver uses it to resync instantly.

## 6. Waterfall receiver: REST client + control UI

New meson dependency: `libcurl` (required for the receiver target; the simulator does
not link it).

- **`--control-url http://host:port`** replaces `--port`/`--sample-rate-hz` when given
  (manual flags remain for REST-less use). On startup: `GET /capabilities` +
  `GET /channels`, pick the initial channel (`--channel N`, default 0), read its UDP
  port + sample rate + bandwidth, then start the UDP consumer.
- **Channel selection UI:** toolbar gains a channel selector (`< ch0 >`), a
  center-frequency readout with retune controls (arrow buttons step by 1/10/···; a
  click on the readout allows typed entry), and a bandwidth button cycling the supported
  profiles from `capabilities`. Each action issues `PUT /channels/{id}` and applies the
  response.
- **Dynamic reconfiguration:** on sample-rate change, recompute stride/history
  (`waterfall_stride_for_history` / `waterfall_history_for_stride` already exist),
  flush the waterfall history, and keep the FFT size. On channel switch, close and
  reopen the UDP socket (re-join multicast group when applicable) on the new port.
- **Change detection:** parse VITA context packets when present (T5); additionally poll
  `GET /capabilities` `config_epoch` at ~1 Hz and refresh channel state when it moves,
  so retunes made by other clients are reflected.
- JSON parsing: the simulator already depends on jansson; reuse it in the receiver.

## 7. Task breakdown

Each task ends with green `meson test`, cppcheck, clang-tidy, and an updated docs
section where relevant.

- **T1 — Data model + config.** `channel_profile_t`/`channel_config_t`, YAML parsing
  (new keys, hard errors for old keys), default profile table, validation, migrate
  in-repo configs. Unit tests: profile parsing/sorting/dedup, channel↔profile
  resolution, validation failures, old-key error messages.
- **T2 — Unified render/stream path.** Merge renderer entry points, N-channel streamer,
  new stream-id scheme, metrics arrays keyed by channel. Verify determinism hash;
  update README hash if it legitimately changes. Unit tests: in-window gate incl.
  `track_tuner`; benchmark run to confirm no throughput regression.
- **T3 — REST API.** `/capabilities`, `/channels` CRUD, reworked `/status` `/config`
  `/streams` `/metrics`, removal of `/ddc/*` + `/streams/80mhz`, `config_epoch`.
  Integration tests: capability discovery, retune roundtrip, unsupported-bandwidth
  rejection, epoch bump, concurrent PUT + streaming smoke test.
- **T4 — Runtime rate-change hardening.** Max-rate scratch sizing, grid re-anchor
  integration test (profile flip while streaming; assert `actual_sample_rate_sps`,
  timestamp continuity, no worker errors).
- **T5 — VITA 49.2 context packets.** Emit on start/change/periodic; receiver parses
  them; document in `docs/vita49_udp.md`. Unit test: context packet encode/decode.
- **T6 — Receiver REST client + control UI.** libcurl+jansson client, `--control-url`,
  channel selector, retune + bandwidth controls, dynamic stride/socket handling.
  Manual verification against the simulator (`/run` flow) + a small pytest that drives
  the REST endpoints the receiver uses.
- **T7 — Docs & polish.** README quick start rewritten around channel discovery,
  `docs/schemas.md` (YAML + REST), `docs/architecture.md`, release checklist entry.

Suggested order: T1 → T2 → T3 → T4 are sequential; T5 and T6 can proceed in parallel
after T3 (T6 gains context-packet support when T5 lands).

## 8. Additional improvements (recommended, mostly folded into tasks above)

1. **`config_epoch`** for cheap client-side change detection (T3).
2. **VITA 49.2 context packets** — the standard in-band way to announce retunes (T5).
3. **Structured validation errors** — `error.code` plus the offending field and the
   list of supported values, so UI clients can render useful messages (T3).
4. **OpenAPI document** (`docs/openapi.yaml`) generated by hand for the v1 API; keeps
   the receiver and any future clients honest (T7).
5. **Aggregate-rate guard:** validate at load and on PUT that the sum of active channel
   sample rates stays under a configurable ceiling (`max_total_sample_rate_sps`,
   default off) so a REST client cannot reconfigure the instance into guaranteed
   overrun (T3/T4).
6. **Later (out of scope):** persisting runtime changes back to YAML
   (`POST /config/save`), per-channel gain/impairments, SSE push notifications instead
   of epoch polling, variable channel count at runtime (add/remove channels via REST).

## 9. Risks / notes

- **Determinism hash will likely change** in T2 (render order/stream ids); this is
  expected and the README hash gets updated once, with the resampler golden test as the
  guard against unintended DSP changes.
- **Sum-of-rates performance:** 5 default channels = same total as today. The perf
  claim in README (196.6 MS/s on the test host) still bounds what YAML authors can
  configure; the aggregate-rate guard (item 5) makes over-subscription an explicit
  choice.
- **Receiver UDP socket churn** on channel switch: brief gap in the waterfall is
  acceptable; document it.
- **libcurl availability:** standard on all target distros; meson `dependency('libcurl')`
  with a clear error if missing.
