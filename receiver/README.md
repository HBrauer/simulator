# SDR Waterfall Receiver

C/SDL2 receiver for simulator VITA 49.2 UDP streams. It is built as:

```sh
build/sdr-waterfall-receiver
```

## Run

```sh
build/sdr-waterfall-receiver \
  --host 127.0.0.1 \
  --port 50001 \
  --fft-size 1024 \
  --sample-rate-hz 24576000
```

Use that with `simulator/scenarios/gnuradio_demo.json`; DDC port `50001` is centered on the demo signal and should show colored waterfall lines immediately. The full 80-MHz scanner stream remains on port `50000`.

The visible history defaults to 30 seconds and is changed only in the UI. Use the `-` and `+` buttons in the top toolbar to select 1 to 60 seconds while the receiver is running. Rows are computed automatically from the current window height, using as many waterfall rows as fit below the toolbar; `--rows` is available only as a manual override.

For multicast, pass the multicast group as `--host`; the receiver joins the group and binds the configured port:

```sh
build/sdr-waterfall-receiver \
  --host 239.10.10.10 \
  --port 50001 \
  --fft-size 1024 \
  --sample-rate-hz 24576000
```

Useful options:

```text
--fft-size N                Power-of-two FFT size, default 1024
--rows N                    Override automatic one-row-per-screen-pixel layout
--sample-rate-hz N          Stream sample rate, default 98304000
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

The parser supports the simulator's VITA 49.2 IF-data subset:

- IF-data packet with stream ID.
- VITA 49.2 indicator bit set.
- UTC integer seconds plus fractional picoseconds timestamp.
- little-endian CI16 IQ payload.

The app validates packet size, timestamp mode, payload alignment, and tracks sequence gaps. FFTs are computed with FFTW3f and displayed in an SDL2 streaming texture.
