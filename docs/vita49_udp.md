# VITA 49.2 UDP Output

The simulator sends one VITA 49.2 IF-data packet per UDP datagram for each enabled
channel, plus periodic IF-context packets on the same port that announce the channel
configuration in-band.

Every packet is big-endian (VITA 49.2 rule 5.1-1), including the sample payload.

## IF Data Packet Layout

The simulator's current packet subset is (offsets shown without a Class ID; an optional
Class ID inserts 8 bytes between the Stream ID and the integer timestamp, shifting every
later field by 8):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | VRT/VITA IF-data header, big-endian. |
| 4 | 4 | Stream ID, big-endian. |
| 8 | 8 | Optional Class ID (present only when `class_id_*` is configured). |
| 8 (16) | 4 | Integer timestamp seconds, big-endian. |
| 12 (20) | 8 | Fractional timestamp in picoseconds, big-endian. |
| 20 (28) | variable | IQ payload, big-endian, in the channel's `output_format` (see below). |

Header fields:

| Field | Value |
| --- | --- |
| Packet type | `1`, IF data (with Stream ID). |
| Class ID present | Header bit 27, set only when a Class ID is configured. |
| VITA 49.2 indicator | Packet-specific indicator bit 25 set. |
| TSI | `1`, UTC. |
| TSF | `2`, real-time fractional timestamp. |
| Sequence | 4-bit per-stream packet counter. |
| Packet size | Total packet size in 32-bit words. |

### Stream ID

Each channel carries a 32-bit Stream ID. Set it per channel with the `stream_id` config
key; any channel left unset is auto-assigned by counting up (`0`, `1`, `2`, …) across all
receivers and channels in configuration order, skipping any value claimed explicitly.

### Class ID

Setting any of `class_id_oui`, `class_id_information_code`, or `class_id_packet_code` at
the instance top level adds an optional two-word VITA 49 Class ID to every data and context
packet. Word 1 carries the 24-bit OUI in bits 23–0 (bits 31–24 reserved, per VITA 49.2 /
DIFI); word 2 packs the 16-bit information class code (high half) and packet class code
(low half).

## IF Context Packets

Each channel stream also carries VITA 49.2 IF-context packets (packet type `4`, 60
bytes, or 68 with a Class ID) so UDP consumers can follow retunes and sample-rate changes
and self-describe the payload without polling the REST API. One is sent when a stream
(re)starts, immediately after every configuration change (with the CIF0 change indicator
set), and once a second as a heartbeat otherwise. It shares the channel's Stream ID and,
when configured, the same optional Class ID as the data packets (inserted at offset 8,
shifting later fields by 8).

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | VRT/VITA context header (type `4`, Class-ID-present bit 27, TSI `1`, TSF `2`, own 4-bit sequence). |
| 4 | 4 | Stream ID (same as the channel's data stream). |
| 8 | 8 | Optional Class ID (present only when configured). |
| 8 (16) | 4 | Integer timestamp seconds. |
| 12 (20) | 8 | Fractional timestamp in picoseconds. |
| 20 (28) | 4 | CIF0 indicator: change (bit 31), bandwidth (29), RF reference frequency (27), reference level (24), sample rate (21), data packet payload format (15). |
| 24 (32) | 8 | Bandwidth in Hz, 64-bit fixed point, radix point after bit 20 (`value_hz << 20`). |
| 32 (40) | 8 | RF reference frequency in Hz, same format. For a tuner-tracking channel this is the instantaneous tuner center. |
| 40 (48) | 4 | Reference Level: RF power (dBm) at digital full scale (0 dBFS). Low 16 bits are a two's-complement `dBm << 7` value; high 16 bits reserved. |
| 44 (52) | 8 | Sample rate in Hz, same fixed-point format as bandwidth. |
| 52 (60) | 8 | Data Packet Payload Format field (VITA 49.2 §9.13.3); describes the paired IF-data payload's sample layout (see below). |

The waterfall receiver parses these to retarget its FFT stride when the sample rate
changes and to display the current center frequency, bandwidth, and absolute-power
reference. Fields appear in descending CIF0-bit order, so the payload format field (bit 15)
follows the sample rate.

## IQ Payload

Each channel's payload format is selected with the per-channel `output_format` key
(default `ci16`). All formats are big-endian, processing-efficient, Complex Cartesian
(interleaved I then Q). The paired context packet's Data Packet Payload Format field
advertises the layout so a consumer can decode without out-of-band configuration.

| `output_format` | Sample layout | Bytes/sample | Payload Format word 1 |
| --- | --- | --- | --- |
| `ci16` | 16-bit signed I, 16-bit signed Q; two items in one 32-bit word (I high, Q low). Full scale ±32767. | 4 | `0x200003CF` |
| `ci24` | 24-bit signed I then Q, each **left-justified** in its own 32-bit item packing field (value in bits 31..8, low 8 bits zero, per rule 6.1.1.1-2). Full scale ±(2²³−1). | 8 | `0x200007D7` |
| `cf32` | IEEE-754 single-precision float I then Q, each a 32-bit word. Full scale ±1.0. | 8 | `0x2E0007DF` |

The Payload Format field's second word is always `0` (Repeat Count / Vector Size = 1).

Each format is carried in its native type end to end (the render mix bus and the per-channel
ring buffer), so `ci24` and `cf32` keep the renderer's sub-16-bit precision rather than being
quantised to `int16` first: a synthesized (mixed/resampled) channel yields genuine >16-bit
detail, and a passthrough channel replays a native `ci24`/`cf32` capture file verbatim (a
`memcpy` at unit gain). `cf32` is not clamped — IEEE-754 samples carry overrange above the
±1.0 full-scale point, per VITA 49.2 §6.1.1.4.

`stream_block_samples` controls the payload size in complex samples. Total UDP datagram
size is:

```text
20 + stream_block_samples * bytes_per_sample bytes          (no Class ID)
28 + stream_block_samples * bytes_per_sample bytes          (with Class ID)
```

Because `ci24` and `cf32` are 8 bytes per sample, they fit half as many samples per MTU as
`ci16`; size `stream_block_samples` accordingly for a given MTU (e.g. the MTU 9000 high-rate
profile uses `1536` for `ci16` but should be halved for the 8-byte formats).

> Note on `ci24`: the simulator follows the VITA 49.2 rule literally (data item
> left-justified in the item packing field). All three formats decode directly with the
> `vita49io` reference library at **v0.1.8 or newer** (which fixed sub-word fixed-point to
> left-justified per §6.1.1.1); older `vita49io` reads `ci24` from the low bits and misreads
> it.

## Receiver Note

The in-repo waterfall receiver auto-detects the payload format from the context packet's Data
Packet Payload Format field and decodes `ci16`, `ci24`, and `cf32` accordingly. Because it
learns the format from the ~1 Hz context heartbeat, a receiver that starts after a stream is
already running decodes the first sub-second of an 8-byte-format stream as `ci16` until the
next context packet arrives, then self-corrects.

## GNU Radio Note

A plain GNU Radio UDP Source can receive the datagrams as bytes, but downstream flowgraphs
must parse and remove the VITA 49.2 headers before reinterpreting the big-endian payload in
the advertised format, and should discard (or use) the interleaved type-4 context packets.
