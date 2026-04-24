#!/usr/bin/env python3
"""Decode gear slots from a DDDA .pawn or unpacked .xfs file.

The weapon item_id is known to live at XFS 0x209A (u16, LE) — validated
by the rusted-staff hire test. Other slots and the quality field are
hypotheses we're trying to pin. This tool prints a table of u16 values at
a base offset + N slots × stride, plus the hypothesized quality u16 at
each slot, so candidate layouts can be eyeballed at a glance.

Quality decoding uses the mask 0x678 and the {Star0, Star1..3, DF, SR, GR}
bits from ddda-dinput8's ItemEditor.cpp.

Usage:
    inspect_gear_xfs.py <file.pawn|file.xfs>
        [--base OFFSET] [--stride N] [--slots N] [--quality-off N]

Accepts either a .pawn file (shelled out to pawnblob unpack for you) or
an already-inflated .xfs.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


PAWNBLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        '..', 'pawnblob', 'pawnblob')

# Per dinput8/ItemEditor.cpp. Quality bits are a sparse mask inside an item's
# flags u16 (not a contiguous range), hence the explicit lookup table.
STAR_MASK = 0x678
STAR_LABELS = {
    0:       'base',
    1 << 3:  '1*',
    1 << 4:  '2*',
    1 << 5:  '3*',
    1 << 6:  'DF',   # Dragon Forged
    1 << 9:  'SR',   # Silver Rarified
    1 << 10: 'GR',   # Gold Rarified
}


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


def unpack_pawn(path: str) -> bytes:
    with tempfile.NamedTemporaryFile(suffix='.xfs', delete=False) as tf:
        out = tf.name
    try:
        subprocess.run([PAWNBLOB, 'unpack', path, out],
                       check=True, stdout=subprocess.DEVNULL)
        return open(out, 'rb').read()
    finally:
        if os.path.exists(out):
            os.remove(out)


def parse_int(s: str) -> int:
    s = str(s).strip().lower()
    return int(s, 16) if s.startswith('0x') else int(s)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument('file')
    ap.add_argument('--base', default='0x209A',
                    help='item_id u16 offset of slot 0 (default 0x209A = weapon)')
    ap.add_argument('--stride', default='0x0C',
                    help='byte stride between consecutive gear slots (default 0x0C)')
    ap.add_argument('--slots', type=int, default=12,
                    help='number of slots to print (default 12)')
    ap.add_argument('--quality-off', default='0x10',
                    help='u16 flags offset relative to item_id '
                         '(default 0x10, per dinput8; try 0x00..0x08 if this '
                         'prints nonsense)')
    args = ap.parse_args()

    data = (unpack_pawn(args.file) if args.file.endswith('.pawn')
            else open(args.file, 'rb').read())
    items = load_items()

    base = parse_int(args.base)
    stride = parse_int(args.stride)
    q_off = parse_int(args.quality_off)

    print(f"file     : {args.file}  ({len(data)} bytes inflated)")
    print(f"base     : 0x{base:04X}")
    print(f"stride   : 0x{stride:X}  ({stride} bytes)")
    print(f"quality  : item_id + 0x{q_off:X}")
    print()
    print(f"{'slot':>4}  {'item_off':>8}  {'item':>5}  "
          f"{'q_off':>8}  {'flags':>6}  {'quality':<10}  name")
    for i in range(args.slots):
        off = base + i * stride
        if off + 2 > len(data):
            break
        item_id = struct.unpack_from('<H', data, off)[0]
        q_offset = off + q_off
        flags = (struct.unpack_from('<H', data, q_offset)[0]
                 if q_offset + 2 <= len(data) else 0)
        qlbl = quality_label(flags) if item_id else '-'
        # item_id=0 in item_ids.txt is a valid food, but in a gear slot 0
        # almost always means "nothing equipped" — call it empty explicitly.
        name = '(empty)' if item_id == 0 else items.get(item_id, '?')
        print(f"{i:>4}  0x{off:06X}  {item_id:>5}  "
              f"0x{q_offset:06X}  0x{flags:04X}  {qlbl:<10}  {name}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
