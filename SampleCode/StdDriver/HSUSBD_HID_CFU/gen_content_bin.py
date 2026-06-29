#!/usr/bin/env python3
"""
gen_content_bin.py - Package a raw firmware .bin into the CFU host
"content" payload format expected by FwUpdateCfu.exe (ProcessSrecBin):

    repeated records of:
        [4 bytes  address  (little-endian)]
        [1 byte   length   (chunk size, max 52)]
        [length   data bytes]

This is NOT a raw image. FwUpdateCfu.exe parses the payload chunk by
chunk, so the input firmware must be re-packaged.

Usage:
    python gen_content_bin.py <fw.bin> --output <content.bin> [--chunk 52]
"""
import argparse, os, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fw_bin")
    ap.add_argument("--output", required=True)
    ap.add_argument("--chunk", type=int, default=52,
                    help="payload bytes per packet (multiple of 4, <=52)")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0,
                    help="starting address (default 0)")
    args = ap.parse_args()

    chunk = args.chunk
    if chunk < 4 or chunk > 52 or (chunk % 4):
        print("[ERR] --chunk must be multiple of 4 and <=52"); sys.exit(1)

    with open(args.fw_bin, "rb") as f:
        data = f.read()

    out = bytearray()
    addr = args.base
    off = 0
    n = 0
    while off < len(data):
        seg = data[off:off+chunk]
        # pad last chunk up to multiple of 4 with 0xFF
        if len(seg) % 4:
            seg = seg + b"\xFF" * (4 - len(seg) % 4)
        out += addr.to_bytes(4, "little")
        out += bytes([len(seg)])
        out += seg
        addr += len(seg)
        off += chunk
        n += 1

    with open(args.output, "wb") as f:
        f.write(out)

    print(f"[OK] {args.output}: {n} packets, {len(out)} bytes "
          f"(fw {len(data)} bytes, chunk {chunk})")

if __name__ == "__main__":
    main()
