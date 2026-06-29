#!/usr/bin/env python3
"""
gen_checksum_bin.py  —  Embed 32-bit byte-sum checksum into firmware .bin
=========================================================================
Calculates the 32-bit byte sum of the padded firmware image (with the
checksum slot zeroed), writes the result at offset (ROM_SIZE - 8), and
saves a new file as  <original_name>_sum.bin.

Flash layout for the last 16 bytes of each 128 KB bank (0x20000):
  Offset 0x1FFF0  [4 bytes]  FW_VERSION  (.fw_version section, g_FwVersion)
  Offset 0x1FFF4  [4 bytes]  (unused / 0xFF)
  Offset 0x1FFF8  [4 bytes]  FW_CHECKSUM  ← written by this script
  Offset 0x1FFFC  [4 bytes]  (unused / 0xFF)

Algorithm
---------
  1. Pad (or trim) the binary to exactly ROM_SIZE bytes, filling with 0xFF.
  2. Zero the 4-byte checksum slot at (ROM_SIZE - 8).
  3. Compute  checksum = sum(all bytes) & 0xFFFFFFFF  (32-bit wrapping sum).
  4. Write checksum as little-endian uint32 at (ROM_SIZE - 8).
  5. Save as  <stem>_sum.bin  (or --output path).

Usage
-----
  # Default: ROM_SIZE = 0x20000, output = HSUSBD_HID_CFU_sum.bin
  python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin

  # Explicit ROM size and output path
  python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin --rom-size 0x20000 --output out.bin

  # Show info only (no output written)
  python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin --info
"""

import struct
import argparse
import os
import sys

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEFAULT_ROM_SIZE = 0x20000          # 128 KB — matches AP0/AP1 bank size
CHECKSUM_OFFSET_FROM_END = 8       # FW_CHECKSUM is at ROM_SIZE - 8


# ---------------------------------------------------------------------------
# Core logic
# ---------------------------------------------------------------------------

def compute_and_embed(bin_path: str, rom_size: int, output: str, info_only: bool) -> None:
    if not os.path.isfile(bin_path):
        print(f"[!] File not found: {bin_path}")
        sys.exit(1)

    with open(bin_path, "rb") as f:
        raw = bytearray(f.read())

    file_size = len(raw)
    chk_offset = rom_size - CHECKSUM_OFFSET_FROM_END   # 0x1FFF8 for 128 KB

    if chk_offset < 0 or chk_offset + 4 > rom_size:
        print(f"[!] Checksum offset 0x{chk_offset:X} is out of range for ROM_SIZE 0x{rom_size:X}")
        sys.exit(1)

    # Pad to rom_size with 0xFF (flash erased value)
    if file_size < rom_size:
        raw += b'\xFF' * (rom_size - file_size)
        print(f"[*] Padded binary from 0x{file_size:X} to 0x{rom_size:X} bytes (fill 0xFF)")
    elif file_size > rom_size:
        print(f"[!] WARNING: file size 0x{file_size:X} > ROM_SIZE 0x{rom_size:X}, truncating")
        raw = raw[:rom_size]

    # Print current FW_VERSION for reference (offset ROM_SIZE - 16)
    ver_offset = rom_size - 16      # 0x1FFF0
    fw_ver = struct.unpack_from("<I", raw, ver_offset)[0]
    print(f"[*] FW_VERSION  @ 0x{ver_offset:05X} : 0x{fw_ver:08X}"
          f"  (major=0x{(fw_ver>>24)&0xFF:02X}"
          f"  minor=0x{(fw_ver>>8)&0xFFFF:04X}"
          f"  build=0x{fw_ver&0xFF:02X})")

    # Show existing checksum slot content before overwrite
    old_chk = struct.unpack_from("<I", raw, chk_offset)[0]
    print(f"[*] Checksum    @ 0x{chk_offset:05X} : 0x{old_chk:08X}  (current / will be overwritten)")

    if info_only:
        print("[*] --info mode: no output written.")
        return

    # Zero the checksum slot, then compute sum
    raw[chk_offset : chk_offset + 4] = b'\x00\x00\x00\x00'

    # Calculation range: bank start up to (but not including) the checksum
    # slot, i.e. bytes 0x00000 .. chk_offset-1. This matches Boot_ChecksumOK().
    calc_start = 0x00000
    calc_end   = chk_offset - 1         # inclusive
    print(f"[*] Checksum range  : 0x{calc_start:05X} - 0x{calc_end:05X}  ({chk_offset} bytes total)")
    print(f"    (bytes at/after slot 0x{chk_offset:05X} excluded)")

    checksum = sum(raw[calc_start:chk_offset]) & 0xFFFFFFFF

    # Embed checksum
    struct.pack_into("<I", raw, chk_offset, checksum)

    with open(output, "wb") as f:
        f.write(raw)

    print(f"[*] Checksum (new) 0x{checksum:08X}  written at offset 0x{chk_offset:05X}")
    print(f"[*] Output  : {output}  ({len(raw)} bytes)")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Embed 32-bit byte-sum checksum into a firmware .bin for HSUSBD_HID_CFU (m3331)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "bin_file",
        metavar="BIN",
        help="Input firmware .bin file (e.g. Keil/obj/HSUSBD_HID_CFU.bin)"
    )
    parser.add_argument(
        "--rom-size", type=lambda x: int(x, 0), default=DEFAULT_ROM_SIZE,
        metavar="SIZE",
        help=f"ROM bank size in bytes (default: 0x{DEFAULT_ROM_SIZE:X} = 128 KB)"
    )
    parser.add_argument(
        "--output", type=str, default=None,
        metavar="FILE",
        help="Output file path (default: <stem>_sum.bin next to input file)"
    )
    parser.add_argument(
        "--info", action="store_true",
        help="Show current FW_VERSION and checksum slot; do not write output file"
    )
    args = parser.parse_args()

    bin_path = os.path.abspath(args.bin_file)

    # Build default output path: <stem>_sum.bin
    if args.output is None:
        stem, ext = os.path.splitext(bin_path)
        output = stem + "_sum" + (ext if ext else ".bin")
    else:
        output = os.path.abspath(args.output)

    print(f"[*] Input   : {bin_path}  ({os.path.getsize(bin_path)} bytes)")
    print(f"[*] ROM size: 0x{args.rom_size:X} ({args.rom_size} bytes)")
    print(f"[*] FW_VERSION  offset : 0x{args.rom_size - 16:05X}  (ROM_SIZE - 16)")
    print(f"[*] Checksum    offset : 0x{args.rom_size - 8:05X}  (ROM_SIZE - 8)")

    compute_and_embed(bin_path, args.rom_size, output, args.info)


if __name__ == "__main__":
    main()
