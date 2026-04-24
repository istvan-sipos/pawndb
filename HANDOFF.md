# Pawn Database Project — Handoff Summary

**Status: gear and study writeback both shipped.** A hired pawn's gear and
its full knowledge state (monsters, vocations, areas, quests) now persist
back into the source `.pawn` archive at every save, so re-hires see the
upgrades.

This document is written so a fresh session can pick up extension or
debugging work without reading the full git history. The most non-obvious
parts of the implementation are documented in the **Study writeback** and
**Open questions** sections — read those if anything in the writeback path
behaves unexpectedly.

---

## What this project is

A mod for *Dragon's Dogma: Dark Arisen* (DDDA, Steam) that persists hired
pawns locally. The rift serves an attached MITM catalogue of past hires,
and release-time changes (gear and soon study data) are written back to
the source `.pawn` archive so re-hires see the upgrades.

The mod loads as a DLL via [gbe_fork](https://github.com/Detanup01/gbe_fork)
(a Goldberg Emulator fork) which runs under Wine. gbe_fork replaces
Steam's `steam_api.dll`; it auto-loads any DLL placed in
`steam_settings/load_dlls/` at init.

---

## Repo layout

```
pawndb.c             main DLL source (Windows target; i686-w64-mingw32)
pawnxfs.c/.h         XFS (.pawn) patcher: Blowfish + zlib + SHA-1 + encrypt
pawnsave.c/.h        DDDA.sav reader: zlib + XML primitives + hired-pawn extract
pawnblob/            standalone C++ tool (xfs-head, unpack, xfs-poke)
third_party/easyzlib single-file zlib, vendored
tools/               Python helpers (RE / migration / prototypes)
ddda-dinput8-master/ reference: another DDDA modding project's source
ddsavetool/          reference: ddsavetool save decoder
gbe_fork-dev/        reference: gbe_fork source
Makefile             builds pawndb.dll and deploys to the Steam dir
```

Build + deploy in one step:
```
make           # -> pawndb.dll, md5'd and copied into the load_dlls path
```

The deploy path is hardcoded in the Makefile:
`~/.steam/steam/steamapps/common/DDDA/steam_settings/load_dlls/pawndb.dll`.
Note: the user's actual Steam tree is
`~/.local/share/Steam/steamapps/common/DDDA/...` — the Makefile's path is
a symlink-friendly alias that resolves to the same files.

The runtime archive store lives at
`<load_dlls>/pawndb/<NNN>/<HEX>.{pawn,meta,json}` where `NNN` is the
pawn's level (1..200) and `HEX` is an 8-char hex stem (epoch offset from
2026-01-01 UTC). `save_dir` in `pawndb.ini` selects the root (`pawndb`).

---

## How the mod works (minimal map)

### Steam API interception
pawndb.c patches ISteamRemoteStorage vtable slots to observe:
- `FileWrite(name, data, size)` slot 0 — the game writes `DDDA.sav` here (~512 KB)
- `FileShare(name)` slot 4 — UGC upload lifecycle
- `UGCDownload` slot 21 / `UGCRead` slot 24 — rift serving

Real UGC downloads are replaced with local archive data at UGCRead time.

### Inn rest → archive
At inn rest the game writes a fresh snapshot of the main pawn to
`remote/0`. `archive_rest` copies that blob to
`pawndb/<NNN>/<stem>.pawn`, sibling `.meta` and `.json`, then uses
`pawnxfs_poke` to stamp the archive's `mArisenName` cName at XFS offsets
`0x36F0` and `0x375E` with `"NNN:HEX"` (capacity 25). The archive now
self-identifies.

### Rift serving
`hook_UGCRead` serves local `.pawn` bytes when the game asks for an
advertised fake UGC handle. The stem appears on the rift card as that
pawn's Arisen name.

### Hire → save → writeback
When a hired pawn is in memory, the save serializes its `mArisenName`
(still `"NNN:HEX"`) into the corresponding `cSAVE_DATA_CMC` block's
`<class name="mArisenName" type="cName">`. At `FileWrite('DDDA.sav')`:

1. `hook_FileWrite` lets the game's save hit disk first.
2. Then calls `pawnsave_read_hired(pvData, cubData, info[2])`:
   - Decompress the 32-byte-header zlib payload (~20 MB XML).
   - Depth-walk `<class type="cSAVE_DATA_CMC">` top-level blocks.
   - For each, extract `mArisenName` + the 12 `mEquipItem` records +
     the two 322-entry `mStudyFlag` / `mLocalStudyFlag` u32 arrays.
   - Classify by first-non-empty gear record's `mOwnerId` (2 or 3 → Hired1/Hired2).
   - First block wins per slot, so the early current-state copies at
     lines 651/2376/4101 take precedence over checkpoint copies at
     line 297720+.
3. For each filled slot, `writeback_to_archive`:
   - Parses `creator_name` via `parse_arisen_stem` → `(level, stem)`.
   - Builds `pawndb\<NNN>\<stem>.pawn`, verifies it exists.
   - Emits 26 patches in one `pawnxfs_poke` call:
     - 12 × {u16 mItemNo @ +0x02, u32 mFlag @ +0x08} for gear
     - 1288 bytes mStudyFlag      @ XFS 0x282C
     - 1288 bytes mLocalStudyFlag @ XFS 0x2D38
   - Gear: transforms `xfs_flag = save_flag & ~0x80` (bit 7 is runtime-only).
   - Study: copied verbatim — save XML and XFS bytes are bit-for-bit
     identical, so no encoding/transform on either side.

Important: the save is the single source of truth for writeback. No
memory binding is needed — works for freshly-hired and load-restored
pawns identically.

### Netuid scrubber (orthogonal)
The fake SteamIDs we inject for hired pawns need continuous zeroing from
memory to avoid leakage. `netuid_scrub_wide` walks a 1 MB window around
`*pBase` every tick (~60 Hz), zeroing any `MtNetUniqueId.mData` whose
byte[7] MSB is set. No longer involved in binding — it's purely a
privacy scrub.

---

## XFS (.pawn archive) format

Inflated size: **20480 bytes** (known-fixed).

Structure:
- `0x0000..0x1DE4`: schema (strings + class/field descriptors)
- `0x1DE4+`: instance region, **struct_size = 0x1578**
- Two `0x142`-tagged 1292-byte (`0x50C`) structs at `0x2828` and `0x2D34`
  hold `mStudyFlag[322]` and `mLocalStudyFlag[322]` respectively. The
  first 4 bytes of each is the class tag (`42 01 00 00` = `0x142`); array
  data starts at struct + 4. **Patching the tag bytes themselves crashes
  the game on summon — only write the array contents (offsets 0x282C
  and 0x2D38).**

Disk form: Blowfish-encrypted body + SHA-1 footer + plain header + `0xDD`
padding tail. `pawnxfs_poke` handles the full pipeline. `pawnblob` is the
standalone tool (C++) with subcommands `xfs-head`, `unpack`, `xfs-poke`.

### Known offsets

| Offset | Type | Field | Confirmed by |
|--------|------|-------|--------------|
| `0x2098` | record base | `mEquipItem[0]` | `tools/save_to_archive.py` swap tests |
| `0x2098 + k*0x46` | record | `mEquipItem[k]`, k=0..11 | same |
| `+0x02` | s16 | `mItemNo` | same |
| `+0x08` | u32 | `mFlag` (xfs form — `save_flag & ~0x80`) | same |
| `0x2828` | u32 | class tag `0x142` (start of mStudyFlag struct) | schema decode + size match |
| `0x282C` | u32[322] | `mStudyFlag` (1288 bytes) | save↔XFS round-trip verified |
| `0x2D34` | u32 | class tag `0x142` (start of mLocalStudyFlag struct) | same |
| `0x2D38` | u32[322] | `mLocalStudyFlag` (1288 bytes) | save↔XFS round-trip verified |
| `0x36F0` | u8[25] | `mArisenName` cName bytes (copy 1) | archive_rest stamping verified |
| `0x375E` | u8[25] | `mArisenName` cName bytes (copy 2) | same |

---

## DDDA.sav format

`/home/istvan/remote/DDDA.sav` (524288 bytes).

Binary header (32 bytes):
```
u32 version (21)
u32 realSize            # decompressed payload (~20 MB)
u32 compressedSize
u32 magic1 (860693325)
u32 zero
u32 magic2 (860700740)
u32 crc32jam hash
u32 magic3 (1079398965)
```

Body: zlib-deflated XML.

### Layout summary

Early current-state blocks (the ones we use for writeback):

| Line | Block | Owner |
|------|-------|-------|
| 651 | `<class type="cSAVE_DATA_CMC">` | 1 (Main Pawn) |
| 2376 | `<class type="cSAVE_DATA_CMC">` | 2 (Hired1) |
| 4101 | `<class type="cSAVE_DATA_CMC">` | 3 (Hired2) |

Late blocks at line `297720+` appear to be checkpoint snapshots; our
iterator takes the first match per owner, so they're ignored by writeback.

`mArisenName` is a `<class name="mArisenName" type="cName">` nested
inside each `cSAVE_DATA_CMC`, containing a
`<array name="( u8* )mEditName" type="u8" count="25">` of `<u8 value="N"/>`
ASCII-code elements, NUL-padded to 25 bytes.

### Knowledge fields — what's serialised

Each `cSAVE_DATA_CMC` block contains FIVE study-related fields:

```xml
<array name="mStudyFlag"      type="u32" count="322"> ... </array>
<array name="mLocalStudyFlag" type="u32" count="322"> ... </array>
<array name="mStudyData.EncountFrame" type="f32" count="72"> ... </array>
<array name="mStudyData.KillCnt"      type="u32" count="72"> ... </array>
<array name="mStudyData.UniqueCnt"    type="u8"  count="116"> ... </array>
```

Important separation:

- **`mStudyFlag` / `mLocalStudyFlag`** are the **persisted** representation
  — they round-trip into the `.pawn` archive and are what the next hirer
  sees. These are what writeback patches.
- **`mStudyData.*`** are the live counters (per-monster encounter frames,
  kill counts, unique-part counts) used at runtime. They are **not**
  serialised into the `.pawn` archive — pawn exports zero them out.
  Only the save XML carries them.

So if you want a hired pawn's knowledge to survive into the next hire,
write `mStudyFlag` and `mLocalStudyFlag`. The detailed counters reset on
hire and accumulate fresh.

---

## Tools

All under `tools/`:

| Tool | Purpose |
|------|---------|
| `find_u16.py` | Scan a binary for u16 LE values; resolve names from `item_ids.txt`. Used to pin XFS gear offsets. |
| `inspect_gear_xfs.py` | Table of u16s at candidate gear offsets in `.pawn`/`.xfs` with quality decoded via the `0x678` mask. |
| `inspect_gear_dump.py` | Same, for memory dumps. |
| `save_to_archive.py` | Reference Python prototype of the gear writeback: decompress DDDA.sav, find mEquipItem by mOwnerId, apply `xfs_flag = save_flag & ~0x80`, drive pawnblob xfs-poke. Still useful as an oracle when debugging the C version. |
| `study_probe.py` | Cross-references save XML mStudyData arrays against XFS bytes; originally written to find the (non-existent) raw-array offsets, now mostly useful as scaffolding for any future RE on the per-entry encoding inside mStudyFlag. |
| `migrate_archives.py` | One-shot migration of legacy `level_<N>/<ts>.pawn` to the current `<NNN>/<HEX>.pawn` scheme with stem injection. |

Standalone binary: `pawnblob/pawnblob` — `xfs-head <pawn>`, `unpack <pawn> <out.xfs>`, `xfs-poke <in.pawn> <out.pawn> <hex_offset> <hex_bytes>`.

To round-trip a save quickly:
```python
import zlib, struct
with open('/home/istvan/remote/DDDA.sav', 'rb') as f:
    hdr = f.read(32); rest = f.read()
_, real, comp, *_ = struct.unpack('<8I', hdr)
xml = zlib.decompress(rest[:comp])
open('/tmp/ddda_latest.xml', 'wb').write(xml)
```

To decrypt and inspect a `.pawn` archive:
```
./pawnblob/pawnblob unpack <archive.pawn> /tmp/out.xfs
xxd /tmp/out.xfs | less
```

---

## Key APIs (what's already wired)

### pawnxfs.h
```c
struct pawnxfs_patch { uint32_t offset; const uint8_t *bytes; uint32_t len; };
int pawnxfs_poke(const char *in_path, const char *out_path,
                 const struct pawnxfs_patch *patches, int count,
                 char *err, size_t err_cap);
```
One call applies any number of patches, any mix of sizes. Backing bytes
must outlive the call. Output path may equal input path (in-place).

### pawnsave.h
```c
struct sav_gear_record { ... };
struct pawnsave_hired_info {
    int      present;
    char     creator_name[32];
    struct   sav_gear_record gear[12];
    uint32_t study_flag[322];        /* mStudyFlag      */
    uint32_t local_study_flag[322];  /* mLocalStudyFlag */
};
int pawnsave_read_hired(const void *save_bytes, int32_t save_len,
                        struct pawnsave_hired_info out[2]);
```

### XML primitives in pawnsave.c
- `xfind` — substring search in an xspan.
- `xml_get_long` — find `name="X" value="N"` and parse N.
- `xml_get_u32_array(in, NAME, out, count)` — iterate `<u32 value="N"/>`
  children of a named array into a u32 buffer. Used for the two study
  arrays. Absent-array → returns 0 with the buffer untouched (caller
  is expected to zero-init).
- `xml_for_each_array(in, "NAME", cb, ctx)` — iterate `<array name="NAME"...>body</array>`.
- `xml_for_each_class(in, "TYPE", cb, ctx)` — iterate bare `<class type="TYPE">body</class>` with depth tracking.
- `xml_get_cname(in, "NAME", out, cap)` — decode a `cName` u8 array into ASCII.

---

## Study writeback — what was built and why

### Result
Two patches per hired pawn at `FileWrite('DDDA.sav')`:
- `mStudyFlag`      → XFS `0x282C`, 1288 bytes, copied verbatim.
- `mLocalStudyFlag` → XFS `0x2D38`, 1288 bytes, copied verbatim.

The 322-entry u32 arrays in the save XML and the bytes inside the two
`0x142`-tagged XFS structs are bit-for-bit identical. No transform, no
encoding logic — `(uint8_t *)info->study_flag` points straight at the
patch source. See `WB_STUDY_FLAG_OFF` / `WB_LOCAL_STUDY_FLAG_OFF` in
`pawndb.c`.

### Why not the mStudyData arrays
The original plan was to write back `mStudyData.{EncountFrame, KillCnt,
UniqueCnt}` from save to XFS. Reverse-engineering established that those
**are not serialised into the .pawn archive** — the game zeroes them on
export. A controlled A/B diff (same main pawn before vs after killing 7
wolves) showed only ~36 bytes of XFS delta, all accounted for by
vocation XP, inclinations, and the mArisenName rename. The save's
`KillCnt[3]=8` byte pattern (`08 00 00 00`) appears nowhere in the XFS.

What does survive into the archive is the *summary*: `mStudyFlag` and
`mLocalStudyFlag`. Those are the fields the game derives from the live
counters and stamps into shared pawn data. Patching those gives the next
hirer the pawn's accumulated knowledge — gear-style, but cheaper.

### What's in the 322 entries
Each u32 entry is a flag-bitmask for one trackable item. The 322 slots
are partitioned into knowledge categories — verified by inspecting which
indices change in response to which gameplay events:

| Index range | Category (inferred) |
|-------------|---------------------|
| 0..9, 30..33, 59 | Vocation / skill bitmasks (high-bit `0x80000xxx` pattern) |
| 130..149 | Monster knowledge — wolf is at index 131 (matches save's KillCnt[3] semantically; different indexing) |
| 258..301 | Areas and/or quests — index 258 ticked when killing wolves in their habitat |

The exact bit semantics inside each u32 are **partially decoded**:
- Bit 0 alone (`0x00000001`) is enough to make a monster appear in the
  bestiary (validated by patching `0xFFFFFFFF` at empty indices — got
  "unfamiliar foe, 1 star" entries).
- Adding bits from a known 2-star pattern (e.g. OR-ing `0x001D8067` into
  the wolf entry) bumps it to 2 stars while preserving its "named"
  status. Validated in-game.
- Beyond that — which bits = which star, which bit = "named" — wasn't
  pinned down. We didn't need to: verbatim copy preserves whatever the
  game itself stored.

### Verified safety
- **Don't touch the `0x142` class tag** at `0x2828` or `0x2D34` (the 4
  bytes before each array). Overwriting the tag crashed on summon in
  earlier blind-poke experiments. Writing only the array bytes (offsets
  `0x282C` / `0x2D38`) is safe.
- The bestiary is **cumulative across hires** — once your save's
  bestiary records monster X via any hired pawn, it persists even if
  you re-hire a different pawn. Useful to know when testing: a "broken"
  patch can't visibly degrade an existing entry, only fail to add
  expected new ones.

---

## Open questions / things to revisit if behaviour seems off

These are the bits of the picture that aren't fully decoded. The current
writeback works without resolving them (because verbatim copy doesn't
need them resolved), but if the in-game result ever looks wrong, start
here.

1. **Per-bit semantics inside `mStudyFlag` u32s.** Bit 0 = "encountered"
   is confirmed. The bit(s) that gate "named" status (so a monster shows
   as e.g. "Wolf" instead of "Unfamiliar Foe") and the bit(s) that gate
   star count are unknown beyond "more bits ⇒ more stars, OR-additive".
   Test loop: patch one entry to `0x00000001`, `0x00000003`, `0x07`,
   `0x001D8067`, etc., re-hire, observe star count. The cumulative-cache
   behaviour means you should test on a *fresh* monster index that
   neither this pawn nor any prior hire has touched, otherwise old
   entries persist and obscure the result.

2. **Index ↔ in-game thing mapping.** Idx 131 is wolf in mStudyFlag, but
   wolf is idx 3 in mStudyData.KillCnt — two different index spaces.
   The 322-table indexing (which slot is which monster / area / quest)
   isn't documented anywhere we have access to. Could be RE'd by writing
   non-zero values at single indices and checking the in-game UI (which
   knowledge tab the entry shows up in), but it's tedious.

3. **`mLocalStudyFlag` vs `mStudyFlag` semantic difference.** Both are
   patched and both are needed (each contributes its own bestiary
   entries — patching only one halves the result). What the "Local"
   prefix means in the game's own code is a guess: maybe "current
   instance" vs "lifetime", maybe "this hire" vs "ever-known", maybe
   "client view" vs "shared view". Doesn't affect writeback correctness
   since we copy both verbatim.

4. **Why 322.** Doesn't divide cleanly into known DDDA counts (72
   monsters, ~120 quests, ~80 areas). Likely a generously-sized
   single-table sized for combined categories with unused padding slots.

5. **Later cSAVE_DATA_CMC blocks at line ~297720.** These are checkpoint
   snapshots (auto-save state etc.). Our iterator takes the first
   matching block per owner, so they're ignored. If save state ever
   behaves oddly — e.g. stale gear writeback after a load — verify the
   iteration order still picks current-state blocks first.

---

## Session restart checklist

Before starting work:
1. `git log --oneline -10` to see recent commits.
2. `cat HANDOFF.md` (this file).
3. `ls /home/istvan/remote/DDDA.sav` — most recent in-game save. Decompress
   it via the Python snippet above to get `/tmp/ddda_latest.xml`.
4. `ls /home/istvan/.local/share/Steam/steamapps/common/DDDA/steam_settings/load_dlls/pawndb/`
   — current archive store.
5. `md5sum pawndb.dll /home/istvan/.steam/steam/steamapps/common/DDDA/steam_settings/load_dlls/pawndb.dll`
   — confirm deployed hash matches source build.

Build cycle: `make` (builds and deploys). Close the game first; Wine
keeps the DLL mapped.

Log file: `<load_dlls>/pawn_database/pawndb.log` or wherever `save_dir`
points. Truncated each session by default (`logging = truncate` in
`pawndb.ini`).

---

## Principles that shaped the current design

- **Save is the source of truth for writeback.** No memory binding, no
  hire-dump machinery, no state carried between sessions. Whatever the
  save says mArisenName is, that's the archive we patch. Same pattern
  applies to study data — copy what the save has, the game already
  computed the right values.
- **One XML pass per FileWrite.** Decompress once, iterate top-level
  `cSAVE_DATA_CMC` blocks, extract everything needed per slot in one
  callback (gear records + both 322-entry knowledge arrays).
- **Pipeline validation before RE.** Every XFS region was verified with
  a no-op poke (byte-identical output) before any real write.
- **Python is the oracle.** `tools/save_to_archive.py` was built first
  as a reference for gear; the C implementation mirrored its logic.
  `tools/study_probe.py` did the same for study, but in the end the
  RE walked through `pawnblob xfs-poke` directly during the in-game
  test cycle rather than through a stand-alone Python encoder.
- **Trust verbatim copy when it's available.** The mStudyData→XFS
  question burned a lot of effort assuming we'd need a transform
  encoder, when in fact mStudyFlag/mLocalStudyFlag round-trip
  unchanged. The lesson: before designing an encoder, check whether
  the save and XFS bytes are already identical for the field in
  question — they often are.
