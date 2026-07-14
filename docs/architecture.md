# Architecture

The simulator is split into deterministic RF rendering, runtime control, and UDP streaming.

```text
YAML config             JSON scenario              IQ assets
    |                       |                         |
    v                       v                         v
config_load_yaml       scenario_load_json       asset_cache_load
    |                       |                         |
    +-----------------------+-------------------------+
                            |
                            v
                    renderer.c / nco.c
                            |
                            v
              renderer_render_channel_block
              (one per channel: 0..N-1)
                            |
                            v
                    renderer thread(s)
                            |
                            v
                      ringbuffer.c
                            |
                            v
                        UDP thread
                            |
                            v
        VITA 49.2 UDP packets (IF data + IF context)
```

## Channel Model

A receiver is a **front end** plus `1..8` **channels**:

- The front end is the tuner (`frequency_start_hz`/`frequency_stop_hz`, fixed or
  scanning) plus an instantaneous analog window `frontend_bandwidth_hz` — the digitised
  band all channels extract from, like an ADC.
- Every output stream is a channel with a `{bandwidth_hz, sample_rate_hz}` **profile**
  (a fixed pair from the receiver's supported set), a UDP port, and either its own
  center frequency or `track_tuner: true` (the center follows the tuner). The former
  80-MHz wideband stream is simply channel 0 with `track_tuner` and the widest profile;
  the former DDCs are fixed-center channels.
- A channel whose span leaves the front-end window keeps streaming, but empty, exactly
  like a hardware DDC tuned outside the digitised band.

Channel center frequency and bandwidth (and therefore sample rate, via the profile) are
runtime-settable through the REST API. A rate change re-anchors the stream's block grid
and is announced in-band with a VITA 49.2 context packet.

## Runtime Threads

Each configured receiver starts one REST server and one stream worker per channel.

Each stream worker owns:

- one renderer thread that renders fixed-size IQ blocks into a ringbuffer.
- one UDP thread that drains complete packets from the ringbuffer, sends them to the configured UDP destination, and interleaves IF-context packets (on start, on configuration change, and ~1 s heartbeat).
- per-stream metrics for packets, bytes, sent samples, average sent sample rate, underruns, overruns, dropped samples, missed samples, and active state.

`samples_sent` counts CI16 samples successfully handed to UDP. `samples_dropped` counts samples discarded before a successful UDP send. `samples_send_dropped` is the subset dropped because the nonblocking UDP socket could not queue the datagram. `udp_send_would_block`, `udp_send_no_buffer`, and `udp_send_other_errors` split those send failures by errno class. `samples_late` counts samples missed because the sender loop was already behind its configured pacing deadline. `samples_missed` includes underruns and late samples. The `/api/v1/metrics` response exposes these counters globally and per channel stream, with each channel's configured `sample_rate_hz` and measured `actual_sample_rate_sps`.

Receiver configuration updates from REST are protected by `receiver_lock`. Stream workers copy a receiver snapshot while holding that lock, then render/send from the snapshot without keeping the lock held; a retune or profile change therefore takes effect on a block boundary within the ring depth (~8 blocks). Every successful REST mutation bumps the receiver's `config_epoch`, which clients can poll cheaply to detect changes.

## Determinism Model

The deterministic inputs are:

- scenario JSON.
- IQ asset files.
- scenario time.
- receiver tuner/channel configuration at that scenario time.

Given the same inputs, `renderer_render_channel_block` emits the same CI16 samples. Integration tests parse the VITA 49.2 wrapper and compare the IQ payload against `--render-once-samples` output (channel selectable with `--render-channel`) for the same scenario time.

The DDC sub-band path (narrow channels extracting from wideband loop replays, `ddc.c`/`ddc_cache.c`) preserves this model with two mechanisms. Per block, the multi-stage decimation cascade is re-initialized and primed with its full filter history from the in-memory loop (the generalization of the resampler's kernel-radius history), so no DSP state survives between blocks and any block index renders identically in isolation. The cached intermediate sub-bands are themselves pure functions of (source samples, applied shift, intermediate rate, front filter plan): the applied shift is snapped to whole cycles per loop so the circular build is exactly periodic, rotation is anchored to an absolute chunk grid so builds are bit-identical for any build-thread count, and the sub-Hz snap remainder is carried by the per-block residual NCO anchored to the absolute sample index.

One documented relaxation: with background builds enabled (`renderer_ddc_background_builds`, the streamer's mode after startup prewarm), a retune to a cold tune area renders through the direct full-rate cascade until the intermediate is ready, so *which* path rendered a given block depends on build timing. The two paths implement the same extraction and agree within the filter design tolerance (sub-dB passband, images below the stopband floor), but not bit-exactly — independently started instances can differ at that level for the duration of one build after a retune. Blocking mode (the default, used by `--render-once-samples` and the tests) keeps strict bit-identity: a missing intermediate is built synchronously and only the delivery timing of that channel is affected.

## Data Format

UDP datagrams contain VITA 49.2 IF-data packets. The simulator emits:

```text
32-bit VRT/VITA header, big-endian
32-bit stream ID, big-endian
32-bit integer timestamp seconds, big-endian
64-bit fractional timestamp picoseconds, big-endian
CI16 payload: I0:int16 Q0:int16 I1:int16 Q1:int16 ...
```

The CI16 payload remains little-endian interleaved IQ. The packet payload sample count is `stream_block_samples` (default 1024, supported range 1 to 4096), except on low-rate channels, where it shrinks to the largest power of two keeping at least ~4 blocks per second (floor 64) — a 2 kS/s DDC channel emits 256-sample / 128 ms packets so pacing and the context heartbeat stay bounded. Interleaved IF-context packets (packet type 4) carry the channel's RF reference frequency, bandwidth, and sample rate; see [vita49_udp.md](vita49_udp.md).
