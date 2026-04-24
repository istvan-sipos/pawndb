#!/usr/bin/env python3
"""Prototype: locate mStudyData XFS offsets by cross-referencing a hired
pawn's save XML against its source .pawn archive.

Assumes the hired pawn was just loaded from the archive (no kills /
encounters since hire), so the save XML values equal the XFS values
verbatim. If that assumption holds, the three arrays' little-endian byte
representations should each appear exactly once in the 20480-byte
inflated XFS — pinning the offsets in one shot.

Usage:
    study_probe.py <DDDA.sav> <source.pawn> [--slot 0|1]
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

# Known-fragile XFS regions: blind pokes here have crashed on summon.
DANGER_ZONES = [(0x2828, 0x50C), (0x2D34, 0x50C)]


def decompress_save(path: str) -> bytes:
    with open(path, 'rb') as f:
        hdr = f.read(32)
        rest = f.read()
    _, _real, comp_size, *_ = struct.unpack('<8I', hdr)
    return zlib.decompress(rest[:comp_size])


def iter_cmc_blocks(xml: str):
    """Yield (start, end) substring indices of each top-level
    `<class type="cSAVE_DATA_CMC">...</class>` body. Depth-tracks so
    nested `<class name=...>` sub-blocks don't close early."""
    opener = '<class type="cSAVE_DATA_CMC">'
    closer = '</class>'
    i = 0
    n = len(xml)
    while True:
        start = xml.find(opener, i)
        if start == -1:
            return
        body_start = start + len(opener)
        depth = 1
        p = body_start
        close_at = -1
        while p < n:
            lt = xml.find('<', p)
            if lt == -1:
                return
            if xml.startswith(closer, lt):
                depth -= 1
                if depth == 0:
                    close_at = lt
                    break
                p = lt + len(closer)
            elif xml.startswith('<class name=', lt) or xml.startswith('<class type=', lt):
                depth += 1
                p = lt + 12
            else:
                p = lt + 1
        if close_at < 0:
            return
        yield body_start, close_at
        i = close_at + len(closer)


_ITEM_REC_RE = re.compile(
    r'<s16 name="data\.mItemNo" value="(-?\d+)"/>\s*'
    r'<u32 name="data\.mFlag" value="\d+"/>\s*'
    r'<u16 name="data\.mChgNum" value="\d+"/>\s*'
    r'<u16 name="data\.mDay1" value="\d+"/>\s*'
    r'<u16 name="data\.mDay2" value="\d+"/>\s*'
    r'<u16 name="data\.mDay3" value="\d+"/>\s*'
    r'<s8 name="data\.mMutationPool" value="-?\d+"/>\s*'
    r'<s8 name="data\.mOwnerId" value="(-?\d+)"/>',
    re.DOTALL)


def first_nonempty_owner(cmc_body: str) -> int | None:
    """Ownership is carried by the first non-empty mEquipItem record's
    mOwnerId. Empty slots keep mOwnerId=0, so scanning in order and
    picking the first mItemNo != -1 is reliable."""
    arr = re.search(r'<array name="mEquipItem"[^>]*>(.*?)</array>',
                    cmc_body, re.DOTALL)
    if not arr:
        return None
    for rec in _ITEM_REC_RE.finditer(arr.group(1)):
        item_no, owner = int(rec.group(1)), int(rec.group(2))
        if item_no != -1:
            return owner
    return None


def extract_array(cmc_body: str, name: str, tag: str, count: int) -> list:
    """Pull values from `<array name=NAME type=TAG count=COUNT>...</array>`
    as floats (f32) or ints (uXX)."""
    m = re.search(
        rf'<array name="{re.escape(name)}" type="{tag}" count="{count}">(.*?)</array>',
        cmc_body, re.DOTALL)
    if not m:
        raise RuntimeError(f"array {name!r} not found in this CMC block")
    conv = float if tag.startswith('f') else int
    vals = [conv(v) for v in re.findall(rf'<{tag} value="([^"]+)"/>', m.group(1))]
    if len(vals) != count:
        raise RuntimeError(f"{name}: expected {count} entries, got {len(vals)}")
    return vals


def pack_le(values, fmt: str) -> bytes:
    return b''.join(struct.pack('<' + fmt, v) for v in values)


def unpack_pawn(path: str) -> bytes:
    with tempfile.NamedTemporaryFile(suffix='.xfs', delete=False) as f:
        tmp = f.name
    try:
        subprocess.run([PAWNBLOB, 'unpack', path, tmp],
                       check=True, stdout=subprocess.DEVNULL)
        with open(tmp, 'rb') as f:
            return f.read()
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def find_all(haystack: bytes, needle: bytes) -> list:
    out = []
    i = 0
    while True:
        j = haystack.find(needle, i)
        if j == -1:
            return out
        out.append(j)
        i = j + 1


def flag_danger(off: int, size: int) -> str:
    for base, span in DANGER_ZONES:
        if off < base + span and off + size > base:
            return f"  *** overlaps 0x142-tagged struct at 0x{base:04X} (unsafe poke region) ***"
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('save', help='DDDA.sav (compressed save file)')
    ap.add_argument('archive', help='source .pawn archive the hired pawn came from')
    ap.add_argument('--slot', type=int, choices=[0, 1], default=0,
                    help='hired slot (0=Hired1, 1=Hired2). Default 0.')
    args = ap.parse_args()

    owner = 2 + args.slot
    print(f"save     : {args.save}")
    print(f"archive  : {args.archive}")
    print(f"slot     : mod slot {args.slot} -> mOwnerId {owner}")

    xml = decompress_save(args.save).decode('utf-8', errors='replace')
    print(f"xml      : {len(xml)} chars")

    target = None
    for body_start, body_end in iter_cmc_blocks(xml):
        body = xml[body_start:body_end]
        if first_nonempty_owner(body) == owner:
            target = body
            break
    if target is None:
        print(f"!! no cSAVE_DATA_CMC block found with mOwnerId={owner}")
        return 1

    enc = extract_array(target, 'mStudyData.EncountFrame', 'f32', 72)
    kil = extract_array(target, 'mStudyData.KillCnt',       'u32', 72)
    uni = extract_array(target, 'mStudyData.UniqueCnt',     'u8',  116)

    nz_enc = sum(1 for v in enc if v != 0.0)
    nz_kil = sum(1 for v in kil if v != 0)
    nz_uni = sum(1 for v in uni if v != 0)
    print(f"\nextracted from save (hired slot {args.slot}):")
    print(f"  EncountFrame : {nz_enc}/72 non-zero")
    print(f"  KillCnt      : {nz_kil}/72 non-zero")
    print(f"  UniqueCnt    : {nz_uni}/116 non-zero")
    if nz_enc + nz_kil + nz_uni == 0:
        print("\n!! all three arrays are zero in the save.")
        print("   either the hired pawn's source archive had no study data,")
        print("   or the game zeroes study on hire. try a richer source pawn;")
        print("   if still all-zero, fall back to A/B play-between-rests RE.")
        return 2

    enc_bytes = pack_le(enc, 'f')
    kil_bytes = pack_le(kil, 'I')
    uni_bytes = bytes(uni)

    xfs = unpack_pawn(args.archive)
    print(f"\nxfs      : {len(xfs)} bytes")

    print("\nfull-array signature search (inflated XFS):")
    results = {}
    for name, sig in [('EncountFrame', enc_bytes),
                      ('KillCnt',      kil_bytes),
                      ('UniqueCnt',    uni_bytes)]:
        hits = find_all(xfs, sig)
        results[name] = hits
        if len(hits) == 1:
            off = hits[0]
            print(f"  {name:12s}: 1 hit at 0x{off:04X} (size 0x{len(sig):X}){flag_danger(off, len(sig))}")
        elif not hits:
            print(f"  {name:12s}: 0 hits — values may not round-trip verbatim")
        else:
            locs = ', '.join(f'0x{h:04X}' for h in hits[:8])
            print(f"  {name:12s}: {len(hits)} hits at [{locs}{'...' if len(hits) > 8 else ''}]")

    # If KillCnt full-array didn't match, try finding the base via
    # per-element hits at consistent (base + i*4) strides. Same for the
    # other arrays if they also miss.
    def fallback(name, values, elem_size, fmt):
        print(f"\nfallback per-element scan for {name}:")
        hits_by_base = {}
        for i, v in enumerate(values):
            if v == 0 or (fmt == 'f' and v == 0.0):
                continue
            b = struct.pack('<' + fmt, v)
            for off in find_all(xfs, b):
                cand = off - i * elem_size
                if cand < 0 or cand + len(values) * elem_size > len(xfs):
                    continue
                hits_by_base.setdefault(cand, []).append(i)
        ranked = sorted(hits_by_base.items(), key=lambda kv: -len(kv[1]))
        if not ranked:
            print(f"  no candidate bases")
            return
        for base, idxs in ranked[:5]:
            size = len(values) * elem_size
            print(f"  base=0x{base:04X}  matches {len(idxs)}/{len(values)} entries"
                  f"{flag_danger(base, size)}")

    if len(results['EncountFrame']) != 1:
        fallback('EncountFrame', enc, 4, 'f')
    if len(results['KillCnt']) != 1:
        fallback('KillCnt', kil, 4, 'I')
    if len(results['UniqueCnt']) != 1:
        fallback('UniqueCnt', uni, 1, 'B')

    # If all three pinned cleanly, also report the inter-array layout —
    # strongest confirmation we've found the right region.
    if all(len(results[k]) == 1 for k in ('EncountFrame', 'KillCnt', 'UniqueCnt')):
        e, k, u = results['EncountFrame'][0], results['KillCnt'][0], results['UniqueCnt'][0]
        print(f"\nlayout:")
        print(f"  EncountFrame @ 0x{e:04X}  (288 bytes)")
        print(f"  KillCnt      @ 0x{k:04X}  (288 bytes)   [EncountFrame+0x{k-e:X}]")
        print(f"  UniqueCnt    @ 0x{u:04X}  (116 bytes)   [KillCnt+0x{u-k:X}]")

    return 0


if __name__ == '__main__':
    sys.exit(main())
