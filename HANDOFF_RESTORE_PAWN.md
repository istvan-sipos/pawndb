# Handoff — `restore_pawn` CLI tool

**Status: discovery complete, implementation in progress (Path B chosen).
Win32 target. Discovery findings folded into [Discovery findings (Path B)](#discovery-findings-path-b)
below — do not re-derive them.**

The current handoff for the now-shipped mStudyData / writeback work lives
in [HANDOFF.md](HANDOFF.md). This file covers a *new, separate* feature.

---

## Goal

Let the player turn an archived pawn (from `pawndb/<NNN>/<HEX>.pawn`) into
their *Arisen's main pawn* in a target `DDDA.sav`. Once it's the main
pawn, the in-game UI exposes edits that are normally locked to the Arisen
(vocation, primary/secondary skills, augments, inclinations), so the
player can adjust the imported pawn freely. Resting at an inn re-archives
the edited pawn through the existing pawndb path.

CLI shape:

```
restore_pawn <archive> <DDDA.sav>
```

`<archive>` is either a `.pawn` file or its `.xml` sidecar (see
[Storage format](#storage-format)). The tool overwrites `<DDDA.sav>`
in-place, after writing `<DDDA.sav>.bak` next to it.

## Two implementation paths

### Path A — translate `.pawn` (XFS blob) to save-XML at restore time

Read the encrypted/deflated XFS pawn blob, walk its fields, and rewrite
the save's main-pawn XML region to match.

- **Pros**: works with every existing archive in the wild
- **Cons**: the XFS blob and the save's `cSAVE_DATA_*` XML overlap but
  are not 1:1. Vocation ranks, skill IDs, augment lists, mInclination
  scoring, mNetUniqueId, equipped weapon attributes, etc. all need
  field-by-field translation. Years of edge cases. We already do the
  reverse direction for gear + study writeback (see `pawndb.c
  writeback_to_archive`); doing it forward, *exhaustively*, is much
  more code.

### Path B — snapshot main-pawn XML at save time, splice it back later

At every `FileWrite('DDDA.sav')` (already hooked for writeback), extract
the Arisen's main-pawn XML region from the inflated save and dump it
verbatim as a sidecar:

```
pawndb/<NNN>/<HEX>.pawn        # existing — XFS blob, summon serves this
pawndb/<NNN>/<HEX>.meta        # existing — leaderboard card
pawndb/<NNN>/<HEX>.json        # existing — human-readable summary
pawndb/<NNN>/<HEX>.xml     # NEW    — verbatim main-pawn XML region
```

`restore_pawn` then:
1. Reads `<HEX>.xml` (or extracts it from the archive sibling).
2. Inflates the target `DDDA.sav`.
3. Locates the Arisen's main-pawn region by element tag.
4. Replaces the bytes with the snapshot.
5. Re-deflates and writes (with `.bak` first).

- **Pros**: no schema translation. The bytes we write are bytes the game
  itself wrote on a prior save, so by definition they're a valid main-
  pawn region. Sidecar is plain text, easy to inspect, diff, edit by
  hand if needed.
- **Cons**: only archives written *after* this feature ships have a
  sidecar. Older archives need to be re-summoned and re-rested-with
  before they're restorable. Storage cost ~30–80 KB / archive.

**Recommendation: Path B.** Start by capturing the snapshot at save time
(small change to the existing `hook_FileWrite` path), then build the CLI
on top. Path A is only worth revisiting if there's strong demand to
restore pre-feature archives.

## Discovery findings (Path B)

These were the questions the original draft flagged as open. Done now —
results below so the next session does not re-do them.

1. **Main-pawn region — what element, where.**
   - The main pawn is the **first `<class type="cSAVE_DATA_CMC">`** inside
     the **first `<array name="mCmc" type="class" count="3">`**. The mCmc
     array holds three pawn slots in order: `[0]` MainPawn, `[1]` Hired1,
     `[2]` Hired2. Mirrors the existing writeback path, which already
     iterates these blocks for hired pawns.
   - Verified on `/home/istvan/remote/DDDA.sav`: the live mCmc array is
     at line 650; mCmc[0] spans bytes [17331, 55613) — about 38 KB.
   - The save also contains a *second* mCmc array at line ~357360. That
     is a checkpoint snapshot (used on death/load-from-checkpoint). See
     "Checkpoint divergence" below.
   - The bare `<class type="cSAVE_DATA_CMC">` opener (no `name=` prefix)
     is unique to the mCmc slots — `<class name="mKaiouData" ...>` and
     `<class name="mKaiouPornData" ...>` use the name-prefixed form, so
     they are naturally excluded from a `<class type=` substring search.

2. **Region boundary detection.**
   - Find first `<array name="mCmc" type="class" count="3">`.
   - From there, find the first `<class type="cSAVE_DATA_CMC">`.
   - Depth-walk forward, treating both `<class name=` and `<class type=`
     as opens, `</class>` as close. Stop at depth 0 — that is the
     matching close tag for mCmc[0]. The exact byte range is
     [open_pos, close_pos + len("</class>")).
   - This is the same depth-walk pattern `pawnsave.c xml_for_each_class`
     already uses for hired-pawn iteration.

3. **Cross-cutting ID references.**
   - The main pawn's `mNetUniqueId.mData` 64-byte block has a unique
     12-byte signature in the high bytes (e.g. `02 00 00 00 9E 15 24 59
     01 00 10 01 ...`).
   - Grep of the full inflated save: that signature appears in **exactly
     two places** — the live mCmc[0] block, and the checkpoint snapshot
     copy at line ~358973. Nothing else (quests, NPC tables, world
     state) references the main pawn by ID.
   - Conclusion: the cSAVE_DATA_CMC block is **self-contained** for
     restore. No external patches needed.

4. **Schema invariants.**
   - mCmc's `count="3"` stays at 3 — we replace one slot, do not add or
     remove. No parent-count update needed.
   - The save header's `realSize` and `compressedSize` (offsets 4 and 8)
     are recomputed from scratch after re-deflate. Same as the existing
     `pawnsave_read_hired` decode path, just in reverse.

5. **Checkpoint divergence (known v1 limitation).**
   - There are two mCmc arrays in the save: live (line 650) and
     checkpoint snapshot (line ~357360). Path B v1 patches only the
     live array.
   - Player-visible effect: if the user triggers a checkpoint reload
     (e.g. death-and-restart from last checkpoint) **before** saving at
     an inn, the pre-restore main pawn comes back. Saving at an inn
     refreshes the checkpoint and the divergence resolves. Document in
     CLI output.
   - Could be addressed later by patching both arrays — same find/splice
     logic, run twice. Skipped in v1 because the inn-save workaround is
     trivial and the hire-then-rest test loop will exercise it anyway.

6. **Appearance is *not* restored — observed and intentional.**
   - In testing: the imported pawn's gear, skills, study flags, vocation,
     augments, and inclinations all came through, but the in-game
     visual model (face, body, voice, name, nickname) stayed the
     player's previous main pawn.
   - Verified that `mCmc[0].mEdit` *is* overwritten by the splice
     (mFaceBase / mFaceEye / mNickname / mVoice all change in the
     spliced bytes). So the live mCmc.mEdit is not the source the
     game reads for the main-pawn appearance render.
   - Two parallel sources survive a restore, either of which would
     explain the persistence:
       (a) The checkpoint `mCmc` array (line ~357360) — its mEdit
           still holds the original values post-splice.
       (b) The `.pawn` (XFS) blob the game writes at every inn rest
           (`remote/0` for the player's own main pawn). Appearance
           data lives there too — that's how the rift renders your
           pawn for other players — and restore_pawn doesn't touch it.
   - **Net effect**: the imported pawn donates its *playable identity*
     (combat / progression) while the player's main pawn keeps its
     *visual identity*. The user reports liking this behavior, so v1
     intentionally leaves it as-is.
   - If a future revision wants full replacement: patch the checkpoint
     mCmc as well, and patch the appearance bytes inside `remote/0` (or
     whichever blob the game's main-pawn loader actually consumes for
     the visual model). Both require additional RE.

## Storage format

`<HEX>.xml` is the raw, inflated XML region — UTF-8, exactly the bytes
that lived between `<class type="cSAVE_DATA_CMC">` and the matching
`</class>` (inclusive of both tags). No wrapping, no compression. The
file is well-formed XML — a single root element — so any XML tool can
parse it. Plain-file ethos matches the rest of the project.

If the region turns out to be multi-megabyte (it shouldn't — main-pawn
data is bounded), revisit and gzip with a `.xml.gz` extension.

## CLI design

```
restore_pawn [--no-backup] [--force] <archive-or-xml> <DDDA.sav>

  <archive-or-xml>  Path to either a .pawn archive (we'll resolve
                        the .xml sibling) or the .xml directly.
  <DDDA.sav>            Target save file. Modified in place after
                        writing <DDDA.sav>.bak.

  --no-backup           Skip the .bak (don't; default is safer).
  --force               Skip the "main pawn currently has gear X, this
                        will replace it" confirmation prompt.

  Exit codes:
    0  success
    1  argument / IO error
    2  target save unparseable (likely already corrupt)
    3  no .xml sibling found for the given .pawn archive
    4  main-pawn region not found in target save (schema mismatch)
```

**Win32 target** (`i686-w64-mingw32-gcc`, matching the rest of the
project). Distributed alongside `pawndb.dll`; the user runs it from the
Wine prefix or from cmd. Native Linux was the original idea but rejected
to keep one toolchain and to make Steam/Proton paths work without
translation.

## Code layout

- `tools/restore_pawn.c` — new tool, ANSI C + zlib
- Reuse the inflate-save scaffolding from `pawnsave.c` if straightforward
  to factor; otherwise duplicate (the tool runs out-of-process and
  doesn't need to share state with the DLL)
- `pawndb.c` change: in `hook_FileWrite('DDDA.sav')`, after parsing the
  inflated save, also extract the main-pawn region and write it to
  `pawndb/<NNN>/<HEX>.xml` for the *current session's* archive (the
  one that was created at the most recent inn rest).

The `.xml` write needs to know which archive stem it belongs to. The
current `archive_rest()` produces `stem_full = "NNN:HEX"` and stamps it
into mArisenName; we can pass the path through to the writeback handler
or look it up via the same parse_arisen_stem mechanism.

## Test plan

1. **Round-trip a known save.** Inflate, locate main pawn, replace its
   region with itself, re-deflate. Game should still load and behave
   identically. (Catches: deflate parameters, byte-for-byte XML
   equality, length prefixes.)
2. **Cross-pawn restore.** Take save A's pawn, restore it into a copy
   of save B, load B in-game, verify the pawn appears with A's
   appearance/skills.
3. **Edit cycle.** Restore → load → change one skill → rest at inn →
   verify a fresh archive appears under the new stem with the changed
   skill.
4. **Backup safety.** `restore_pawn --no-backup` against a deliberately
   bad sidecar — verify save is left untouched if the splice fails.
5. **Older-archive rejection.** Pre-feature archive (no `.xml`) —
   verify the tool exits 3 with a clear error.

## Risks / open questions

- **Save corruption from off-by-one in region detection.** The bak file
  is the safety net; emphasise it in CLI output.
- ~~**Quest / NPC state referencing the old pawn ID.**~~ Resolved by
  discovery #3 above: the main pawn's `mNetUniqueId` signature appears
  only inside the cSAVE_DATA_CMC blocks. No quest / NPC / world-state
  table cross-references it, so no extra patches are needed.
- **Re-archive creates a duplicate.** After restore-and-rest, both the
  old archive (the one that was restored from) and a new archive (the
  freshly-edited version) exist. The CLI could optionally delete the
  source archive on success (`--consume`), but I'd default to leaving
  both — the user can prune manually.
- **Wine permissions on the save file.** The tool is a Win32 PE built
  with `i686-w64-mingw32-gcc`, so it runs in the same Wine prefix as
  DDDA itself and reaches the save via the prefix's normal Windows
  paths (`%APPDATA%\GSE Saves\367500\remote\DDDA.sav`). No Z:\ mapping
  issues either way.
- **gbe_fork update path.** If the save XML schema ever shifts (Capcom
  patches DDDA — unlikely at this point but possible) old `.xml`
  snapshots may no longer splice cleanly. Sidecar versioning could go
  in an XML comment at the top of the file (`<!-- pawnxml v1 -->`) for
  future detection.

## Out of scope

- Editing the `.xml` directly to change skills/etc. without going
  through the game UI. Tempting but a pandora's box of schema
  validation. Stick to "make it the main pawn, edit in-game, re-archive."
- Multiplayer / Steam sharing of restored pawns. The whole project is
  single-player by design.
