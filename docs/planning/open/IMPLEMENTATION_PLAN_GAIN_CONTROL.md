# Gain Control & Reference Level — Design Note

**Status:** Design only. Not scheduled. Written 2026-07-21.
**Purpose:** Capture the full context of the reference-level / gain-control discussion so this
work can be picked up later without re-deriving it. Part 1 documents what is **already built**
(the VITA-49.2 Reference Level feature). Parts 2–4 are the **proposed, not-yet-built** work
(runtime MGC, then AGC) and the reasoning behind the recommended order.

---

## 0. TL;DR / the decision

- The receiver already reports an absolute **dBm** spectrum axis, calibrated from a VITA-49.2
  **Reference Level** carried in the IF-context packet. **This part is done and verified.**
- `rf_reference_power_dbm` is the receiver's full-scale calibration anchor (the RF power that maps
  to 0 dBFS). It is a **config input**, not computed. It is effectively **manual gain control
  (MGC)** — but currently **load-time only** (no runtime setter).
- **Recommended next step (if/when we do this):** make the reference level **runtime-settable via
  REST**, modelled as a **front-end (per-receiver) gain** that shifts all channels together, plus
  an optional **per-channel digital trim**. This exercises the dynamic VITA Reference Level path
  deterministically.
- **AGC: deferred.** Add only when a concrete consumer needs to be tested against an
  auto-adapting receiver. Prefer implementing AGC-like behaviour as an **external script driving
  the MGC REST setter** rather than a built-in feedback loop. If built in, make it **opt-in
  per-channel**, static reference as the default.

---

## 1. What is already built (VITA-49.2 Reference Level → dBm axis)

### 1.1 Concept

`rf_reference_power_dbm` = **the RF power (dBm) that maps to digital full scale (0 dBFS)**. It is a
calibration *anchor*, not a clamp:
- A signal rendered at `power_dbm == rf_reference_power_dbm` reaches full scale.
- Below it → headroom; above it → the sample **clips** at the ci16 rails.
- The renderer accumulates all signals + noise into a wide **float** mix bus and saturates to
  ci16 only at the very end, so exceeding the reference is an expressible (mis)configuration that
  clips exactly like real hardware. There is **no validation** against the reference.

VITA-49.2 defines a **Reference Level** field precisely to carry this anchor in-band, so a
consumer can convert dBFS → absolute dBm (and follow it if it changes, e.g. under AGC).

### 1.2 Simulator side (emit)

- Struct field `reference_level_dbm` added to `vita49_context_packet_t`
  ([simulator/src/vita49_packet.h](simulator/src/vita49_packet.h)).
- Encoded in `vita49_write_context_packet` ([simulator/src/vita49_packet.c](simulator/src/vita49_packet.c)):
  - **CIF0 bit 24** (`VITA49_CIF0_REFERENCE_LEVEL`).
  - Format per VITA-49.2 §9.5.9: a 32-bit word; **low 16 bits** are a two's-complement value in
    dBm with the radix point after bit 7 (i.e. `dBm × 2^7`); high 16 bits reserved.
  - Field order in the packet (descending CIF0 bit order): bandwidth (29), RF reference frequency
    (27), **reference level (24)**, sample rate (21). Context packet grew from 12 → 13 words.
  - Helper: `vita49_reference_level(double dbm)`.
- Populated at the emit site from the channel's configured reference in
  `maybe_send_context_packet` ([simulator/src/streamer.c](simulator/src/streamer.c)):
  `.reference_level_dbm = channel->rf_reference_power_dbm`.

### 1.3 Receiver side (parse)

- `vita49_rx_context_t` gained `has_reference_level` + `reference_level_dbm`
  ([receiver/src/vita49_rx.h](receiver/src/vita49_rx.h)).
- `vita49_rx_parse_context` ([receiver/src/vita49_rx.c](receiver/src/vita49_rx.c)) adds bit 24 to
  the accepted CIF0 mask (**required** — the parser rejects any packet with unknown CIF0 bits, so
  the bit must be whitelisted) and decodes `int16(low16) / 128.0` → dBm. Packets **without** the
  field still parse (`has_reference_level = false`) → backward compatible.

### 1.4 Receiver side (calibration + axis)

- **FFT full-scale normalization** ([receiver/src/waterfall.c](receiver/src/waterfall.c),
  [receiver/src/waterfall.h](receiver/src/waterfall.h)): `wf->full_scale_db = 20·log10(Σwindow)`
  is subtracted from every bin so a full-scale on-bin complex tone reads **0 dBFS**. (Previously
  the spectrum floated ~+54 dB above 0 because the FFT was unnormalized.)
- **Axis** ([receiver/src/main.c](receiver/src/main.c)):
  - `rx_stats_t` gained `have_reference_level` + `reference_level_dbm`; set from the parsed
    context in the context-packet handler.
  - `ui_draw_spectrum` takes a `label_offset_db` and a `unit_label`. When a reference level is
    known: `label_offset_db = reference_level_dbm`, unit `"DBM"`. Otherwise offset 0, unit
    `"DBFS"` (0 = full scale). Gridline tick values are chosen "nice" in the *displayed* domain
    and mapped back to dBFS for placement.

### 1.5 Calibration chain (why it reads true dBm)

Simulator render gain (see `renderer_render_window_block` / passthrough in
[simulator/src/renderer.c](simulator/src/renderer.c)):
```
digital_gain = passband_gain · output_scale · 10^((signal.power_dbm − rf_reference_power_dbm)/20)
```
So a full-scale-normalized source at `power_dbm == reference` renders at 0 dBFS. The receiver
computes `dBFS = 20·log10(|FFT|/Σwindow)`, then `dBm = dBFS + reference_level`. End to end, a tone
configured at `P` dBm reads `P` dBm at its peak.

### 1.6 Verification already done

- Unit tests (all pass; only pre-existing `test_config.c` string-assert failures remain):
  - Wire round-trip both directions (write bit 24 + Q-format bytes; parse them; plus a
    backward-compat no-reference-level packet). See
    [simulator/tests/unit/test_vita49_packet.c](simulator/tests/unit/test_vita49_packet.c) and
    [receiver/tests/test_receiver_c.c](receiver/tests/test_receiver_c.c).
  - Calibration: full-scale on-bin tone → 0.0 dBFS (±0.2); half-scale → −6.0 dBFS (±0.2) →
    1:1 scaling. (`full_scale_tone_reads_zero_dbfs` in the receiver test.)
- Live end-to-end: a tone configured at −65 dBm measured back from the raw UDP stream (using the
  receiver's exact calibration) at **−65.00 dBm**. A captured context packet decoded to
  `reference_level = −55.0 dBm`.

---

## 2. The gain architecture (mental model for the future work)

The simulated receiver has **one wideband ADC** behind an 80 MHz front end; each channel is a
**DDC (digital downconverter)** that extracts a sub-band from that single digitized stream.

- **Analog gain (the real MGC)** — LNA / attenuators / IF amp — is applied **once, before the
  ADC**. It sets the ADC full-scale reference in absolute dBm and is **shared by all channels**.
  Turn front-end gain up 10 dB → every channel's reference drops 10 dB together (one ADC).
- **DDCs are purely digital.** A per-channel *analog* gain would be fiction. But a DDC can apply a
  **digital gain** after decimation, which shifts *that channel's* effective full-scale reference.
- So a **per-channel reference is legitimate** as "front-end gain + this channel's digital trim,"
  but the physically primary knob is **per-receiver**.

How this maps to the current config ([simulator/src/config.c](simulator/src/config.c),
[simulator/src/sim_types.h](simulator/src/sim_types.h)):

| Layer | Field | Models | Notes |
|---|---|---|---|
| Receiver | `rf_reference_power_dbm` | Front-end / ADC reference (real MGC) | Default **−55.0** if unset (0.0 sentinel = "unset"). |
| Channel | `rf_reference_power_dbm` | Per-DDC digital trim (effective per-channel ref) | Inherits the receiver value if unset. |
| Channel/Receiver | `output_scale` | Extra digital scaling | Already runtime-settable: `POST /api/v1/output-scale`. |

**Design tension to preserve:** if each channel holds a fully independent reference with no shared
front-end value, it still works but drops the "one ADC ties them together" physics. Prefer keeping
a receiver-level front-end reference that all channels are relative to.

### Config vs scenario separation (do not blur this)

| File | Fields | Meaning |
|---|---|---|
| scenario (`simulator/scenarios/*.yaml`) | `signal.power_dbm`, `signal.snr_db`, `noise_floor.power_dbm_per_hz` | What is **on the air** (ground truth, absolute). |
| receiver config (`simulator/configs/receiver_*.yaml`) | `rf_reference_power_dbm`, `output_scale` | How the **receiver** is set up (full-scale calibration). |

This separation is why the noise floor can exceed the reference (overdriven front end): the
scenario author sets on-air power, the config author sets full-scale, nothing couples them.

---

## 3. Proposed work — Runtime MGC (recommended first, if we do anything)

**Goal:** make the reference level adjustable at runtime, modelled as front-end gain with an
optional per-channel trim. High value because it exercises the *dynamic* VITA Reference Level path
(the whole reason the field is dynamic) **deterministically**, with no feedback loop.

### 3.1 REST surface (mirror `output-scale`)

Template to copy: the `POST /api/v1/output-scale` handler in
[simulator/src/rest_server.c](simulator/src/rest_server.c) (around the `output-scale` route) and
its validation/apply pattern.

Proposed endpoints (final shape TBD — see open questions):
- **Receiver-level (primary):** set the front-end reference; **shifts all channels together**.
  e.g. `POST /api/v1/reference-level` `{ "rf_reference_power_dbm": <dBm> }`.
- **Per-channel (optional trim):** override one channel. e.g. a field on the existing channel
  `PUT /api/v1/channels/{id}`, or `POST /api/v1/channels/{id}/reference-level`.

Semantics: the receiver-level value is the front-end reference; a channel's effective reference =
its own override if set, else the receiver value. (Decide whether "front-end shift" should also
move channels that have explicit overrides — physically yes, since they share the ADC; a clean
model is `effective = front_end_ref + channel_digital_trim_db`, storing the trim rather than an
absolute per-channel reference. **This is a modelling decision to settle before coding.**)

### 3.2 VITA `changed` bit path (important)

When the reference changes, the next context packet for each affected channel must set the CIF0
**change indicator** (bit 31) so consumers re-read it promptly. See `context_config_changed` /
`maybe_send_context_packet` in [simulator/src/streamer.c](simulator/src/streamer.c) — currently it
tracks bandwidth/sample-rate/track_tuner changes; extend the change detection to include the
reference level. Also ensure a change triggers a context packet **immediately** rather than only
on the heartbeat interval.

### 3.3 GET reflection

`rf_reference_power_dbm` is already in the GET responses (receiver + channel + capabilities) in
[simulator/src/rest_server.c](simulator/src/rest_server.c). Ensure any new
front-end/trim split is reflected consistently.

### 3.4 Tests

- REST: set receiver-level reference → all channels' reported reference shifts; set per-channel →
  only that channel. (Model on existing REST tests.)
- Streamer: a reference change sets the context `changed` bit and emits promptly.
- End-to-end sanity (optional, manual): retune reference via REST while the receiver GUI is up →
  the dBm axis re-labels live.

### 3.5 Receiver GUI note

The receiver already re-reads the reference from every context packet and re-labels the axis, so
a mid-stream change is picked up automatically. No receiver change needed for MGC beyond what
exists — verify the `changed`-bit prompt actually shortens the latency.

---

## 4. Proposed work — AGC (deferred; design sketch only)

**Recommendation: do NOT build this until there is a concrete need** (e.g. testing that a
downstream consumer correctly follows a Reference Level that moves on its own). Reasons:
- Adds a **feedback loop** that fights the simulator's core value: deterministic, author-controlled
  output.
- **Hides misconfigurations** — e.g. the clipping-noise-floor case would be silently "corrected."
- Real AGC is a rabbit hole (attack/decay time constants, hysteresis, headroom target, per-freq
  behaviour).

**Preferred approach:** implement AGC-like dynamics **externally** as a script that polls the
stream / measured level and drives the **MGC REST setter** from Part 3. You get AGC behaviour with
zero controller baked into the sim, and full control over the policy.

**If built in anyway**, constraints:
- **Opt-in per channel** (or per receiver), with static reference as the default — never on by
  default.
- Drive off **measured aggregate power at the ADC** (total in-window power), **not** knowledge of
  individual `signal.power_dbm` (that would be circular / physically dishonest — the receiver does
  not know what's on the air).
- Parameters: target headroom (dB below full scale), attack time, decay time, min/max gain
  (reference) bounds, optional hysteresis.
- Whenever AGC moves the reference, it must **update the VITA Reference Level and set the change
  bit** (reuse the Part 3 machinery) so consumers stay calibrated. This is the single most
  important interop behaviour — the reason the field is dynamic in the first place.
- Consider real-hardware caveats it will still gloss over: frequency-dependent gain (a table, not
  one number), temperature drift, non-linear compression (1 dB compression point sits a few dB
  below hard clip). Document what is and isn't modelled.

**Why reference level must stay receiver-side (not signal-derived):** it is the *yardstick* for
measuring signals. Deriving it from the signal is circular (you couldn't call a signal weak or
strong), impossible with multiple signals per channel, and breaks the scenario/receiver
separation. AGC is the only legitimate "signal-responsive" mechanism, and even it responds to
*measured aggregate level*, stays receiver-side, and reports the result out.

---

## 5. Related opportunistic item (came up in discussion)

**Load-time sanity warning for an overdriven front end.** Because nothing validates the noise
floor against the reference, a too-high `noise_floor.power_dbm_per_hz` for the channel bandwidth
clips the ADC before any signal arrives. Example that bit us:
```
noise density = −120 dBm/Hz,  bw = 20 MHz  → integrated = −120 + 10·log10(20e6) = −47 dBm
reference     = −55 dBm (= 0 dBFS)          → noise is +8 dB over full scale → clips
```
Proposed: at load, warn when the integrated in-window noise floor
(`power_dbm_per_hz + 10·log10(bandwidth)`) comes within N dB of (or exceeds) the channel's
reference level. Small, catches a real authoring-mistake class, and the new dBm axis makes the
symptom visible (noise crowding the top of scale). Independent of MGC/AGC; could be done anytime.

---

## 6. Open questions to settle before implementing Part 3

1. **Storage model:** per-channel **absolute reference** (as today) vs a receiver **front-end
   reference + per-channel digital trim (dB)**. The trim model is more physically faithful
   (front-end shift moves everyone together) — recommended, but it changes the config schema and
   the GET/PUT surface.
2. **REST shape:** dedicated `reference-level` endpoints vs fields on existing channel/receiver
   PUT. Match whatever convention the rest of the API leans toward.
3. **Units/naming:** expose as `rf_reference_power_dbm` (calibration language) or as a `gain_db`
   knob (operator language)? Gain is what a real operator turns; reference is what the ADC sees.
   Possibly both: gain in, reference derived and reported.
4. **Should a front-end change move channels that have explicit overrides?** Physically yes (shared
   ADC). Decide and document.

---

## 7. Quick file map

| Area | File |
|---|---|
| Context packet struct/encode | [simulator/src/vita49_packet.h](simulator/src/vita49_packet.h), [simulator/src/vita49_packet.c](simulator/src/vita49_packet.c) |
| Emit site + change detection | [simulator/src/streamer.c](simulator/src/streamer.c) |
| Config parse/defaults | [simulator/src/config.c](simulator/src/config.c), [simulator/src/sim_types.h](simulator/src/sim_types.h) |
| Render gain formula | [simulator/src/renderer.c](simulator/src/renderer.c) |
| REST (output-scale is the template) | [simulator/src/rest_server.c](simulator/src/rest_server.c) |
| Receiver parse | [receiver/src/vita49_rx.h](receiver/src/vita49_rx.h), [receiver/src/vita49_rx.c](receiver/src/vita49_rx.c) |
| Receiver FFT calibration | [receiver/src/waterfall.c](receiver/src/waterfall.c), [receiver/src/waterfall.h](receiver/src/waterfall.h) |
| Receiver dBm axis | [receiver/src/main.c](receiver/src/main.c) |
| Wire/format docs | [docs/schemas.md](docs/schemas.md), [docs/configuration_manual.md](docs/configuration_manual.md), [docs/rest_api.md](docs/rest_api.md) |

*(Line numbers deliberately omitted — reference by symbol/function name; they drift.)*
