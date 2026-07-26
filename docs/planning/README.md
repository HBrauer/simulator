# Planning & Issues

Development plans, implementation plans, TODOs, and open issues for the simulator, sorted by
state. Move a file between folders when its state changes; keep the one-line status header inside
each doc in sync.

- **`open/`** — active or not-yet-implemented work, and open issues.
- **`done/`** — plans whose work has landed. Kept for the rationale/history.
- **`reference/`** — living cross-cutting docs that are not a single plan.

Process docs that are not plans stay in `docs/` (e.g. `release_checklist.md`, `architecture.md`,
`schemas.md`, `rest_api.md`, `vita49_udp.md`, `configuration_manual.md`).

---

## Open

| Doc | What |
| --- | --- |
| [open/pocsag_antialiasing_snr_issue.md](open/pocsag_antialiasing_snr_issue.md) | POCSAG decode failures wrongly blamed on a missing anti-alias filter; real cause was scenario `snr_db`. Fix applied; kept for the DecoderToolkit reconciliation. |
| [open/IMPLEMENTATION_PLAN_GAIN_CONTROL.md](open/IMPLEMENTATION_PLAN_GAIN_CONTROL.md) | Reference-level / gain-control design. Part 1 (VITA-49.2 Reference Level → dBm axis) is built; runtime MGC and AGC are proposed, not scheduled. |
| [open/stage2_two_stage_upsampling_plan.md](open/stage2_two_stage_upsampling_plan.md) | Two-stage upsampling for narrowband signals in wideband channels. CPU-efficiency optimization, not a correctness fix. Not started. |

## Done

| Doc | What |
| --- | --- |
| [done/DEVELOPMENT_PLAN.md](done/DEVELOPMENT_PLAN.md) | Original C simulator skeleton plan (timebase, scenario model, REST, UDP), plus the library-selection research. |
| [done/IMPLEMENTATION_PLAN.md](done/IMPLEMENTATION_PLAN.md) | Simulator improvements (phase/time continuity, determinism, mix bus, resampler, noise). All A–D tasks landed. |
| [done/IMPLEMENTATION_PLAN_AUDIO.md](done/IMPLEMENTATION_PLAN_AUDIO.md) | Audio pre-rendering to IQ + polyphase-resampler generalization. Landed (see `reference/PERF_NOTES.md` for the measured outcome). |
| [done/IMPLEMENTATION_PLAN_CHANNELS.md](done/IMPLEMENTATION_PLAN_CHANNELS.md) | Unified channel model with REST-configurable bandwidth/frequency. Implemented. |
| [done/VITA49_WATERFALL_PLAN.md](done/VITA49_WATERFALL_PLAN.md) | Repo split into simulator + receiver and switch to VITA 49.2 UDP. Implemented. |

## Reference

| Doc | What |
| --- | --- |
| [reference/PERF_NOTES.md](reference/PERF_NOTES.md) | Measured `renderer_benchmark` results and what each rework did / did not achieve. Update on perf-relevant changes. |
| [reference/TODO.md](reference/TODO.md) | Rolling task checklist (currently all items checked). Update per `docs/release_checklist.md`. |
