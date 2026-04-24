#!/usr/bin/env python3
"""Scan a binary file for one or more u16 LE values; print matching offsets.

Primary use: pin item_id positions in memory dumps or XFS files once you
have the ID from DDsavetool's XML output or from item_ids.txt.

Usage:
    find_u16.py <file> <value> [<value>...] [--align N]

Values can be decimal (426) or hex (0x1AA). Output is one line per hit:
    0x<offset>  u16=<value>  <name?>
Names are resolved from item_ids.txt (repo root) when present.
"""

import argparse
import os
import sys


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


def parse_int(s: str) -> int:
    s = s.strip().lower()
    return int(s, 16) if s.startswith('0x') else int(s)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument('file')
    ap.add_argument('values', nargs='+')
    ap.add_argument('--align', type=int, default=1,
                    help='byte alignment of candidate offsets (default 1; '
                         'use 2 for u16-aligned only)')
    args = ap.parse_args()

    data = open(args.file, 'rb').read()
    items = load_items()
    targets = [parse_int(v) for v in args.values]

    grand_total = 0
    for v in targets:
        name = items.get(v, '')
        print(f"=== u16 {v} (0x{v:04X})"
              + (f"  {name}" if name else '')
              + " ===")
        lo, hi = v & 0xff, (v >> 8) & 0xff
        hits = []
        for i in range(0, len(data) - 1, args.align):
            if data[i] == lo and data[i + 1] == hi:
                hits.append(i)
        for off in hits:
            print(f"  0x{off:05X}")
        print(f"  ({len(hits)} hit(s))")
        grand_total += len(hits)

    print(f"total: {grand_total} hit(s) across {len(targets)} value(s)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
