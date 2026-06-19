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
              +-------------+-------------+
              |                           |
              v                           v
       80-MHz renderer               DDC renderer
              |                           |
              v                           v
       renderer thread(s)            renderer thread(s)
              |                           |
              v                           v
        ringbuffer.c                  ringbuffer.c
              |                           |
              v                           v
          UDP thread                   UDP thread
              |                           |
              v                           v
       raw CI16 UDP packets       raw CI16 UDP packets
```

## Runtime Threads

Each configured receiver starts one REST server and five stream workers:

- one 80-MHz stream worker for the full receiver bandwidth.
- four DDC stream workers for DDC IDs 0 through 3.

Each stream worker owns:

- one renderer thread that renders fixed-size IQ blocks into a ringbuffer.
- one UDP thread that drains complete packets from the ringbuffer and sends them to the configured UDP destination.
- per-stream metrics for packets, bytes, underruns, overruns, dropped samples, and active state.

Receiver configuration updates from REST are protected by `receiver_lock`. Stream workers copy a receiver snapshot while holding that lock, then render/send from the snapshot without keeping the lock held.

## Determinism Model

The deterministic inputs are:

- scenario JSON.
- IQ asset files.
- scenario time.
- receiver frequency/DDC configuration at that scenario time.

Given the same inputs, `renderer_render_80mhz_block` emits the same CI16 samples. Integration tests compare UDP packets against `--render-once-samples` output for the same scenario time.

## Data Format

UDP payloads are raw little-endian complex int16 samples:

```text
I0:int16 Q0:int16 I1:int16 Q1:int16 ...
```

The packet payload sample count is `stream_block_samples`. The default is 1024 samples and the supported range is 1 to 4096 samples.
