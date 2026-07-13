#!/usr/bin/env python3
import argparse
import math
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate deterministic CI16 interleaved IQ samples.")
    parser.add_argument("--output", default="simulator/assets/fsk_20mhz.c16")
    parser.add_argument("--pattern", choices=("fsk", "ramp", "tone", "multitone"), default="fsk",
                        help="fsk: FSK-like test signal; ramp: sample index encoded in I/Q "
                             "(position assertions); tone: clean complex exponential (FFT checks); "
                             "multitone: several loop-periodic tones spread over the band "
                             "(--tones, DDC sub-band extraction checks)")
    parser.add_argument("--sample-rate", type=float, default=24_576_000.0)
    parser.add_argument("--samples", type=int, default=24_576)
    parser.add_argument("--tone-hz", type=float, default=1_000_000.0)
    parser.add_argument("--tones", type=str, default="",
                        help="multitone: comma-separated baseband tone offsets in Hz (each is "
                             "snapped to a whole number of cycles per file so the loop is "
                             "seamless). Default: 8 tones across +/-40%% of the sample rate.")
    parser.add_argument("--deviation-hz", type=float, default=120_000.0)
    parser.add_argument("--symbol-rate", type=float, default=12_000.0)
    parser.add_argument("--amplitude", type=float, default=8192.0)
    args = parser.parse_args()

    multitone_hz = []
    if args.pattern == "multitone":
        if args.tones:
            requested = [float(v) for v in args.tones.split(",")]
        else:
            requested = [args.sample_rate * f for f in
                         (-0.4, -0.27, -0.15, -0.05, 0.03, 0.12, 0.24, 0.38)]
        # Snap each tone to a whole number of cycles per file so the looped asset has no
        # seam discontinuity and DFT bins are exact in loop-periodic capture windows.
        multitone_hz = [round(f * args.samples / args.sample_rate) * args.sample_rate / args.samples
                        for f in requested]

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
            elif args.pattern == "multitone":
                acc_i = 0.0
                acc_q = 0.0
                for tone in multitone_hz:
                    phase = 2.0 * math.pi * tone * n / args.sample_rate
                    acc_i += math.cos(phase)
                    acc_q += math.sin(phase)
                scale = args.amplitude / max(len(multitone_hz), 1)
                i = int(round(scale * acc_i))
                q = int(round(scale * acc_q))
            else:
                t = n / args.sample_rate
                bit = 1.0 if int(t * args.symbol_rate) % 2 == 0 else -1.0
                freq = args.tone_hz + bit * args.deviation_hz
                phase += 2.0 * math.pi * freq / args.sample_rate
                i = int(round(args.amplitude * math.cos(phase)))
                q = int(round(args.amplitude * math.sin(phase)))
            f.write(struct.pack("<hh", i, q))

    print(f"wrote {args.samples} CI16 {args.pattern} samples to {output}")
    if multitone_hz:
        print("tones (Hz): " + ", ".join(f"{tone:.3f}" for tone in multitone_hz))


if __name__ == "__main__":
    main()
