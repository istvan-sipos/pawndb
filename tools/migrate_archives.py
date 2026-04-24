#!/usr/bin/env python3
"""Migrate legacy level_<N>/<YYYYMMDDThhmmss>.pawn archives to the new
<NNN>/<HEX>.pawn scheme used by pawndb.c >= commit 5a94c1a.

For each legacy file:
  - level N  <-  digits after "level_" in the folder name
  - HEX      <-  (epoch(filename) - 2026-01-01 UTC)  &  0xFFFFFFFF,  %08X
  - stamp    <-  "NNN:HEX"   (also what archive_rest injects)
  - xfs-poke mArisenName at XFS 0x36F0 and 0x375E with `stamp` + null pad
  - move .pawn/.meta/.json to <NNN>/<HEX>.*
  - remove the legacy folder if empty afterwards

Filename timestamps are local time (archive_rest used GetLocalTime); we parse
them with Python's default local-time interpretation, then convert to UTC
epoch with .timestamp(). That matches how the running mod computes HEX now
(time(NULL) is UTC since 1970).

Usage:
    tools/migrate_archives.py <save_dir>   [--dry-run]

The save dir is typically:
    <DDDA>/steam_settings/load_dlls/pawndb/
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys

EPOCH_2026 = 1767225600              # 2026-01-01 00:00:00 UTC
MARISEN_OFFSETS = (0x36F0, 0x375E)
MARISEN_CAP = 25                     # cName capacity; zero-pad the stamp to this

PAWNBLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        '..', 'pawnblob', 'pawnblob')


def stamp_bytes_hex(stem: str) -> str:
    """Produce the hex string pawnblob xfs-poke expects for a 25-byte
    null-padded mArisenName patch."""
    raw = stem.encode('ascii')
    if len(raw) > MARISEN_CAP - 1:
        raise ValueError(f"stem too long ({len(raw)} > {MARISEN_CAP - 1})")
    raw = raw + b'\0' * (MARISEN_CAP - len(raw))
    return raw.hex()


def parse_level_dir(name: str) -> int | None:
    """Return N for 'level_N', or None for any other directory name.
    Rejects the already-migrated 'NNN' dirs, 'dumps', etc."""
    if not name.startswith('level_'):
        return None
    try:
        n = int(name[6:])
    except ValueError:
        return None
    if n < 1 or n > 200:
        return None
    return n


def hex_from_filename(stem: str) -> str:
    """Convert a legacy 'YYYYMMDDThhmmss' stem to the new 8-hex-digit stem
    (epoch seconds since 2026-01-01 UTC, masked to 32 bits)."""
    t = datetime.datetime.strptime(stem, '%Y%m%dT%H%M%S')
    epoch_local = int(t.timestamp())
    return f"{(epoch_local - EPOCH_2026) & 0xFFFFFFFF:08X}"


def migrate_one(pawn_path: str, level: int, new_dir: str, dry_run: bool) -> bool:
    old_name = os.path.basename(pawn_path)
    stem_old, _ = os.path.splitext(old_name)
    try:
        hex_stem = hex_from_filename(stem_old)
    except ValueError:
        print(f"  SKIP  {pawn_path}: filename not a timestamp")
        return False

    stamp = f"{level:03d}:{hex_stem}"
    new_path = os.path.join(new_dir, f"{hex_stem}.pawn")
    if os.path.exists(new_path):
        print(f"  SKIP  {stem_old} -> {os.path.basename(new_dir)}/{hex_stem}.pawn (target exists)")
        return False

    if dry_run:
        print(f"  plan  {stem_old} -> {os.path.basename(new_dir)}/{hex_stem}.pawn  stamp='{stamp}'")
        return True

    # Two xfs-poke calls, one per offset. pawnblob reads input + writes output,
    # so we stage through a .tmp file between the two offsets.
    patch_hex = stamp_bytes_hex(stamp)
    os.makedirs(new_dir, exist_ok=True)
    tmp_path = new_path + '.tmp'
    try:
        subprocess.run([PAWNBLOB, 'xfs-poke', pawn_path, tmp_path,
                        f"{MARISEN_OFFSETS[0]:X}", patch_hex],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([PAWNBLOB, 'xfs-poke', tmp_path, new_path,
                        f"{MARISEN_OFFSETS[1]:X}", patch_hex],
                       check=True, stdout=subprocess.DEVNULL)
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

    # Move sidecar files (.meta, .json), then remove the original .pawn.
    src_dir = os.path.dirname(pawn_path)
    for ext in ('.meta', '.json'):
        src = os.path.join(src_dir, stem_old + ext)
        dst = os.path.join(new_dir, hex_stem + ext)
        if os.path.exists(src):
            shutil.move(src, dst)
    os.remove(pawn_path)

    print(f"  OK    {stem_old} -> {os.path.basename(new_dir)}/{hex_stem}.pawn  stamp='{stamp}'")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('save_dir', help='e.g. .../steam_settings/load_dlls/pawndb')
    ap.add_argument('--dry-run', action='store_true',
                    help='print planned moves without touching any files')
    args = ap.parse_args()

    if not os.path.isdir(args.save_dir):
        print(f"not a directory: {args.save_dir}", file=sys.stderr)
        return 2
    if not os.access(PAWNBLOB, os.X_OK):
        print(f"pawnblob binary not executable: {PAWNBLOB}", file=sys.stderr)
        return 2

    migrated = 0
    for entry in sorted(os.listdir(args.save_dir)):
        level = parse_level_dir(entry)
        if level is None:
            continue
        legacy_dir = os.path.join(args.save_dir, entry)
        new_dir = os.path.join(args.save_dir, f"{level:03d}")
        print(f"level_{level} -> {level:03d}")

        for f in sorted(os.listdir(legacy_dir)):
            if not f.endswith('.pawn'):
                continue
            pawn_path = os.path.join(legacy_dir, f)
            if migrate_one(pawn_path, level, new_dir, args.dry_run):
                migrated += 1

        if not args.dry_run:
            # Prune the legacy folder if we drained it (leaves .orig-pre-sentinel
            # backups in place, so a dir with leftovers is not a failure).
            try:
                os.rmdir(legacy_dir)
                print(f"  rmdir {entry}")
            except OSError:
                pass

    print(f"\n{'would migrate' if args.dry_run else 'migrated'} {migrated} archive(s)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
