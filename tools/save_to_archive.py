#!/usr/bin/env python3
"""Prototype: write hired-pawn gear from DDDA.sav back into the source .pawn archive.

This is the Path-A design:
    DDDA.sav (zlib-compressed XML)
        -> mEquipItem records for mOwnerId=2 / mOwnerId=3
            -> pawnxfs patches at XFS offsets (record base + {2,8})
                -> pawnblob xfs-poke <source.pawn> (in place)

The transformation from save.mFlag to xfs.mFlag is `xfs_flag = save_flag & ~0x80`:
bit 7 is a runtime "equipped" marker that the game re-applies on load.

Usage:
    save_to_archive.py <DDDA.sav> <mod_slot:0|1> <source.pawn>
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib


PAWNBLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        '..', 'pawnblob', 'pawnblob')

XFS_GEAR_BASE   = 0x2098    # first mEquipItem record start inside the XFS
XFS_GEAR_STRIDE = 0x46      # record stride
XFS_GEAR_COUNT  = 12
XFS_ITEM_OFF    = 0x02      # s16 mItemNo in the XFS record
XFS_FLAG_OFF    = 0x08      # u32 mFlag in the XFS record
SAVE_EQUIPPED_BIT = 0x80    # bit 7 of mFlag: set in memory/save, cleared in XFS


def decompress_save(path: str) -> bytes:
    """DDDA.sav layout: 32-byte header (see ddsavetool main.cpp) followed by
    zlib-deflated XML. We only need the decompressed payload."""
    with open(path, 'rb') as f:
        hdr = f.read(32)
        rest = f.read()
    _, real_size, comp_size, *_ = struct.unpack('<8I', hdr)
    return zlib.decompress(rest[:comp_size])


RECORD_PATTERN = re.compile(
    r'<class type="sItemManager::cITEM_PARAM_DATA">\s*'
    r'<s16 name="data\.mNum" value="(-?\d+)"/>\s*'
    r'<s16 name="data\.mItemNo" value="(-?\d+)"/>\s*'
    r'<u32 name="data\.mFlag" value="(\d+)"/>\s*'
    r'<u16 name="data\.mChgNum" value="(\d+)"/>\s*'
    r'<u16 name="data\.mDay1" value="(\d+)"/>\s*'
    r'<u16 name="data\.mDay2" value="(\d+)"/>\s*'
    r'<u16 name="data\.mDay3" value="(\d+)"/>\s*'
    r'<s8 name="data\.mMutationPool" value="(-?\d+)"/>\s*'
    r'<s8 name="data\.mOwnerId" value="(-?\d+)"/>\s*'
    r'<u32 name="data\.mKey" value="(\d+)"/>'
)

ARRAY_PATTERN = re.compile(
    r'<array name="mEquipItem" type="class" count="12">(.*?)</array>', re.DOTALL
)


def extract_equipment_for_owner(xml_bytes: bytes, owner_id: int) -> list[tuple]:
    """Return the 12-record list (mNum, mItemNo, mFlag, ...) for the mEquipItem
    array whose entries are tagged with the given mOwnerId. Convention:
        owner 0 = Arisen, 1 = Main Pawn, 2 = Hired Pawn 1, 3 = Hired Pawn 2.
    Empty slots get mOwnerId=0 regardless of block ownership, so we classify by
    the FIRST non-empty record's mOwnerId, or fall back to block order."""
    xml = xml_bytes.decode('utf-8', errors='replace')

    for block_idx, m in enumerate(ARRAY_PATTERN.finditer(xml)):
        recs = list(RECORD_PATTERN.finditer(m.group(1)))
        if len(recs) != 12:
            continue
        parsed = [tuple(int(g) for g in r.groups()) for r in recs]
        # Find a non-empty record's ownerId to identify the block.
        block_owner = next((p[8] for p in parsed if p[1] != -1), None)
        if block_owner == owner_id:
            return parsed
        # For Arisen (owner 0), all records might legitimately have mOwnerId=0;
        # fall back to block position (CMC index).
        if owner_id == 0 and block_idx == 0 and block_owner in (None, 0):
            return parsed

    raise RuntimeError(f"no mEquipItem block found for ownerId={owner_id}")


def build_patches(records: list[tuple]) -> list[tuple]:
    """Return [(xfs_offset, patch_bytes), ...] that express these equipment
    records in XFS terms.  Patches cover only mItemNo and mFlag — the rest of
    each 0x46-byte record is left untouched."""
    patches = []
    for slot, rec in enumerate(records):
        _num, item_no, save_flag, *_ = rec
        base = XFS_GEAR_BASE + slot * XFS_GEAR_STRIDE
        # mItemNo s16 LE
        patches.append((base + XFS_ITEM_OFF, struct.pack('<h', item_no)))
        # mFlag u32 LE, stripped of the 0x80 equipped bit
        patches.append((base + XFS_FLAG_OFF,
                        struct.pack('<I', save_flag & ~SAVE_EQUIPPED_BIT)))
    return patches


def apply_patches(in_path: str, out_path: str, patches: list[tuple]):
    """Drive pawnblob xfs-poke once per (offset, bytes) tuple. Each call reads
    and rewrites the whole blob, so stage through a temp file between patches."""
    src = in_path
    tmp = out_path + '.tmp'
    try:
        for i, (off, data) in enumerate(patches):
            dst = out_path if i == len(patches) - 1 else tmp
            hex_patch = data.hex()
            subprocess.run(
                [PAWNBLOB, 'xfs-poke', src, dst, f'{off:X}', hex_patch],
                check=True, stdout=subprocess.DEVNULL,
            )
            src = dst
    finally:
        if os.path.exists(tmp) and tmp != out_path:
            os.remove(tmp)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('save', help='DDDA.sav (compressed save file)')
    ap.add_argument('slot', type=int, choices=[0, 1],
                    help='hired pawn mod slot (0 or 1)')
    ap.add_argument('archive', help='source .pawn archive to write back into')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    owner_id = 2 + args.slot    # CMC[2]=Hired1, CMC[3]=Hired2
    print(f"save         : {args.save}")
    print(f"slot/owner   : mod slot {args.slot}  ->  mOwnerId {owner_id}")
    print(f"archive      : {args.archive}")

    xml = decompress_save(args.save)
    print(f"decompressed : {len(xml)} bytes")

    records = extract_equipment_for_owner(xml, owner_id)
    print(f"records      : {len(records)} (slots 0..{len(records)-1})")

    for slot, (num, item, flag, chg, d1, d2, d3, mut, own, key) in enumerate(records):
        print(f"  [{slot:>2}]  num={num:>2} item={item:>5} save_flag=0x{flag:08X} "
              f"-> xfs_flag=0x{flag & ~SAVE_EQUIPPED_BIT:08X}  "
              f"(chg={chg} mut={mut} own={own} key={key})")

    patches = build_patches(records)
    print(f"\nwill apply {len(patches)} patches to {args.archive}")

    if args.dry_run:
        print("(dry-run, stopping here)")
        return 0

    apply_patches(args.archive, args.archive, patches)
    print("done — re-unpack the archive to verify gear:")
    print(f"    pawnblob/pawnblob unpack {args.archive} /tmp/after.xfs")
    print(f"    tools/inspect_gear_xfs.py {args.archive}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
