# Configuration Manual

A practical guide to configuring the SDR simulator, with worked examples. For the exhaustive
field-by-field reference (every key and error code) see [`schemas.md`](schemas.md); this document
explains how the pieces fit together and how to express common setups.

There are two configuration files:

| File | Format | Describes | Parsed by |
| --- | --- | --- | --- |
| **Instance** | YAML | The *receiver* hardware: tuner range, front-end, output channels, UDP/REST | `src/config.c` |
| **Scenario** | YAML | The *RF environment*: signal sources and where/when they appear on the air | `src/scenario.c` |

They are independent: one scenario can be replayed by many instance configs, and vice versa.
You run them together:

```sh
./build/sdr-simulator --config simulator/configs/receiver_scanner.yaml \
                      --scenario simulator/tests/benchmarks/benchmark_load.yaml
```

If `--scenario` is omitted, the instance's `scenario_file` is used.

---

## 1. The mental model

Three concepts, in order of the signal path:

1. **Source** — a piece of raw content: an IQ recording (`iq_file`) or an audio clip
   (`audio_file`). A source has *no* position in frequency or time on its own; it is just samples.
2. **Signal** — places a source on the air: at some RF frequency, at some power, with some
   modulation, appearing at some time (repeating in bursts, or continuously). Many signals can
   reference the same source.
3. **Channel** — an output stream (a receiver tuned to a bandwidth). The simulator renders,
   for each channel, the sum of every signal whose passband overlaps that channel's window,
   plus the noise floor.

> Sources and signals live in the **scenario**. Channels live in the **instance**. A signal
> becomes audible on a channel only when their frequency spans overlap.

---

## 2. Parameter reference

At-a-glance tables for every parameter. The sections that follow explain the behaviour in detail.

### Scenario — top level

| Parameter | Type / range | Description |
| --- | --- | --- |
| `schema_version` | integer, must be `1` | Required. |
| `scenario_id` | string | Required. Identifier for logs/REST. |
| `description` | string | Optional, human-readable. |
| `noise_floor` | object | Optional. See the noise-floor table. |
| `sources` | array (≤ 64) | Required. Source definitions. |
| `signals` | array (≤ 256) | Required. Signal placements. |

### Scenario — `noise_floor`

| Parameter | Type / range | Description |
| --- | --- | --- |
| `enabled` | bool | Defaults to `true` when the object is present. |
| `power_dbm_per_hz` | number (dBm/Hz) | Spectral density. Required when the noise floor is enabled. In-window power is `power_dbm_per_hz + 10·log10(window_bandwidth_hz)`. |
| `seed` | integer | Noise realization; identical across instances for a given seed. Default `1`. |

### Scenario — source

| Parameter | Type / range | Description |
| --- | --- | --- |
| `id` | string | Required, unique. |
| `source_type` | `iq_file` \| `audio_file` | Required. |
| `file` | string (path) | Required (unless `passthrough_variants` is given). |
| `format` | `ci16` \| `cf32` (IQ) \| `wav` (audio) | Required. `cf32` is interleaved little-endian `float32` (±1.0 full scale), converted to ci16 at load. |
| `byte_order` | `little_endian` | IQ only. |
| `iq_layout` | `interleaved_iq` | IQ only. |
| `sample_rate_hz` | integer (Hz) | Required. Source sample rate. |
| `bandwidth_hz` | integer (Hz) | Required. Intrinsic bandwidth of the content. |
| `center_frequency_hz` | integer (Hz) | Source-relative center; `0` in current scenarios. |
| `nominal_level_dbfs` | number (dBFS) | Required. Declarative metadata only — **not applied** to IQ replay; the file's real sample amplitude sets the level. See [§3.3 Power](#33-signals) for the clipping implications. |
| `passthrough_variants` | array | Optional. One capture per channel rate (see Passthrough). |

The sample count is **not** a parameter — it is derived from the file at load (CI16: file size / 4
bytes per complex sample; WAV: the header's frame count).

### Scenario — signal

| Parameter | Type / range | Description |
| --- | --- | --- |
| `signal_id` | string | Required, unique. |
| `source_reference` | string | Required. Must match a source `id`. |
| `modulation` | `iq` \| `wbfm` \| `am` \| `usb` \| `lsb` | Default `iq`. `iq` for IQ sources; the rest for audio. |
| `center_frequency_hz` | integer (Hz) | Absolute RF center. Required except in `range` replay mode. |
| `bandwidth_hz` | integer (Hz) | On-air footprint. Required for audio; optional for IQ (defaults to the source's bandwidth). |
| `power_dbm` | number (dBm) | Absolute RF power. Optional — omit to set power via `snr_db`. Mutually exclusive with `snr_db`. |
| `snr_db` | number (dB) | RF power as dB above the in-band noise (`noise_dbm_per_hz + 10·log10(bandwidth_hz) + snr_db`). Mutually exclusive with `power_dbm`. If neither is set, defaults to `+20 dB` SNR so the signal is always visible; with the noise floor disabled a base density of `-110 dBm/Hz` is assumed. |
| `fm_deviation_hz` | number (Hz) | WBFM only. Peak deviation, default `75000`. |
| `am_depth` | number, `0.0`–`1.0` | AM only. Modulation depth, default `0.8`. |
| `start_time_s` | number, `0` ≤ t < `86400` | First playback time. Default `0`. |
| `repeat_interval_s` | number, `> 0` | Present → recurring burst with this period; absent → continuous playback. See "Timing model". |
| `replay_mode` | `fixed` \| `range` \| `shift` | Default `fixed`. IQ sources only. |
| `frequency_range` | object `{start_hz, stop_hz}` | Required for `range`/`shift`. Tune interval in which the signal is active. |
| `passthrough` | bool | Default `false`. Dedicated verbatim-replay channel (see Passthrough). |

### Instance — top level

| Parameter | Type / range | Description |
| --- | --- | --- |
| `schema_version` | integer, must be `1` | Required. |
| `instance_id` | string | Required. |
| `scenario_file` | string (path) | Required. Default scenario when `--scenario` is omitted. |
| `log_path` | string | Optional. |
| `stream_block_samples` | integer, `1`–`4096` | CI16 samples per VITA 49.2 packet. Default `1024` (use `1536` for the MTU-9000 profile). |
| `stream_max_batch_latency_us` | integer (µs) | Max wall-clock span of samples the UDP thread coalesces into one paced `sendmmsg` burst. Bounds send-side latency so low-rate channels update smoothly instead of scrolling in jerks; high-rate channels are unaffected (they hit the fixed 16-packet count cap first). Default `25000` (25 ms); `0` uses the default. Raise it to favour syscall batching, lower it for smoother low-rate updates. |
| `stream_cpu` | integer | CPU index for stream threads; `-1` disables pinning. Default `-1`. |
| `asset_cache_max_bytes` | integer (bytes) | In-memory asset budget. Default 16 GiB; `0` = unlimited. |
| `ddc_cache_max_bytes` | integer (bytes) | DDC intermediate sub-band budget. Default 2 GiB; `0` disables caching. |
| `class_id_oui` | integer, 24-bit | Optional. Setting any `class_id_*` key adds a VITA 49 Class ID to every data/context packet. |
| `class_id_information_code` | integer, 16-bit | Optional. Information class code. Default `0`. |
| `class_id_packet_code` | integer, 16-bit | Optional. Packet class code. Default `0`. |
| `receivers` | array (`1`–`12`) | Required. |

### Instance — receiver

| Parameter | Type / range | Description |
| --- | --- | --- |
| `receiver_id` | integer | Required, unique. |
| `rest_bind_host` | string (IP) | Required. REST bind address. |
| `rest_port` | integer | Required, unique. |
| `udp_output_host` | string (IP) | Required. **Destination** the IQ samples are sent to. Unicast (e.g. `127.0.0.1`, one receiver) or IPv4 multicast (e.g. `239.10.10.10`, any receiver that joins the group). |
| `udp_multicast_interface` | string (IP) | Optional, **multicast only**. Picks which of the machine's network connections the multicast is sent out of, when it has more than one (e.g. Ethernet, Wi-Fi, loopback). Give the IP address of the connection to use. Empty (default) lets the system choose automatically — fine in most cases. Change it only when the automatic choice is wrong: to force local-only testing (`127.0.0.1`) or to send out a specific network when several are connected. Ignored for unicast destinations. |
| `frequency_min_hz` | integer, `0`–`100e9` | Required. Minimum configurable frequency for this receiver (tuner range start). |
| `frequency_max_hz` | integer, > min, ≤ `100e9` | Required. Maximum configurable frequency for this receiver (tuner range stop). |
| `bandwidth_hz` | integer (Hz) | Bandwidth of the receiver. All channels must be inside of this bandwidth. Default `80000000`. |
| `scan_rate_hz_per_s` | number (Hz/s) | Required. Used when the tuner range exceeds the front-end bandwidth. |
| `output_scale` | number, `> 0` | Default channel output multiplier. Default `1.0`. |
| `rf_reference_power_dbm` | number (dBm) | RF power that preserves a source's nominal level. Default `-55.0`. |
| `channels` | array (`1`–`24`) | Required. |

`udp_output_host` sets *where* packets go; `udp_multicast_interface` sets *which of the machine's network connections* they leave from, and only applies when the destination is a multicast group.

```yaml
# Unicast to one receiver — interface is left out (the system routes it).
udp_output_host: "192.168.1.50"

# Multicast to a group, forced out the loopback connection so the
# traffic stays on this machine (typical for local testing).
udp_output_host: "239.10.10.10"
udp_multicast_interface: "127.0.0.1"
```

`frequency_min_hz`/`frequency_max_hz` are the frequency range **this receiver** is configured to cover — pick whatever band it should tune across. The `0`–`100e9` limit on those values is a separate thing: it is the range the **simulator** as a whole can represent (`SIM_MAX_RF_HZ`, reported over REST as `simulator_frequency_max_hz`). Any receiver range must fit inside it, but the limit is not itself a receiver setting.

### Instance — channel

| Parameter | Type / range | Description |
| --- | --- | --- |
| channel `channel_id` | integer, `0`–`N−1` | In order. |
| channel `track_tuner` | bool | Center follows the tuner. Default `false`. |
| channel `center_frequency_hz` | integer (Hz) | Required when not tracking; ignored when tracking. |
| channel `rates` | array (`1`–`128`) | Required. `{bandwidth_hz, sample_rate_hz}` options this channel supports; the first is active at load, and a REST retune selects among them. |
| rate `bandwidth_hz` | integer (Hz) | Non-zero, and ≤ the receiver `bandwidth_hz` (front-end window). |
| rate `sample_rate_hz` | integer, ≥ `bandwidth_hz` | The stream rate paired with this bandwidth. |
| channel `output_scale` | number, `> 0` | Default: receiver `output_scale`. |
| channel `rf_reference_power_dbm` | number (dBm) | Default: receiver value. |
| channel `stream_enabled` | bool | Whether the channel emits its rendered IQ over `udp_output_port`. Default `true`; set `false` to keep the channel defined (e.g. a `track_tuner` reference) without putting its packets on the wire, which saves loopback bandwidth and receiver CPU. |
| channel `stream_id` | integer, 32-bit | Optional VITA 49 Stream ID. When unset, IDs are auto-assigned by counting up (`0`, `1`, …) across all channels in order, skipping explicitly-set values. |
| channel `udp_output_port` | integer | Required, unique across the instance. |

---

## 3. Scenario YAML

### 3.1 Skeleton

```yaml
schema_version: 1
scenario_id: my_scenario
description: optional human-readable text
noise_floor: { enabled: true, power_dbm_per_hz: -160.0, seed: 12345 }
sources: []   # one or more source mappings (see 3.2)
signals: []   # one or more signal mappings (see 3.3)
```

### 3.2 Sources

**IQ file** — complex baseband samples, interleaved `ci16` (little-endian I,Q pairs):

```yaml
- id: fsk_iq
  source_type: iq_file
  file: simulator/assets/fsk_20mhz.c16
  format: ci16
  byte_order: little_endian
  iq_layout: interleaved_iq
  sample_rate_hz: 24576000
  bandwidth_hz: 20000000
  center_frequency_hz: 0
  nominal_level_dbfs: -12.0
```

**Audio file** — a PCM16 WAV (mono, or stereo folded to mono), to be modulated onto a carrier:

```yaml
- id: radio_clip_wav
  source_type: audio_file
  file: simulator/assets/radio_clip.wav
  format: wav
  sample_rate_hz: 48000
  bandwidth_hz: 200000
  center_frequency_hz: 0
  nominal_level_dbfs: -6.0
```

### 3.3 Signals

A signal binds a source to a place on the air:

```yaml
- signal_id: iq_lower
  source_reference: fsk_iq
  modulation: iq
  center_frequency_hz: 9985000000
  bandwidth_hz: 20000000
  power_dbm: -60.0
  start_time_s: 0.0
  repeat_interval_s: 1.0
```

**Modulations**: `iq` (for `iq_file` sources — replayed as-is), and `wbfm`, `am`, `usb`, `lsb`
(for `audio_file` sources — the audio is modulated onto the carrier). WBFM honours
`fm_deviation_hz` (default 75 kHz); AM honours `am_depth` (0.0–1.0, default 0.8).

**Power**: the mixer multiplies the source's samples by
`output_scale · 10^((power_dbm − rf_reference_power_dbm) / 20)` and sums them onto the channel's
mix bus. `rf_reference_power_dbm` (instance config, default −55 dBm) is the RF power at which a
**0 dBFS** source fills the ci16 output exactly; `output_scale` (default 1.0) is a channel digital
gain applied on top.

For **IQ sources the level is set by the file's actual sample amplitude** — the samples are replayed
verbatim, only scaled by that gain. `nominal_level_dbfs` is *declarative metadata and is not
applied*: if you tag a file `nominal_level_dbfs: −10` but it was mastered at full scale, the mixer
still sees full scale. (Audio sources differ: they are RMS-normalised at load, so `power_dbm` means
the same thing regardless of how hot the WAV was mastered.)

> ⚠️ **Overdrive / clipping.** The float mix bus is saturated to ci16 (±32767) once per block. A
> signal whose peak lands above full scale is **hard-clipped**, and clipping a strong carrier —
> or two overlapping ones — sprays **intermodulation products across the whole channel** as a comb
> of evenly spaced ghost carriers on the waterfall. Because the output is quantised to a real ADC's
> word width, this is exactly how a real receiver behaves when you overdrive its front end.
>
> A single IQ signal clips when
>
> ```
> power_dbm  >  rf_reference_power_dbm − source_peak_dBFS − 20·log10(output_scale)
> ```
>
> where `source_peak_dBFS ≤ 0` is the file's true peak. For a **full-scale** IQ file
> (`source_peak_dBFS = 0`) at `output_scale = 1.0`, that reduces to **`power_dbm > rf_reference_power_dbm`**
> — i.e. *any* signal hotter than the reference clips. Signals that overlap in the same channel
> **sum**, so each needs a further ≈ 6 dB of headroom per doubling of overlapping signals.
>
> Note that `snr_db` resolves to an absolute `power_dbm` against the noise floor, so a high SNR over
> a low noise floor can quietly land above the reference. Example from the default scene: a 30 dB-SNR
> POCSAG signal over a −120 dBm/Hz floor in 15 kHz resolves to −48.2 dBm — 6.8 dB above the −55 dBm
> reference — so at `output_scale 1.0` it rails ~68 % of samples.
>
> **Fixes**, in order of preference: lower the **channel `output_scale`** (attenuates signal *and*
> noise together, so SNR is unchanged — just buys headroom); or reduce the signal `power_dbm` /
> `snr_db`; or raise `rf_reference_power_dbm` (affects every signal on the receiver, and de-tunes the
> passthrough memcpy fast path). The simulator prints a per-signal **`warning: signal '…' overdrives
> receiver R channel C …`** at startup whenever this condition is met (see [§10 Validation](#10-validation)),
> with the specific `output_scale` ceiling to use.

### 3.4 Signal bandwidth

`bandwidth_hz` is the signal's **on-air footprint** — the width the mixer uses to decide, per
channel, how the signal interacts with that channel's window. It drives three things:

- **In or out:** whether the signal's band overlaps the channel window at all (no overlap → the
  signal is skipped for that channel).
- **Edge attenuation:** when a signal straddles the window boundary, only the fraction of its band
  inside the window contributes — a signal half in the window comes through at ≈ −3 dB. This is a
  cheap stand-in for a channel filter's roll-off.
- **Nyquist guard:** a band that, shifted to baseband, lies entirely beyond the output Nyquist is
  dropped rather than aliased back in.

It is **bookkeeping, not a filter**: the samples that get mixed are the source's content resampled
to the channel rate; `bandwidth_hz` never reshapes them. Declaring a signal narrower than its
source does not band-limit it — it only mis-scales the overlap math (and trips a load-time
warning).

**Audio sources must state it**, because the modulation — not the source — sets the RF width: one
48 kHz clip is ≈ 200 kHz as WBFM but ≈ 10 kHz as AM (see [`audio_radio_demo.yaml`](../simulator/scenarios/audio_radio_demo.yaml),
where one WAV feeds signals of 200 k / 10 k / 3 k Hz).

**IQ sources may omit it** — the recording already *is* a given width, so an IQ signal that leaves
`bandwidth_hz` out inherits the referenced source's `bandwidth_hz`. Set it explicitly only to
model a narrower footprint than the source (e.g. a narrow signal inside a wideband capture).

---

## 4. The timing model — how inputs repeat

`repeat_interval_s` selects between the two ways a signal appears over time.

### 4.1 Burst — `repeat_interval_s` present

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

```yaml
- signal_id: wbfm_station
  source_reference: radio_clip_wav
  modulation: wbfm
  center_frequency_hz: 10005000000
  bandwidth_hz: 200000
  power_dbm: -65.0
  fm_deviation_hz: 75000
  start_time_s: 0.0
  repeat_interval_s: 30.0
```

To play something essentially once, set a very large interval (e.g. `86400.0` = once a day).

### 4.2 Continuous — `repeat_interval_s` absent

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
- Continuous playback is what you get by leaving `repeat_interval_s` out; there is nothing else to set.
- Works for both IQ and audio sources. IQ recordings wrap as-is (a capture is assumed
  seam-safe). **Audio is seam-conditioned at load** so the modulated waveform is exactly
  periodic — see below.

**Audio seam conditioning.** A modulated clip does not naturally wrap: the WBFM carrier phase is
the running integral of the audio (it ends at an arbitrary angle, not where it started), and the
AM/SSB waveform at the last sample generally differs from the first — a raw wrap would click once
per pass. A continuous audio signal is therefore conditioned before synthesis:

- the last **20 ms** of the clip are crossfaded (equal-power) onto its beginning, shortening the
  playback period by that much — like a DJ loop splice;
- the clip's DC offset is removed so the FM phase completes a whole number of carrier cycles per
  pass (DC is inaudible in audio and only shifts the carrier);
- the SSB quadrature (Hilbert) is computed circularly so it wraps too.

Burst signals are untouched by this — each burst restarts cleanly after its silent gap, so their
pre-renders are bit-identical to a burst-only configuration. Conditioning also means a continuous
audio signal's pre-render is slightly shorter than the clip (by the 20 ms fade) and its one-time
load cost is somewhat higher.

Example — a continuous IQ emitter with no gaps:

```yaml
- signal_id: iq_beacon
  source_reference: fsk_iq
  modulation: iq
  center_frequency_hz: 10005000000
  bandwidth_hz: 20000000
  power_dbm: -60.0
  start_time_s: 0.0
```

Example — an endless FM radio station from an audio clip (the clip repeats seam-conditioned,
with no gap and no click):

```yaml
- signal_id: fm_station
  source_reference: radio_clip_wav
  modulation: wbfm
  center_frequency_hz: 10005000000
  bandwidth_hz: 200000
  power_dbm: -65.0
```

### 4.3 Quick reference

| You want | Set |
| --- | --- |
| Play once per N seconds, silent between | `repeat_interval_s: N` |
| Play continuously with no gap (e.g. an endless radio station) | omit `repeat_interval_s` |

---

## 5. Replay modes (IQ + `iq` modulation only)

By default a signal sits at a fixed frequency. Replay modes make an IQ recording behave like a
live capture that follows the receiver's tuner. Replay-mode signals stream continuously as you
tune across them, so they carry no `repeat_interval_s` (`replay_mode_no_repeat` if one is given).

| `replay_mode` | Behaviour |
| --- | --- |
| `fixed` (default) | The signal sits at `center_frequency_hz`; rendered when its passband overlaps the channel. |
| `range` | The file is played **centered on the tuned frequency** whenever the channel center is inside `frequency_range`; identical output anywhere in the range, silent outside. |
| `shift` | The file content stays at its **absolute** RF position; while tuned inside `frequency_range` the IQ is rotated by `e^{j·2π·(center_frequency_hz − f_tune)·t}`. |

```yaml
- signal_id: replay_range_fm
  source_reference: fsk_iq
  modulation: iq
  replay_mode: range
  frequency_range: { start_hz: 9990000000, stop_hz: 10010000000 }
  bandwidth_hz: 20000000
  power_dbm: -55.0
```

Note `center_frequency_hz` is omitted here — in `range` mode the content follows the tune, so it
is ignored. (`shift` mode still requires it: it is the content's true RF home.)

A narrow channel extracting from a wideband continuous replay (integer rate ratio > 16) is rendered
as a hardware-style DDC — see the "DDC sub-band extraction" section in [`schemas.md`](schemas.md).

---

## 6. Passthrough (dedicated replay channels)

When a channel's only job is to replay one capture verbatim, `passthrough: true` bypasses the
mixer entirely (no float mix bus, no noise floor, no other signals — a `memcpy` at unit gain).
It requires:
- `replay_mode` of `range` or `shift`,
- a continuous signal — i.e. no `repeat_interval_s`,
- a source with `passthrough_variants` — one capture file per channel sample rate.

A channel whose current rate has no matching variant renders **silence**, not a resampled
approximation. See [`schemas.md`](schemas.md) for the full `passthrough_variants` example and the
performance rationale.

---

## 7. Noise floor

Optional; applied to every channel that isn't a passthrough. Prefer the spectral **density**
form so a narrow channel and the wide stream carry the same dBm/Hz:

```yaml
noise_floor: { enabled: true, power_dbm_per_hz: -160.0, seed: 12345 }
```

In-window power is `power_dbm_per_hz + 10·log10(window_bandwidth_hz)`, so a narrow DDC and the
wide stream carry the same density rather than the same total power. `seed` makes the noise
reproducible across instances.

Signals set their power relative to this floor with `snr_db` (dB above the in-band noise) instead
of an absolute `power_dbm`; a signal that sets neither defaults to `+20 dB` SNR and is always
visible without hand-computing dBm. See the signal table above.

---

## 8. Instance YAML

The instance describes the receiver and its output channels.

```yaml
schema_version: 1
instance_id: "receiver_scanner"
scenario_file: "simulator/scenarios/scanner_fsk.yaml"
stream_block_samples: 1024        # CI16 samples per VITA 49.2 packet (1..4096)
stream_max_batch_latency_us: 25000 # cap on a paced UDP send batch's time span (default 25 ms)
receivers:
  - receiver_id: 0
    rest_bind_host: "127.0.0.1"
    rest_port: 8100
    udp_output_host: "127.0.0.1"   # unicast or IPv4 multicast (e.g. 239.10.10.10)
    frequency_min_hz: 9960000000
    frequency_max_hz:  10040000000
    bandwidth_hz: 80000000  # instantaneous window all channels extract from
    scan_rate_hz_per_s: 100000000000
    rf_reference_power_dbm: -55.0
    channels:
      - channel_id: 0
        track_tuner: true            # center follows the tuner (the "wideband" stream)
        rates:                       # first entry is active; REST/B-key select among them
          - { bandwidth_hz: 80000000, sample_rate_hz: 98304000 }
          - { bandwidth_hz: 20000000, sample_rate_hz: 24576000 }
        udp_output_port: 50000
      - channel_id: 1
        center_frequency_hz: 10005000000   # fixed-center channel
        rates:
          - { bandwidth_hz: 20000000, sample_rate_hz: 24576000 }
        udp_output_port: 50001
```

Key points:
- Each **channel lists the `{bandwidth_hz, sample_rate_hz}` options it supports** under `rates`.
  The first entry is active at load; a REST retune (or the receiver's B key) selects another —
  the requested pair must be one of the listed options. Every rate must have `sample_rate_hz ≥
  bandwidth_hz`, and each `bandwidth_hz` must fit the receiver's front-end `bandwidth_hz`.
- `track_tuner: true` makes the channel center follow the tuner (fixed center or scan sweep);
  otherwise give an absolute `center_frequency_hz`.
- A channel whose span leaves the front-end window (tuner center ± the receiver `bandwidth_hz`/2)
  keeps streaming, but empty — like a hardware DDC tuned outside the digitised band.
- `udp_output_port` must be unique across the instance's channels.

See [`schemas.md`](schemas.md) for every receiver/channel field and the DDC cache tuning
knobs (`asset_cache_max_bytes`, `ddc_cache_max_bytes`).

---

## 9. Worked example: `benchmark_load.yaml`

The shipped [`benchmark_load.yaml`](../simulator/tests/benchmarks/benchmark_load.yaml) puts four signals
on the air over a −160 dBm/Hz noise floor, all using the **burst** timing model (each has a
`repeat_interval_s`), from two sources:

| Signal | Source | Modulation | Center | Repeats every |
| --- | --- | --- | --- | --- |
| `iq_lower` | `fsk_iq` (IQ) | `iq` | 9.985 GHz | 1 s |
| `iq_upper` | `fsk_iq` (IQ) | `iq` | 10.025 GHz | 1 s |
| `wbfm_station` | `radio_clip_wav` (audio) | `wbfm` | 10.005 GHz | 30 s |
| `am_station` | `radio_clip_wav` (audio) | `am` | 10.012 GHz | 86400 s (effectively once) |

Note the same IQ source feeds two signals at different frequencies and powers, and the same audio
clip feeds both an FM and an AM station. To turn `iq_lower` into a **continuous** emitter instead
of a 1-second burst — or `wbfm_station` into an endless radio station — delete the signal's
`repeat_interval_s` line.

---

## 10. Validation

Both files are validated at load; a failure prints an error code and the simulator refuses to
start. The full list of codes is in [`schemas.md`](schemas.md). The timing-related ones:

| Error | Cause |
| --- | --- |
| `signal_repeat_too_short` | `repeat_interval_s` is shorter than the source duration. |
| `replay_mode_no_repeat` | A `range`/`shift` replay signal carries a `repeat_interval_s`. |

See [`schemas.md`](schemas.md) for the complete error-code list.

### Load-time warnings (non-fatal)

Beyond the hard errors above, the simulator prints `warning:` lines to stderr at startup for
configurations that load fine but will not behave as intended. These do **not** stop the run.

| Warning | Cause | Fix |
| --- | --- | --- |
| `signal '…' overdrives receiver R channel C … by N dB` | A signal's configured power, after the source's real peak level and the channel's `output_scale`, exceeds the ci16 full scale — the output clips and intermodulates into ghost carriers (see [§3.3 Power](#33-signals)). Reported per signal for the worst-overdriven channel; the check is tune-independent (worst case: the channel centred on the signal), because the tuner is retunable at runtime. | Set the channel `output_scale` to the ceiling printed in the message, lower the signal `power_dbm`/`snr_db`, or raise `rf_reference_power_dbm`. |
| `receiver … channel … rate … does not divide source … rate …` | A channel rate that is not an integer divisor of a looping IQ source falls back to the legacy resampler with reduced alias rejection. | Choose a channel `sample_rate_hz` that divides the source rate. |

The overdrive warning is the load-time counterpart of a subtle failure mode: a strong signal
(or a high `snr_db` over a low noise floor) can silently resolve to a power above the full-scale
reference and rail the ADC, whose hard clipping shows up only as a comb of evenly spaced ghost
carriers on the waterfall — not as an obvious error.
