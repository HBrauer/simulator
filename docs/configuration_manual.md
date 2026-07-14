# Configuration Manual

A practical guide to configuring the SDR simulator, with worked examples. For the exhaustive
field-by-field reference (every key, type, and error code) see [`schemas.md`](schemas.md); this
document explains how the pieces fit together and how to express common setups.

There are two configuration files:

| File | Format | Describes | Parsed by |
| --- | --- | --- | --- |
| **Instance** | YAML | The *receiver* hardware: tuner range, front-end, output channels, UDP/REST | `src/config.c` |
| **Scenario** | JSON | The *RF environment*: signal sources and where/when they appear on the air | `src/scenario.c` |

They are independent: one scenario can be replayed by many instance configs, and vice versa.
You run them together:

```sh
./build/sdr-simulator --config simulator/configs/instance_001.yaml \
                      --scenario simulator/scenarios/benchmark_load.json
```

If `--scenario` is omitted, the instance's `scenario_file` is used.

---

## 1. The mental model

Three concepts, in order of the signal path:

1. **Source** — a piece of raw content: an IQ recording (`iq_file`) or an audio clip
   (`audio_file`). A source has *no* position in frequency or time on its own; it is just samples.
2. **Signal** — places a source on the air: at some RF frequency, at some power, with some
   modulation, appearing at some time (once, repeating, or continuously). Many signals can
   reference the same source.
3. **Channel** — an output stream (a receiver tuned to a bandwidth). The simulator renders,
   for each channel, the sum of every signal whose passband overlaps that channel's window,
   plus the noise floor.

> Sources and signals live in the **scenario**. Channels live in the **instance**. A signal
> becomes audible on a channel only when their frequency spans overlap.

---

## 2. Scenario JSON

### 2.1 Skeleton

```json
{
  "schema_version": 1,
  "scenario_id": "my_scenario",
  "description": "optional human-readable text",
  "noise_floor": { "enabled": true, "power_dbm_per_hz": -160.0, "seed": 12345 },
  "sources": [ /* ... */ ],
  "signals": [ /* ... */ ]
}
```

### 2.2 Sources

**IQ file** — complex baseband samples, interleaved `ci16` (little-endian I,Q pairs):

```json
{
  "id": "fsk_iq",
  "source_type": "iq_file",
  "file": "simulator/assets/fsk_20mhz.c16",
  "format": "ci16",
  "byte_order": "little_endian",
  "iq_layout": "interleaved_iq",
  "sample_rate_hz": 24576000,
  "bandwidth_hz": 20000000,
  "center_frequency_hz": 0,
  "sample_count": 24576,
  "nominal_level_dbfs": -12.0
}
```

`sample_count` may be omitted — it is derived from the file size.

**Audio file** — a PCM16 WAV (mono, or stereo folded to mono), to be modulated onto a carrier:

```json
{
  "id": "radio_clip_wav",
  "source_type": "audio_file",
  "file": "simulator/assets/radio_clip.wav",
  "format": "wav",
  "sample_rate_hz": 48000,
  "bandwidth_hz": 200000,
  "center_frequency_hz": 0,
  "nominal_level_dbfs": -6.0
}
```

### 2.3 Signals

A signal binds a source to a place on the air:

```json
{
  "signal_id": "iq_lower",
  "source_reference": "fsk_iq",
  "modulation": "iq",
  "center_frequency_hz": 9985000000,
  "bandwidth_hz": 20000000,
  "power_dbm": -60.0,
  "start_time_s": 0.0,
  "repeat_interval_s": 1.0
}
```

**Modulations**: `iq` (for `iq_file` sources — replayed as-is), and `wbfm`, `am`, `usb`, `lsb`
(for `audio_file` sources — the audio is modulated onto the carrier). WBFM honours
`fm_deviation_hz` (default 75 kHz); AM honours `am_depth` (0.0–1.0, default 0.8).

**Power**: the digital level written to the channel is
`nominal_level_dbfs + (power_dbm − rf_reference_power_dbm)`, then scaled by the channel's
`output_scale`. `rf_reference_power_dbm` comes from the instance config (default −55 dBm) and is
the RF power at which a source plays back at its nominal digital level.

---

## 3. The timing model — how inputs repeat

This is the part most worth understanding. **Every signal is one of two kinds, and the kind is
chosen by a single fact: whether `repeat_interval_s` is present.** There is no separate `loop`
switch — the two ideas are the same choice, so the configuration expresses it once.

### 3.1 Burst — `repeat_interval_s` is present

The source plays **once** starting at `start_time_s`, runs for its natural duration, then goes
**silent** until the next multiple of `repeat_interval_s`, and repeats. The silent gap is
`repeat_interval_s − source_duration`.

```
repeat_interval_s = 1.0, source is ~1 ms long:

 |█_______________________|█_______________________|█________ ...
  ↑ play once (~1 ms)       ↑ replay from sample 0
  └──── silent ~999 ms ────┘
  0 s                      1 s                      2 s
```

Rules:
- `repeat_interval_s` must be `> 0`.
- It must be `>=` the source duration, otherwise the plays would overlap
  (error `signal_repeat_too_short`).
- Works for **both** IQ and audio sources.

Example — the same audio clip as a station that re-broadcasts every 30 seconds:

```json
{
  "signal_id": "wbfm_station",
  "source_reference": "radio_clip_wav",
  "modulation": "wbfm",
  "center_frequency_hz": 10005000000,
  "bandwidth_hz": 200000,
  "power_dbm": -65.0,
  "fm_deviation_hz": 75000,
  "start_time_s": 0.0,
  "repeat_interval_s": 30.0
}
```

To play something essentially once, set a very large interval (e.g. `86400.0` = once a day).

### 3.2 Loop — `repeat_interval_s` is absent

The source is played **back-to-back with no gap**, wrapping seamlessly at the file boundary
(the end of the file is immediately followed by its start). Playback position is anchored to the
epoch timebase, so two independently started instances emit identical samples at identical
wall-clock times.

```
no repeat_interval_s:

 |████████████████████████████████████████████████████████ ...
  end-of-file → start-of-file, continuous, forever
```

Rules:
- **IQ-file sources only** (audio cannot loop: `loop_source_unsupported`).
- Cannot also carry a `repeat_interval_s` — that is what distinguishes it from a burst.

Example — a continuous IQ emitter with no gaps:

```json
{
  "signal_id": "iq_beacon",
  "source_reference": "fsk_iq",
  "modulation": "iq",
  "center_frequency_hz": 10005000000,
  "bandwidth_hz": 20000000,
  "power_dbm": -60.0,
  "start_time_s": 0.0
}
```

### 3.3 Why there is no `loop: false`

A common question: *why not just set `repeat_interval_s: 0` to mean "loop"?* Because a repeat
interval is a **period**, and a period of zero is a division by zero, not "no gap." "No gap" is a
different thing entirely — it's the loop model, which you get by **omitting** the interval. So:

| You want | Set |
| --- | --- |
| Play once per N seconds, silent between | `repeat_interval_s: N` |
| Play continuously with no gap | omit `repeat_interval_s` (IQ only) |

Omitting the interval on an **audio** source is an error (`loop_source_unsupported`), because
audio can only be used as a finite, recurring burst.

---

## 4. Replay modes (IQ + `iq` modulation only)

By default a signal sits at a fixed frequency. Replay modes make an IQ recording behave like a
live capture that follows the receiver's tuner. All replay-mode signals are **loops** (they
stream continuously as you tune across them); a `repeat_interval_s` there is rejected
(`replay_mode_no_repeat`).

| `replay_mode` | Behaviour |
| --- | --- |
| `fixed` (default) | The signal sits at `center_frequency_hz`; rendered when its passband overlaps the channel. |
| `range` | The file is played **centered on the tuned frequency** whenever the channel center is inside `frequency_range`; identical output anywhere in the range, silent outside. |
| `shift` | The file content stays at its **absolute** RF position; while tuned inside `frequency_range` the IQ is rotated by `e^{j·2π·(center_frequency_hz − f_tune)·t}`. |

```json
{
  "signal_id": "replay_range_fm",
  "source_reference": "fsk_iq",
  "modulation": "iq",
  "replay_mode": "range",
  "frequency_range": { "start_hz": 9990000000, "stop_hz": 10010000000 },
  "bandwidth_hz": 20000000,
  "power_dbm": -55.0
}
```

Note `center_frequency_hz` is omitted here — in `range` mode the content follows the tune, so it
is ignored. (`shift` mode still requires it: it is the content's true RF home.)

A narrow channel extracting from a wideband looping replay (integer rate ratio > 16) is rendered
as a hardware-style DDC — see the "DDC sub-band extraction" section in [`schemas.md`](schemas.md).

---

## 5. Passthrough (dedicated replay channels)

When a channel's only job is to replay one capture verbatim, `passthrough: true` bypasses the
mixer entirely (no float mix bus, no noise floor, no other signals — a `memcpy` at unit gain).
It requires:
- `replay_mode` of `range` or `shift` (`passthrough_requires_replay_mode`),
- a **looping** signal — no `repeat_interval_s` (`passthrough_requires_loop`),
- a source with `passthrough_variants` — one capture file per channel sample rate
  (`passthrough_requires_variants`).

A channel whose current rate has no matching variant renders **silence**, not a resampled
approximation. See [`schemas.md`](schemas.md) for the full `passthrough_variants` example and the
performance rationale.

---

## 6. Noise floor

Optional; applied to every channel that isn't a passthrough. Prefer the spectral **density**
form so a narrow channel and the wide stream carry the same dBm/Hz:

```json
"noise_floor": { "enabled": true, "power_dbm_per_hz": -160.0, "seed": 12345 }
```

In-window power is `power_dbm_per_hz + 10·log10(window_bandwidth_hz)`. The legacy `power_dbm`
form (total power, bandwidth-independent) is still accepted but you must give exactly one of the
two (`noise_floor_conflicting_power` otherwise). `seed` makes the noise reproducible across
instances.

---

## 7. Instance YAML

The instance describes the receiver and its output channels.

```yaml
schema_version: 1
instance_id: "sim_instance_001"
scenario_file: "simulator/scenarios/test_scenario_001.json"
stream_block_samples: 1024        # CI16 samples per VITA 49.2 packet (1..4096)
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: 8100
    udp_output_host: "127.0.0.1"   # unicast or IPv4 multicast (e.g. 239.10.10.10)
    frequency_start_hz: 9960000000
    frequency_stop_hz:  10040000000
    frontend_bandwidth_hz: 80000000  # instantaneous window all channels extract from
    scan_rate_hz_per_s: 100000000000
    rf_reference_power_dbm: -55.0
    profiles:                        # allowed {bandwidth, sample_rate} pairs
      - { bandwidth_hz: 80000000, sample_rate_hz: 98304000, name: "80M" }
      - { bandwidth_hz: 20000000, sample_rate_hz: 24576000, name: "20M" }
      - { bandwidth_hz:  1000000, sample_rate_hz:  1536000, name: "1M"  }
    channels:
      - channel_id: 0
        track_tuner: true            # center follows the tuner (the "wideband" stream)
        bandwidth_hz: 80000000
        udp_output_port: 50000
      - channel_id: 1
        center_frequency_hz: 10005000000   # fixed-center channel
        bandwidth_hz: 20000000
        udp_output_port: 50001
```

Key points:
- A **channel's sample rate is not set directly** — it comes from the profile whose
  `bandwidth_hz` matches. Setting `sample_rate_hz` on a channel is an error.
- `track_tuner: true` makes the channel center follow the tuner (fixed center or scan sweep);
  otherwise give an absolute `center_frequency_hz`.
- A channel whose span leaves the front-end window (tuner center ± `frontend_bandwidth_hz/2`)
  keeps streaming, but empty — like a hardware DDC tuned outside the digitised band.
- `udp_output_port` must be unique across the instance's channels.

See [`schemas.md`](schemas.md) for every receiver/profile/channel field and the DDC cache tuning
knobs (`asset_cache_max_bytes`, `ddc_cache_max_bytes`).

---

## 8. Worked example: `benchmark_load.json`

The shipped [`benchmark_load.json`](../simulator/scenarios/benchmark_load.json) puts four signals
on the air over a −160 dBm/Hz noise floor, all using the **burst** timing model (each has a
`repeat_interval_s`), from two sources:

| Signal | Source | Modulation | Center | Repeats every | Timing kind |
| --- | --- | --- | --- | --- | --- |
| `iq_lower` | `fsk_iq` (IQ) | `iq` | 9.985 GHz | 1 s | burst |
| `iq_upper` | `fsk_iq` (IQ) | `iq` | 10.025 GHz | 1 s | burst |
| `wbfm_station` | `radio_clip_wav` (audio) | `wbfm` | 10.005 GHz | 30 s | burst |
| `am_station` | `radio_clip_wav` (audio) | `am` | 10.012 GHz | 86400 s | burst (effectively once) |

Note the same IQ source feeds two signals at different frequencies and powers, and the same audio
clip feeds both an FM and an AM station. To turn `iq_lower` into a **continuous** emitter instead
of a 1-second burst, you would simply delete its `"repeat_interval_s": 1.0` line (it is an IQ
source, so it may loop).

---

## 9. Validation

Both files are validated at load; a failure prints an error code and the simulator refuses to
start. The full list of codes is in [`schemas.md`](schemas.md). The timing-related ones:

| Error | Cause |
| --- | --- |
| `signal_repeat_too_short` | `repeat_interval_s` is shorter than the source duration. |
| `loop_source_unsupported` | A looping (no-interval) signal references an audio source. |
| `replay_mode_no_repeat` | A `range`/`shift` replay signal carries a `repeat_interval_s`. |
| `passthrough_requires_loop` | A `passthrough` signal carries a `repeat_interval_s`. |
