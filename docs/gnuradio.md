# GNU Radio UDP Compatibility

The simulator sends raw CI16 IQ over UDP by default. Set `framed_udp: true` only when a receiver needs a small simulator header before each payload.

## UDP Source Settings

Use one GNU Radio UDP Source per simulator stream.

Recommended settings:

| Setting | 80-MHz stream | DDC stream |
| --- | --- | --- |
| Address | `0.0.0.0` or the receiving interface | `0.0.0.0` or the receiving interface |
| Port | `udp_80mhz_output_port` | DDC `udp_output_port` |
| Payload size | `stream_block_samples * 4` bytes | `stream_block_samples * 4` bytes |
| Output type | byte stream or short stream | byte stream or short stream |
| Header | none | none |

Each complex sample is four bytes:

```text
little-endian int16 I, little-endian int16 Q
```

For the default `stream_block_samples: 1024`, each UDP datagram carries:

```text
1024 samples * 4 bytes = 4096 bytes
```

## Converting To Complex Float

When using a byte stream, reinterpret the payload as interleaved little-endian signed 16-bit values, split I/Q, then scale to float with:

```text
float = int16 / 32768.0
```

The simulator's DDC streams use the same raw CI16 layout as the 80-MHz stream; only the sample rate differs.

## Sample Rates

| Stream | Sample rate |
| --- | --- |
| 80-MHz receiver output | `98304000` samples/s |
| 20-MHz DDC output | `24576000` samples/s |

Set downstream GNU Radio throttle/sample-rate metadata to the matching value when the UDP Source does not provide timing.

## Multi-Instance Notes

For parallel simulator instances, use unique UDP port ranges per instance. The scenario content can be identical; only instance YAML networking fields need to differ.

## Current Raw Mode Limits

- no sequence number.
- no timestamp.
- no packet framing beyond UDP datagram boundaries.
- packet loss detection must be external, using receiver-side counters or future framed mode.

## Optional Framed Mode

When `framed_udp: true`, each UDP datagram starts with a 24-byte little-endian header:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic bytes `SDR1`. |
| 4 | 2 | Header version, currently `1`. |
| 6 | 2 | Header byte length, currently `24`. |
| 8 | 4 | Raw CI16 payload byte length. |
| 12 | 4 | Stream ID. `0xffffffff` means the 80-MHz stream; DDC streams use IDs `0..3`. |
| 16 | 8 | Scenario time in nanoseconds. |

The raw CI16 payload immediately follows the header and uses the same sample layout as raw mode.
