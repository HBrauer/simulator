# SDR Waterfall Receiver

C/SDL2 receiver for simulator VITA 49.2 UDP streams. It is built as:

```sh
build/sdr-waterfall-receiver
```

## Run (REST-controlled)

Point the receiver at a simulator receiver's REST API and pick a channel. It discovers
the channel's UDP port, sample rate, and bandwidth from `GET /api/v1/capabilities` and
`GET /api/v1/channels`, so no `--port`/`--sample-rate-hz` is needed:

```sh
build/sdr-waterfall-receiver \
  --control-url http://127.0.0.1:8100 \
  --channel 1 \
  --fft-size 1024
```

Use that with `simulator/scenarios/gnuradio_demo.yaml`; channel `1` (port `50001`) is centered on the demo signal and should show colored waterfall lines immediately. The full 80-MHz scanner stream is channel `0` on port `50000`.

With `--control-url` the toolbar gains a control group on the left:

- **Channel combo box** (`CH1 v`) — lists all channels with their center frequency and bandwidth; selecting one reopens the UDP socket on that channel's port and adopts its sample rate. Keyboard: `PgUp`/`PgDn`, or `0`-`7` for direct selection.
- **Center frequency field** — shows the channel's current center in MHz. Click it, type a new value, and press Enter to retune (Esc cancels). Greyed out for a tuner-tracking channel (channel 0 in the default config), which is retuned via the receiver's `frequency-range` API instead. Keyboard: `Left`/`Right` steps by bandwidth/10 (Shift for a 10x step).
- **Bandwidth combo box** (`20M v`) — lists the receiver's supported bandwidth/sample-rate profiles; selecting one applies the paired sample rate automatically and the waterfall re-derives its frame stride. Keyboard: `B` cycles through the profiles.

Retunes and bandwidth changes made by *other* clients are picked up automatically from the in-band VITA 49.2 context packets each stream carries — the receiver re-times its waterfall when the sample rate changes and shows the current center frequency and bandwidth in the window title.

If the simulator publishes to a multicast group, the receiver reads the group address from the API and joins it; pass `--interface` to select the receive interface:

```sh
build/sdr-waterfall-receiver \
  --control-url http://127.0.0.1:8100 \
  --channel 1 \
  --interface 127.0.0.1 \
  --fft-size 1024
```

## Run (manual)

Without `--control-url` the stream parameters are given explicitly:

```sh
build/sdr-waterfall-receiver \
  --host 127.0.0.1 \
  --port 50001 \
  --fft-size 1024 \
  --sample-rate-hz 24576000
```

For multicast, pass the multicast group as `--host`; the receiver joins the group and binds the configured port:

```sh
build/sdr-waterfall-receiver \
  --host 239.10.10.10 \
  --interface 127.0.0.1 \
  --port 50001 \
  --fft-size 1024 \
  --sample-rate-hz 24576000
```

## Display

The visible history defaults to 30 seconds and is changed only in the UI. Use the `-` and `+` buttons on the right of the toolbar (or `[`/`]` keys) to select 1 to 60 seconds while the receiver is running. Rows are computed automatically from the current window height, using as many waterfall rows as fit below the toolbar; `--rows` is available only as a manual override.

For the full 98.304 MS/s stream, the receiver needs a large kernel UDP receive buffer. At startup it prints the actual `rcvbuf` value. If it warns that the buffer is small, raise the Linux socket receive limits before starting the receiver:

```sh
sudo sysctl -w net.core.rmem_max=134217728 net.core.rmem_default=134217728
```

For a two-machine setup, configure the link for MTU 9000 and use `stream_block_samples: 1536` on the simulator. That keeps each VITA/UDP datagram below jumbo MTU and avoids IP fragmentation.

Useful options:

```text
--control-url URL           Simulator REST API, e.g. http://127.0.0.1:8100
--channel N                 Initial channel with --control-url, default 0
--fft-size N                Power-of-two FFT size, default 1024
--interface HOST            Multicast receive interface, default 0.0.0.0
--rows N                    Override automatic one-row-per-screen-pixel layout
--sample-rate-hz N          Stream sample rate (manual mode), default 98304000
--max-packets N             Exit after N UDP packets
--max-frames N              Exit after N waterfall frames
--width N                   Window width, default 1200
--height N                  Window height, default 700
--min-db DB                 Manual waterfall floor, default -100
--max-db DB                 Manual waterfall ceiling, default 0
--no-auto-level             Disable dynamic waterfall levels
--log-iq-stats              Log periodic received CI16 payload min/max/nonzero counts
--headless                  Process packets without opening an SDL window
```

## Packet Support

The parser supports the simulator's VITA 49.2 subset:

- IF-data packet (type 1) with stream ID.
- VITA 49.2 indicator bit set.
- UTC integer seconds plus fractional picoseconds timestamp.
- little-endian CI16 IQ payload.
- IF-context packet (type 4) carrying RF reference frequency, bandwidth, and sample rate.

The app validates packet size, timestamp mode, payload alignment, and tracks sequence gaps on the data stream. FFTs are computed with FFTW3f and displayed in an SDL2 streaming texture.
