#!/usr/bin/env python3
"""Decode gear slots from a hired-pawn memory dump.

Reads one of the dumps produced by pawndb's hire_dump_one:
    HIRE_DUMP_SIZE  = 0x8000 bytes of live memory
    HIRE_DUMP_PRE   = 0x2000 bytes before the slot's mData

So in a dump, mData sits at dump offset 0x2000, and each dump byte at
offset X corresponds to pBase + (HIRE_SLOT_MDATA_OFF(slot) - HIRE_DUMP_PRE + X).

The filename carries the slot number (e.g. "..._slot0_L002_0094BA89_hire.bin"),
which lets us also print the pBase-relative offset of each slot.

Usage:
    inspect_gear_dump.py <dump.bin>
        [--base OFFSET]       # dump offset of slot 0 item_id; default: scan
        [--stride N]
        [--slots N]
        [--quality-off N]
"""

import argparse
import os
import re
import struct
import sys


STAR_MASK = 0x678
STAR_LABELS = {
    0:       'base',
    1 << 3:  '1*',
    1 << 4:  '2*',
    1 << 5:  '3*',
    1 << 6:  'DF',
    1 << 9:  'SR',
    1 << 10: 'GR',
}

# Layout constants mirrored from pawndb.c.
HIRE_SLOT0_MDATA_OFF = 0x0AAAB0
HIRE_SLOT_STRIDE     = 0x1660
HIRE_DUMP_PRE        = 0x2000


def load_items() -> dict[int, str]:
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'item_ids.txt')
    out = {}
    if os.path.isfile(p):
        for line in open(p):
            s = line.strip()
            if not s:
                continue
            head, _, name = s.partition(' ')
            try:
                out[int(head)] = name
            except ValueError:
                pass
    return out


def quality_label(flags: int) -> str:
    masked = flags & STAR_MASK
    lbl = STAR_LABELS.get(masked)
    return lbl if lbl else f'multi?(0x{masked:03x})'


def slot_from_filename(path: str) -> int | None:
    m = re.search(r'_slot(\d+)_', os.path.basename(path))
    return int(m.group(1)) if m else None


def parse_int(s: str) -> int:
    s = str(s).strip().lower()
    return int(s, 16) if s.startswith('0x') else int(s)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument('file')
    ap.add_argument('--base', default=None,
                    help='dump offset of slot 0 item_id (no default — pin '
                         'with find_u16.py first)')
    ap.add_argument('--stride', default='0x0C',
                    help='byte stride between consecutive gear slots '
                         '(default 0x0C; try 0x20, 0x30 if weird)')
    ap.add_argument('--slots', type=int, default=12)
    ap.add_argument('--quality-off', default='0x10',
                    help='u16 flags offset relative to item_id (default 0x10)')
    args = ap.parse_args()

    data = open(args.file, 'rb').read()
    items = load_items()

    slot = slot_from_filename(args.file)
    if slot is not None:
        mdata_abs = HIRE_SLOT0_MDATA_OFF + slot * HIRE_SLOT_STRIDE
        dump_origin_abs = mdata_abs - HIRE_DUMP_PRE
        print(f"slot     : {slot} (from filename)")
        print(f"mData    : pBase + 0x{mdata_abs:06X}  (dump+0x{HIRE_DUMP_PRE:04X})")
    else:
        dump_origin_abs = None
        print("slot     : unknown (filename doesn't match '_slotN_')")

    if args.base is None:
        print("\nno --base given; use find_u16.py to locate a known item_id "
              "first, then re-run with --base <dump_offset>.")
        return 0

    base = parse_int(args.base)
    stride = parse_int(args.stride)
    q_off = parse_int(args.quality_off)

    print(f"\nbase     : dump+0x{base:04X}"
          + (f"  (pBase+0x{dump_origin_abs + base:06X})"
             if dump_origin_abs is not None else ''))
    print(f"stride   : 0x{stride:X}")
    print(f"quality  : item_id + 0x{q_off:X}")
    print()
    print(f"{'slot':>4}  {'dump_off':>9}  "
          + ("pBase+".ljust(11) if dump_origin_abs is not None else "")
          + f"{'item':>5}  {'q_off':>9}  {'flags':>6}  {'quality':<10}  name")
    for i in range(args.slots):
        off = base + i * stride
        if off + 2 > len(data):
            break
        item_id = struct.unpack_from('<H', data, off)[0]
        q_offset = off + q_off
        flags = (struct.unpack_from('<H', data, q_offset)[0]
                 if q_offset + 2 <= len(data) else 0)
        qlbl = quality_label(flags) if item_id else '-'
        name = '(empty)' if item_id == 0 else items.get(item_id, '?')
        pbase_str = (f"0x{dump_origin_abs + off:06X} "
                     if dump_origin_abs is not None else '')
        print(f"{i:>4}  0x{off:07X}  {pbase_str}"
              f"{item_id:>5}  0x{q_offset:07X}  0x{flags:04X}  "
              f"{qlbl:<10}  {name}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
