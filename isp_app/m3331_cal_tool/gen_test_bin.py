#!/usr/bin/env python3
"""Generate a 4 KB test binary for m3331_cal_tool (Calibration / OEM region).

The output file is exactly 4096 bytes (one flash region). By default each
4-byte word is filled with an incrementing little-endian counter so the data
is easy to recognise when read back for verification.

Usage:
    python gen_test_bin.py                      # -> test_4k.bin (incrementing pattern)
    python gen_test_bin.py -o calib.bin         # custom output name
    python gen_test_bin.py --pattern 0x5A       # fill every byte with 0x5A
    python gen_test_bin.py --random             # fill with random bytes
    python gen_test_bin.py --size 0x1000        # custom size (default 4096)
"""
import argparse
import os
import struct
import sys


def build_data(size: int, mode: str, pattern: int) -> bytes:
    if mode == "byte":
        return bytes([pattern & 0xFF]) * size
    if mode == "random":
        return os.urandom(size)
    # default: incrementing 32-bit little-endian counter per word
    words = size // 4
    buf = bytearray()
    for i in range(words):
        buf += struct.pack("<I", i & 0xFFFFFFFF)
    # pad any remaining tail bytes (size not multiple of 4) with 0xFF
    buf += b"\xFF" * (size - len(buf))
    return bytes(buf)


def main() -> int:
    p = argparse.ArgumentParser(description="Generate a 4 KB test bin file.")
    p.add_argument("-o", "--output", default="test_4k.bin", help="output file name")
    p.add_argument("--size", type=lambda x: int(x, 0), default=0x1000,
                   help="size in bytes (default 0x1000 = 4096)")
    p.add_argument("--pattern", type=lambda x: int(x, 0), default=None,
                   help="fill every byte with this value, e.g. 0x5A")
    p.add_argument("--random", action="store_true", help="fill with random bytes")
    args = p.parse_args()

    if args.random:
        mode, pattern = "random", 0
    elif args.pattern is not None:
        mode, pattern = "byte", args.pattern
    else:
        mode, pattern = "incr", 0

    data = build_data(args.size, mode, pattern)
    with open(args.output, "wb") as f:
        f.write(data)

    print(f"Wrote {len(data)} bytes ({len(data) / 1024:.1f} KB) to {args.output} "
          f"[mode={mode}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
