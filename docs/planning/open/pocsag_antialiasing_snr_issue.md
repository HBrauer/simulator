# Issue: POCSAG decode failures blamed on "missing anti-alias filter"

**Status:** Resolved (scenario SNR fix applied) — kept open as a reference for the
DecoderToolkit maintainer discussion. Delete/close once the external report is reconciled.
**Date:** 2026-07-24
**Area:** narrowband channel output, noise-floor model, scenario SNR resolution
**Trigger:** External report `DecoderToolkit/docs/sender-antialiasing-fix.md` claimed our
IQ sender does naive decimation without an anti-alias filter.

---

## 1. Summary

An external decoder (DecoderToolkit) decoding our channel-2 POCSAG output at 466.075 MHz
reported only ~4 of 12 messages surviving, surrounded by phantom decodes. Its analysis
attributed this to the **sender doing decimation without an anti-alias filter** (flat-to-Nyquist
noise floor, ~9 dB worse peak SNR).

**Verdict: the symptoms are real, the root cause is wrong for this simulator.** There is no
naive-decimation bug. The actual cause was a scenario parameter: the POCSAG signal's `snr_db`
had been lowered from 30 to 20, putting it well below the ~39 dB SNR of the clean reference
recording the decoder validated against. Fix = raise `snr_db`.

---

## 2. Config under test

- Receiver config: `simulator/configs/receiver_default.yaml`, channel 2.
- Channel 2 tuned to 466.075 MHz, 25 kHz bandwidth → selects the rate
  `{ bandwidth_hz: 25000, sample_rate_hz: 32000 }`.
- Scenario: `simulator/scenarios/default.yaml`, signal `sig_pocsag_466075_02`
  (source `pocsag_15k_capture`, 32 ksps, 15 kHz bandwidth, placed at 466.075 MHz).
- The decoder's own capture measured a 24 kHz stream (the `{15000, 24000}` rate); the
  conclusion below holds for both rates.

---

## 3. Why the "missing anti-alias filter" diagnosis does not apply

1. **In the 25 kHz/32000 config there is no resampling at all.** The source is 32 ksps and the
   output is 32000 sps at offset 0, so the renderer takes the verbatim-copy branch
   (`render_direct_baseband`, `renderer.c` — the `source_rate_hz == output_sample_rate_hz &&
   offset_hz == 0` case). No decimation ⇒ decimation aliasing is impossible.

2. **When it does resample (e.g. the 24 kHz rate), a proper polyphase anti-alias filter is
   already applied.** `renderer.c` builds a windowed-sinc polyphase kernel whose cutoff is set to
   exactly the output Nyquist when decimating (`resampler_cutoff`, `build_resampler_table`) and
   grows the kernel radius with the ratio to hold stopband depth (`resampler_radius_for_cutoff`).
   This is the polyphase AA resampler section 5 of the external doc asks for — it exists.

3. **The flat-to-Nyquist noise floor is a modeling choice, not an alias.** `render_noise_floor`
   (`renderer.c`) synthesizes complex **white** Gaussian noise directly at the output sample
   rate across the whole channel. White noise at the output rate is flat to ±Nyquist by
   construction and always will be, independent of any filtering. The decoder's PSD test
   (edge-vs-floor ≈ 0 dB) is measuring this, not decimation aliasing.

---

## 4. Actual root cause

Signal power is derived from `snr_db` as "dB above the in-band noise integrated over the
signal bandwidth" (`scenario.c`, `out->power_dbm = density + 10*log10(bandwidth) + snr_db`).
The scenario had been edited to lower the POCSAG signal from `snr_db: 30` to `snr_db: 20`.

At 20 dB over 15 kHz, with noise filling the full channel, the 2-FSK bit-error rate rises
enough that POCSAG's BCH(31,21) error correction miscorrects codewords into plausible-but-wrong
addresses/messages — exactly the phantom-decode signature. The decoder's reference points were
~39 dB (original recording) and ~30 dB (its "clean resample"); 20 dB is materially worse.

---

## 5. Fix applied

`simulator/scenarios/default.yaml`, `sig_pocsag_466075_02`: `snr_db: 20` → `snr_db: 40`
(≈ the ~39 dB clean-recording reference; dial toward 30 for a noisier-but-decodable signal).

This is a scenario parameter, not a code change. No DSP was modified.

---

## 6. Optional follow-up (NOT done — decide later)

**Band-limit the synthesized noise floor to the channel bandwidth** so its PSD rolls off before
Nyquist, matching a real narrowband receiver's channel filter.

- **What it changes:** `render_noise_floor` currently emits white noise flat to full Nyquist;
  this would low-pass (or generate band-limited) noise so the outer part of the band rolls off.
- **Why it might be wanted:** the decoder's acceptance test 6.1 ("band-edge ≥ 5 dB below
  mid-band") will *always* fail against our stream until the floor rolls off; and it makes the
  spectrum look physically faithful on a waterfall/PSD.
- **Why it is not needed for decoding:** a POCSAG decoder filters to the signal's ~11 kHz band
  before demodulating, so out-of-band noise is removed on its side. Once `snr_db` is adequate,
  in-band SNR (which drives BER) is unchanged whether or not the out-of-band noise rolls off.
- **Cost/risk:** real DSP change to a shared hot path (every channel's noise floor), with its
  own correctness and per-block-cost considerations.

Recommendation: leave as-is unless we specifically want spectral realism or to satisfy the
external 6.1 check. Test 6.1 is built on a wrong premise (flat floor ⇒ missing decimation
filter), which does not hold here.

---

## 7. Message to send back to DecoderToolkit

The sender already implements the polyphase anti-alias resampler the report requests; in the
25 kHz config it does not resample at all. The phantom decodes came from the scenario running
the POCSAG signal at 20 dB SNR instead of ~40 dB, now corrected. The flat-to-Nyquist noise
floor is an intentional noise-model simplification, not an aliasing artifact.
