# VITA 49.2 UDP Output

The simulator sends one VITA 49.2 IF-data packet per UDP datagram for each enabled stream.

## Packet Layout

The simulator's current packet subset is:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | VRT/VITA IF-data header, big-endian. |
| 4 | 4 | Stream ID, big-endian. |
| 8 | 4 | Integer timestamp seconds, big-endian. |
| 12 | 8 | Fractional timestamp in picoseconds, big-endian. |
| 20 | variable | CI16 IQ payload. |

Header fields:

| Field | Value |
| --- | --- |
| Packet type | `1`, IF data. |
| VITA 49.2 indicator | Packet-specific indicator bit 25 set. |
| TSI | `1`, UTC. |
| TSF | `2`, real-time fractional timestamp. |
| Sequence | 4-bit per-stream packet counter. |
| Packet size | Total packet size in 32-bit words. |

Stream IDs use this layout:

```text
0x53440000 | (receiver_id << 8) | stream_index
```

`stream_index` is `0` for the receiver bandwidth stream and `1..4` for DDC IDs `0..3`.

## IQ Payload

The packet payload is unchanged CI16 IQ:

```text
little-endian int16 I, little-endian int16 Q
```

`stream_block_samples` controls the CI16 payload size in complex samples. Total UDP datagram size is:

```text
20 + stream_block_samples * 4 bytes
```

## GNU Radio Note

A plain GNU Radio UDP Source can receive the datagrams as bytes, but downstream flowgraphs must parse and remove the VITA 49.2 header before reinterpreting the payload as CI16 IQ.
