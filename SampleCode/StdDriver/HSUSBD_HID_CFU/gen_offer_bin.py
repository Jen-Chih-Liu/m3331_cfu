#!/usr/bin/env python3
"""
gen_offer_bin.py  —  Generate CFU offer.bin for HSUSBD_HID_CFU (m3331)
=======================================================================
Parses Module/ComponentFwUpdate.c to detect current firmware versions
and accepted version threshold, then writes a 16-byte offer.bin that
matches the FWUPDATE_OFFER_COMMAND structure.

Version source priority (highest → lowest)
------------------------------------------
1. --version 0xXXXXXXXX          (explicit, always wins)
2. --fw-bin <file>                (read uint32 LE from compiled .bin at --version-offset)
                                   REQUIRES g_FwVersion placed at that offset via scatter file.
                                   AP0_OFFSET.sct / AP1_OFFSET.sct both place g_FwVersion
                                   (.fw_version section) at  ROM_BASE + ROM_SIZE - 0x10.
                                   So the correct --version-offset is:
                                     AP0 binary: 0x1FFF0  (ROM base 0x00000, ROM size 0x20000,
                                                          16 bytes from end = 0x1FFF0)
                                     AP1 binary: 0x1FFF0  (ROM base 0x20000, ROM size 0x20000,
                                                          objcopy strips base, so file offset = 0x1FFF0)
3. auto-detect from C source      (reads FW_VERSION_VALUE or *pVersion literal)
   then bump major byte + 1

Usage
-----
  # Auto-detect version from source, auto-increment, generate offer.bin
  python gen_offer_bin.py

  # Specify version explicitly
  python gen_offer_bin.py --version 0x7D000000

  # Specify component (AP0=0x30, AP1=0x31) and output file
  python gen_offer_bin.py --component 0x31 --output VirtualDevice_AP1.offer.bin

  # Force ignore version check (forceIgnoreVersion bit)
  python gen_offer_bin.py --force-version

  # Force immediate reset after update
  python gen_offer_bin.py --force-reset

  # Read version from compiled .bin (g_FwVersion is at ROM_SIZE-16, offset 0x1FFF0 for 128 KB bank)
  # Prerequisite: scatter file FW_VERSION section must exist (AP0/AP1_OFFSET.sct already has it)
  python gen_offer_bin.py --fw-bin Keil/obj/HSUSBD_HID_CFU.bin --version-offset 0x1FFF0

  # Show current info only
  python gen_offer_bin.py --info
"""

import struct
import re
import argparse
import sys
import os

# ---------------------------------------------------------------------------
# Paths (relative to this script's directory)
# ---------------------------------------------------------------------------
SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
CFW_UPDATE_C = os.path.join(SCRIPT_DIR, "Module", "ComponentFwUpdate.c")

# ---------------------------------------------------------------------------
# offer.bin field defaults (matching current HSUSBD_HID_CFU project)
# ---------------------------------------------------------------------------
DEFAULT_SEGMENT_NUMBER   = 0x00
DEFAULT_TOKEN            = 0x00
DEFAULT_HW_VARIANT_MASK  = 0x00000000
DEFAULT_PROTOCOL_REV     = 2          # CFU protocol revision 2
DEFAULT_BANK             = 0
DEFAULT_MILESTONE        = 0x00
DEFAULT_PRODUCT_ID       = 0x0000

COMPONENT_AP0 = 0x30
COMPONENT_AP1 = 0x31


# ---------------------------------------------------------------------------
# Version helpers
# ---------------------------------------------------------------------------

def version_to_str(v: int) -> str:
    major = (v >> 24) & 0xFF
    minor = (v >>  8) & 0xFFFF
    build = (v >>  0) & 0xFF
    return f"0x{v:08X}  (major={major}, minor={minor}, build={build})"


def bump_version(v: int) -> int:
    """Increment the major byte by 1 (simplest safe bump)."""
    major = ((v >> 24) & 0xFF) + 1
    if major > 0xFF:
        raise ValueError("Cannot bump version: major byte would overflow 0xFF")
    return (major << 24) | (v & 0x00FFFFFF)


# ---------------------------------------------------------------------------
# Source-code parser
# ---------------------------------------------------------------------------

def parse_source_versions(src_path: str) -> dict:
    """
    Scan ComponentFwUpdate.c for version defines and threshold.
    Returns a dict with keys: ver_ap0, ver_ap1, target_version, fw_version_macro.
    Values are ints or None if not found.

    Detection order for ver_ap0 / ver_ap1:
      1. #define FW_VERSION_VALUE 0xXXXXXXXX  (new canonical define)
      2. *pVersion = 0xXXXXXXXX literal in GetVersionImpl_compNN body
         (old style, still supported)
      3. *pVersion = g_FwVersion  → resolves back to FW_VERSION_VALUE if found
    """
    result = {"ver_ap0": None, "ver_ap1": None, "target_version": None, "fw_version_macro": None}
    if not os.path.isfile(src_path):
        return result

    with open(src_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    # 1. Canonical #define FW_VERSION_VALUE 0xXXXXXXXX
    m = re.search(r"#define\s+FW_VERSION_VALUE\s+(0x[0-9A-Fa-f]+)", content)
    if m:
        v = int(m.group(1), 16)
        result["fw_version_macro"] = v
        result["ver_ap0"] = v
        result["ver_ap1"] = v

    # 2. Fallback: *pVersion = 0xXXXXXXXX; literal inside GetVersionImpl_compNN
    if result["ver_ap0"] is None or result["ver_ap1"] is None:
        blocks = re.findall(
            r"GetVersionImpl_comp(\d+)\s*\([^)]*\)\s*\{[^}]*?\*pVersion\s*=\s*(0x[0-9A-Fa-f]+)\s*;",
            content, re.DOTALL
        )
        for comp, ver_str in blocks:
            v = int(ver_str, 16)
            if comp == "30" and result["ver_ap0"] is None:
                result["ver_ap0"] = v
            elif comp == "31" and result["ver_ap1"] is None:
                result["ver_ap1"] = v

    # 3. target_version threshold in ProcessOfferImpl
    m = re.search(r"target_version\s*=\s*(0x[0-9A-Fa-f]+)\s*;", content)
    if m:
        result["target_version"] = int(m.group(1), 16)

    return result


# ---------------------------------------------------------------------------
# Binary firmware parser (version at fixed offset)
# ---------------------------------------------------------------------------

def read_version_from_bin(bin_path: str, offset: int) -> int:
    """Read a uint32_t little-endian version from a compiled firmware binary."""
    with open(bin_path, "rb") as f:
        f.seek(offset)
        raw = f.read(4)
    if len(raw) < 4:
        raise ValueError(f"Cannot read 4 bytes at offset 0x{offset:X} in {bin_path}")
    return struct.unpack("<I", raw)[0]


# ---------------------------------------------------------------------------
# offer.bin builder
# ---------------------------------------------------------------------------

def build_offer_bin(
    component_id:    int,
    version:         int,
    segment_number:  int  = DEFAULT_SEGMENT_NUMBER,
    force_ignore_ver: bool = False,
    force_reset:     bool = False,
    token:           int  = DEFAULT_TOKEN,
    hw_variant_mask: int  = DEFAULT_HW_VARIANT_MASK,
    protocol_rev:    int  = DEFAULT_PROTOCOL_REV,
    bank:            int  = DEFAULT_BANK,
    milestone:       int  = DEFAULT_MILESTONE,
    product_id:      int  = DEFAULT_PRODUCT_ID,
) -> bytes:
    """
    Build the 16-byte FWUPDATE_OFFER_COMMAND binary blob.

    Memory layout (little-endian, byte 0 first):
      Byte  0    : segmentNumber
      Byte  1    : reserved[5:0] | forceImmediateReset[6] | forceIgnoreVersion[7]
      Byte  2    : componentId
      Byte  3    : token
      Bytes 4-7  : version          (uint32 LE)
      Bytes 8-11 : hwVariantMask    (uint32 LE)
      Byte  12   : protocolRevision[3:0] | bank[5:4] | reserved[7:6]
      Byte  13   : milestone[2:0]   | reserved[7:3]
      Bytes 14-15: productId        (uint16 LE)
    """
    flags = 0x00
    if force_ignore_ver:
        flags |= (1 << 7)
    if force_reset:
        flags |= (1 << 6)

    proto_bank_byte = (protocol_rev & 0x0F) | ((bank & 0x03) << 4)
    milestone_byte  = milestone & 0x07

    data = struct.pack(
        "<BBBBIIBBH",
        segment_number & 0xFF,   # byte 0
        flags          & 0xFF,   # byte 1
        component_id   & 0xFF,   # byte 2
        token          & 0xFF,   # byte 3
        version        & 0xFFFFFFFF,  # bytes 4-7
        hw_variant_mask & 0xFFFFFFFF, # bytes 8-11
        proto_bank_byte & 0xFF,  # byte 12
        milestone_byte  & 0xFF,  # byte 13
        product_id      & 0xFFFF # bytes 14-15
    )
    assert len(data) == 16, f"Unexpected offer size: {len(data)}"
    return data


def dump_offer(data: bytes, component_id: int, version: int) -> None:
    """Pretty-print the generated offer."""
    print("\n--- offer.bin content (16 bytes) ---")
    hex_str = " ".join(f"{b:02X}" for b in data)
    # Group as 4-byte words
    words = [hex_str[i*12:(i+1)*12].strip() for i in range(4)]
    print("  " + "  ".join(words))
    print()
    print(f"  Byte 00  segmentNumber     : 0x{data[0]:02X}")
    print(f"  Byte 01  flags             : 0x{data[1]:02X}"
          f"  (forceIgnoreVersion={(data[1]>>7)&1}, forceReset={(data[1]>>6)&1})")
    print(f"  Byte 02  componentId       : 0x{data[2]:02X}"
          f"  ({'AP0' if data[2]==COMPONENT_AP0 else 'AP1' if data[2]==COMPONENT_AP1 else '?'})")
    print(f"  Byte 03  token             : 0x{data[3]:02X}")
    ver_val = struct.unpack_from("<I", data, 4)[0]
    print(f"  Bytes04  version           : {version_to_str(ver_val)}")
    hvm_val = struct.unpack_from("<I", data, 8)[0]
    print(f"  Bytes08  hwVariantMask     : 0x{hvm_val:08X}")
    print(f"  Byte 12  protRev|bank      : 0x{data[12]:02X}"
          f"  (protRev={(data[12]&0xF)}, bank={(data[12]>>4)&0x3})")
    print(f"  Byte 13  milestone         : 0x{data[13]:02X}")
    pid_val = struct.unpack_from("<H", data, 14)[0]
    print(f"  Bytes14  productId         : 0x{pid_val:04X}")
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate CFU offer.bin for HSUSBD_HID_CFU (m3331)",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--component", type=lambda x: int(x, 0), default=None,
        metavar="ID",
        help=f"Component ID hex (default: 0x{COMPONENT_AP0:02X}=AP0 or 0x{COMPONENT_AP1:02X}=AP1, "
             f"auto-detected from source)"
    )
    parser.add_argument(
        "--version", type=lambda x: int(x, 0), default=None,
        metavar="VER",
        help="Offer version as hex uint32 (e.g. 0x7D000000). "
             "Default: auto-detect from source and bump major by +1."
    )
    parser.add_argument(
        "--fw-bin", type=str, default=None,
        metavar="PATH",
        help="Path to compiled firmware .bin file to read current version from"
    )
    parser.add_argument(
        "--version-offset", type=lambda x: int(x, 0), default=0x1FFF0,
        metavar="OFFSET",
        help="Byte offset of version uint32 inside --fw-bin (default: 0x1FFF0 = ROM_SIZE-16 of 128 KB bank)"
    )
    parser.add_argument(
        "--token", type=lambda x: int(x, 0), default=DEFAULT_TOKEN,
        metavar="TOKEN",
        help=f"Token byte (default: 0x{DEFAULT_TOKEN:02X})"
    )
    parser.add_argument(
        "--hw-variant", type=lambda x: int(x, 0), default=DEFAULT_HW_VARIANT_MASK,
        metavar="MASK",
        help=f"hwVariantMask uint32 (default: 0x{DEFAULT_HW_VARIANT_MASK:08X})"
    )
    parser.add_argument(
        "--product-id", type=lambda x: int(x, 0), default=DEFAULT_PRODUCT_ID,
        metavar="PID",
        help=f"productId uint16 (default: 0x{DEFAULT_PRODUCT_ID:04X})"
    )
    parser.add_argument(
        "--protocol-rev", type=int, default=DEFAULT_PROTOCOL_REV,
        metavar="REV",
        help=f"CFU protocol revision 0-15 (default: {DEFAULT_PROTOCOL_REV})"
    )
    parser.add_argument(
        "--bank", type=int, default=DEFAULT_BANK,
        metavar="BANK",
        help=f"Bank number 0-3 (default: {DEFAULT_BANK})"
    )
    parser.add_argument(
        "--milestone", type=int, default=DEFAULT_MILESTONE,
        metavar="MS",
        help=f"Milestone 0-7 (default: {DEFAULT_MILESTONE})"
    )
    parser.add_argument(
        "--force-version", action="store_true",
        help="Set forceIgnoreVersion bit (allow downgrade)"
    )
    parser.add_argument(
        "--force-reset", action="store_true",
        help="Set forceImmediateReset bit"
    )
    parser.add_argument(
        "--no-bump", action="store_true",
        help="Use the detected/source version as-is without incrementing the major byte. "
             "Use this for post-build offer generation where the offer should carry "
             "the exact firmware version of the new image."
    )
    parser.add_argument(
        "--output", type=str, default=None,
        metavar="FILE",
        help="Output file path (default: <component_name>.offer.bin in this directory)"
    )
    parser.add_argument(
        "--info", action="store_true",
        help="Show detected versions and exit (do not write file)"
    )
    args = parser.parse_args()

    # ------------------------------------------------------------------
    # Step 1: parse source to detect versions
    # ------------------------------------------------------------------
    print(f"[*] Parsing source: {CFW_UPDATE_C}")
    src_info = parse_source_versions(CFW_UPDATE_C)

    if src_info["fw_version_macro"] is not None:
        print(f"    #define FW_VERSION_VALUE      : {version_to_str(src_info['fw_version_macro'])}")
        print(f"    (g_FwVersion placed at ROM_BASE + ROM_SIZE - 0x10  via scatter FW_VERSION section)")
        print(f"    (binary file offset: 0x1FFF0 for 128 KB bank)")
    else:
        print("    #define FW_VERSION_VALUE      : (not found — scatter section not configured yet)")

    if src_info["ver_ap0"] is not None:
        print(f"    GetVersionImpl_comp30  AP0    : {version_to_str(src_info['ver_ap0'])}")
    else:
        print("    GetVersionImpl_comp30  AP0    : (not found in source)")

    if src_info["ver_ap1"] is not None:
        print(f"    GetVersionImpl_comp31  AP1    : {version_to_str(src_info['ver_ap1'])}")
    else:
        print("    GetVersionImpl_comp31  AP1    : (not found in source)")

    if src_info["target_version"] is not None:
        print(f"    ProcessOfferImpl threshold    : {version_to_str(src_info['target_version'])}")
        print(f"    (offer version must be >  0x{src_info['target_version']:08X}  to be ACCEPTED)")
    else:
        print("    ProcessOfferImpl threshold    : (not found in source)")

    if args.info:
        return

    # ------------------------------------------------------------------
    # Step 2: determine component ID
    # ------------------------------------------------------------------
    if args.component is not None:
        component_id = args.component
    else:
        # Default: AP0 (0x30) — most common case when AP0 is the running bootloader
        component_id = COMPONENT_AP0
        print(f"\n[*] --component not specified, defaulting to 0x{component_id:02X} (AP0)")

    comp_name = "AP0" if component_id == COMPONENT_AP0 else \
                "AP1" if component_id == COMPONENT_AP1 else \
                f"0x{component_id:02X}"

    # ------------------------------------------------------------------
    # Step 3: determine offer version
    # ------------------------------------------------------------------
    if args.version is not None:
        offer_version = args.version
        print(f"\n[*] Using specified version: {version_to_str(offer_version)}")
    elif args.fw_bin is not None:
        # Read version from compiled binary
        fw_bin = os.path.abspath(args.fw_bin)
        print(f"\n[*] Reading version from binary: {fw_bin}  (offset 0x{args.version_offset:X})")
        current_version = read_version_from_bin(fw_bin, args.version_offset)
        print(f"    Detected version: {version_to_str(current_version)}")
        if current_version == 0xFFFFFFFF:
            # 0xFFFFFFFF = erased flash: scatter FW_VERSION section not yet in the binary.
            # Cause: Keil used a cached .axf built before the scatter file was changed.
            # Fix:   Project > Clean, then rebuild in Keil so the new scatter is linked.
            # Workaround for this build: fall back to source-detected version.
            print(f"    [!] 0xFFFFFFFF detected \u2014 FW_VERSION scatter not effective yet.")
            print(f"        Perform a full Clean + Rebuild in Keil to fix permanently.")
            print(f"        Falling back to source-detected version for this build.")
            current_version = src_info["ver_ap0"] if component_id == COMPONENT_AP0 else src_info["ver_ap1"]
            if current_version is None:
                print("\n[!] Source version also unavailable. Use --version explicitly.")
                sys.exit(1)
            print(f"    Source fallback : {version_to_str(current_version)}")
        if args.no_bump:
            offer_version = current_version
            print(f"    Offer version   : {version_to_str(offer_version)}  (--no-bump)")
        else:
            offer_version = bump_version(current_version)
            print(f"    Bumped version  : {version_to_str(offer_version)}")
    else:
        # Auto-detect from source
        current_version = src_info["ver_ap0"] if component_id == COMPONENT_AP0 else src_info["ver_ap1"]
        if current_version is None:
            print("\n[!] Could not detect current version from source.")
            print("    Please specify --version explicitly.")
            sys.exit(1)
        if args.no_bump:
            offer_version = current_version
            print(f"\n[*] Source version (--no-bump): {version_to_str(offer_version)}")
        else:
            offer_version = bump_version(current_version)
            print(f"\n[*] Auto-bump: {version_to_str(current_version)}  \u2192  {version_to_str(offer_version)}")

    # ------------------------------------------------------------------
    # Step 4: validate against threshold
    # ------------------------------------------------------------------
    threshold = src_info["target_version"]
    if threshold is not None:
        if offer_version <= threshold and not args.force_version:
            print(f"\n[!] WARNING: offer version 0x{offer_version:08X} is NOT > threshold 0x{threshold:08X}.")
            print("    The MCU's ProcessOfferImpl() will REJECT this offer.")
            print("    Use --force-version to set forceIgnoreVersion=1, or increase --version.")
            choice = input("    Continue anyway? [y/N]: ").strip().lower()
            if choice != "y":
                print("Aborted.")
                sys.exit(1)

    # ------------------------------------------------------------------
    # Step 5: build offer.bin
    # ------------------------------------------------------------------
    data = build_offer_bin(
        component_id     = component_id,
        version          = offer_version,
        force_ignore_ver = args.force_version,
        force_reset      = args.force_reset,
        token            = args.token,
        hw_variant_mask  = args.hw_variant,
        protocol_rev     = args.protocol_rev,
        bank             = args.bank,
        milestone        = args.milestone,
        product_id       = args.product_id,
    )

    dump_offer(data, component_id, offer_version)

    # ------------------------------------------------------------------
    # Step 6: write output file
    # ------------------------------------------------------------------
    if args.output is not None:
        out_path = args.output
    else:
        ver_tag  = f"{(offer_version>>24)&0xFF:02X}{(offer_version>>8)&0xFFFF:04X}{offer_version&0xFF:02X}"
        out_name = f"VirtualDevice_{comp_name}_v{ver_tag}.offer.bin"
        out_path = os.path.join(SCRIPT_DIR, out_name)

    with open(out_path, "wb") as f:
        f.write(data)

    print(f"[OK] Written: {out_path}  ({len(data)} bytes)")
    print()
    print("  Use with FwUpdateCfu.exe:")
    offer_file = os.path.basename(out_path)
    print(f"    FwUpdateCfu.exe update Cfg.cfg {offer_file} <payload.bin>")


if __name__ == "__main__":
    main()
