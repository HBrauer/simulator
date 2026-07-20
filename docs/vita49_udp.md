# VITA 49.2 UDP Output

The simulator sends one VITA 49.2 IF-data packet per UDP datagram for each enabled
channel, plus periodic IF-context packets on the same port that announce the channel
configuration in-band.

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
| 20 (28) | variable | CI16 IQ payload. |

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

Each channel stream also carries VITA 49.2 IF-context packets (packet type `4`, 48
bytes, or 56 with a Class ID) so UDP consumers can follow retunes and sample-rate changes
without polling the REST API. One is sent when a stream (re)starts, immediately after every
configuration change (with the CIF0 change indicator set), and once a second as a heartbeat
otherwise. It shares the channel's Stream ID and, when configured, the same optional Class
ID as the data packets (inserted at offset 8, shifting later fields by 8).

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | VRT/VITA context header (type `4`, Class-ID-present bit 27, TSI `1`, TSF `2`, own 4-bit sequence). |
| 4 | 4 | Stream ID (same as the channel's data stream). |
| 8 | 8 | Optional Class ID (present only when configured). |
| 8 (16) | 4 | Integer timestamp seconds. |
| 12 (20) | 8 | Fractional timestamp in picoseconds. |
| 20 (28) | 4 | CIF0 indicator: change (bit 31), bandwidth (bit 29), RF reference frequency (bit 27), sample rate (bit 21). |
| 24 (32) | 8 | Bandwidth in Hz, 64-bit fixed point, radix point after bit 20 (`value_hz << 20`). |
| 32 (40) | 8 | RF reference frequency in Hz, same format. For a tuner-tracking channel this is the instantaneous tuner center. |
| 40 (48) | 8 | Sample rate in Hz, same format. |

The waterfall receiver parses these to retarget its FFT stride when the sample rate
changes and to display the current center frequency and bandwidth.

## IQ Payload

The packet payload is unchanged CI16 IQ:

```text
little-endian int16 I, little-endian int16 Q
```

`stream_block_samples` controls the CI16 payload size in complex samples. Total UDP datagram size is:

```text
20 + stream_block_samples * 4 bytes          (no Class ID)
28 + stream_block_samples * 4 bytes          (with Class ID)
```

## GNU Radio Note

A plain GNU Radio UDP Source can receive the datagrams as bytes, but downstream flowgraphs must parse and remove the VITA 49.2 headers before reinterpreting the payload as CI16 IQ, and should discard (or use) the interleaved type-4 context packets.
