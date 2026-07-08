#!/usr/bin/env python3
import argparse
import math
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate deterministic CI16 interleaved IQ samples.")
    parser.add_argument("--output", default="simulator/assets/fsk_20mhz.c16")
    parser.add_argument("--pattern", choices=("fsk", "ramp", "tone"), default="fsk",
                        help="fsk: FSK-like test signal; ramp: sample index encoded in I/Q "
                             "(position assertions); tone: clean complex exponential (FFT checks)")
    parser.add_argument("--sample-rate", type=float, default=24_576_000.0)
    parser.add_argument("--samples", type=int, default=24_576)
    parser.add_argument("--tone-hz", type=float, default=1_000_000.0)
    parser.add_argument("--deviation-hz", type=float, default=120_000.0)
    parser.add_argument("--symbol-rate", type=float, default=12_000.0)
    parser.add_argument("--amplitude", type=float, default=8192.0)
    args = parser.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    phase = 0.0
    with output.open("wb") as f:
        for n in range(args.samples):
            if args.pattern == "ramp":
                # Sample index recoverable from the payload: I carries the low 15 bits,
                # Q the next 15, so any replayed sample identifies its file position.
                i = n & 0x7FFF
                q = (n >> 15) & 0x7FFF
            elif args.pattern == "tone":
                phase = 2.0 * math.pi * args.tone_hz * n / args.sample_rate
                i = int(round(args.amplitude * math.cos(phase)))
                q = int(round(args.amplitude * math.sin(phase)))
            else:
                t = n / args.sample_rate
                bit = 1.0 if int(t * args.symbol_rate) % 2 == 0 else -1.0
                freq = args.tone_hz + bit * args.deviation_hz
                phase += 2.0 * math.pi * freq / args.sample_rate
                i = int(round(args.amplitude * math.cos(phase)))
                q = int(round(args.amplitude * math.sin(phase)))
            f.write(struct.pack("<hh", i, q))

    print(f"wrote {args.samples} CI16 {args.pattern} samples to {output}")


if __name__ == "__main__":
    main()
