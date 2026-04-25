# pawndb

Local pawn-rift emulator for **Dragon's Dogma: Dark Arisen** (Steam, on Linux
via Proton, probably works on windows too). This shim keeps the in-game rift
functional against a folder of archived pawn snapshots on disk.

## What it does

DDDA uploads the player's main pawn to Steam leaderboards at every inn rest:
a 2-detail entry on `leader_231`/`leader_232` holds the UGC handle for the
pawn blob, and `leader_<N>` (N = pawn level) is where other players' pawns
appear when you search by level. If Capcom's backend dies, `leader_<N>` is
empty and the rift shows zero online pawns.

This shim runs as a 32-bit Windows DLL loaded by
[gbe_fork](https://github.com/Detanup01/gbe_fork) (a Steam API emulator) and:

1. **Archives** the player's own main pawn into `pawndb/<NNN>/<HEX>.{pawn,meta,json,xml}`
   on every inn rest (`<NNN>` = pawn's live level zero-padded to 3 digits,
   `<HEX>` = 8-char hex epoch-offset from 2026-01-01 UTC). The archive's
   `mArisenName` cName is also stamped with `"NNN:HEX"` so the blob self-
   identifies on a later hire.
2. **Populates** `leader_231`/`leader_232` and `leader_<N>` with synthetic
   entries for every archived pawn so the rift displays them.
3. **Intercepts** the summon flow (`UGCDownload` / `UGCRead`) so clicking a
   fake pawn in the rift serves its archived blob back to the game.
4. **Writes back** equipment and knowledge changes on save. When the
   game writes `DDDA.sav`, the hook decompresses it, reads each hired
   pawn's gear records and the two 322-entry `mStudyFlag` /
   `mLocalStudyFlag` arrays, and patches the corresponding source
   `.pawn` archive so re-hires see the upgrades — both gear and
   accumulated monster / area / quest knowledge.

Archives from previous sessions are fully browsable and summonable in the
rift. Archives written *in the current session* are intentionally hidden
from the rift until the next game launch (the game caches the leaderboard
results).

Sidecars next to each `.pawn`:

- `.json` — human-readable summary (level, vocation, stats, vocation
  ranks, inclinations, equipped skills with resolved names) for browsing
  the archive folder from the shell.
- `.xml` — verbatim copy of the pawn's `<class type="cSAVE_DATA_CMC">…
  </class>` region from the save XML at the time of the most recent
  archive write. Consumed by `tools/restore_pawn.exe` to splice the pawn
  into a target save's main-pawn slot. See [RESTORE_PAWN.md](RESTORE_PAWN.md).

## Dependencies

Build-time:
- `i686-w64-mingw32-gcc` (mingw-w64, 32-bit Windows cross-compiler)
  - Debian/Ubuntu: `apt install gcc-mingw-w64-i686`
- GNU `make`

Runtime:
- A DDDA install that launches under Proton (32-bit, Steam app id 367500).
- [gbe_fork](https://github.com/Detanup01/gbe_fork) experimental x32 build
  replacing DDDA's `steam_api.dll`. See gbe_fork docs for setup; TL;DR:
  - Drop its experimental/x32 `steam_api.dll` on top of DDDA's original.
  - Create `steam_settings/` next to `DDDA.exe` with `steam_appid.txt = 367500`.
- Optional: [ddda-dinput8](https://github.com/nrbrt/ddda-dinput8) for
  in-game stat inspection (not required; pawndb reads the same memory
  offsets on its own).

## Build

```sh
make            # produces pawndb.dll
```

## Install

For a **binary release** (no build toolchain), follow [INSTALL.md](INSTALL.md).

From source:

```sh
make install
```

This copies `pawndb.dll` and (if absent) `pawndb.ini` into
`$DDDA_DIR/steam_settings/load_dlls/`, where gbe_fork auto-loads any DLL
dropped in. Edit the `DDDA_DIR` variable at the top of the Makefile if your
install path differs.

The `pawndb.ini` copy is idempotent — re-running `make install` does NOT
overwrite your tuned settings. To force-refresh the INI, delete the installed
copy first.

## Configuration (pawndb.ini)

```ini
save_dir           = pawndb   # root of archives, relative to DLL folder
max_search_results = 100             # per-level rift cap; oldest beyond this don't surface
logging            = truncate        # one of: disabled, truncate, append
enable_exports     = 1               # 1 = archive main pawn at each inn rest; 0 = skip
enable_updates     = 1               # 1 = writeback hired-pawn gear/knowledge at save; 0 = skip
```

## On-disk layout after install

```
DDDA/
  steam_api.dll                         # gbe_fork's replacement
  steam_settings/
    steam_appid.txt                     # = 367500
    load_dlls/
      pawndb.dll                 # this project
      pawndb.ini                 # config
      pawndb/
        010/
          0090A8E8.pawn                 # 8 KB encoded pawn blob
          0090A8E8.meta                 # 18 int32s of leaderboard card data
          0090A8E8.json                 # human-readable descriptor
          0090A8E8.xml                  # cSAVE_DATA_CMC region snapshot (for restore_pawn)
        021/
          ...
  pawndb.log                            # log file (next to DDDA.exe)
```

The file name is referenced within the pawn data for the mod to be able
to write back the updates in the form of <NNN>:<HEX>, where <NNN> is the
containing folder, and <HEX> is the files creation time hashed. If the
file is moved/renamed, the mod won't be able to find it and write back
the updates.
Folder names must be a zero-padded 3-digit level (`001`..`200`); anything
else is ignored. Filename stems must be 8 hex digits. Each `.pawn` needs
its matching `.meta` sibling to display correct card details in the rift;
`.json` is purely for humans; `.xml` is required by `restore_pawn` and
optional otherwise.

## How it works (short version)

- Hooks eleven vtable slots on `ISteamUserStats`, `ISteamRemoteStorage`,
  and `ISteamUser` via direct pointer overwrite (no function-entry
  patching).
- Scans the on-disk archive tree once at worker-thread startup; every
  FindLeaderboard afterward rewrites the relevant rift boards from the
  in-memory cache (needed because gbe_fork's own writes during rest cycles
  wipe our synthetic entries).
- Archive trigger is the *first* `cDetails=18` upload after `FileShare('0')`
  in a rest cycle — that's when DDDA has uploaded fresh pawn stats, so the
  `.meta` sidecar captures this-rest-accurate data.
- JSON is produced by reading live DDDA.exe memory; the character-data
  base pointer is discovered via signature scan of the game's `.text`
  section (pattern lifted from ddda-dinput8).
- Gear and study writeback piggyback on `FileWrite('DDDA.sav')`. The
  save's XML (~20 MB zlib-deflated) is parsed in-process; for each
  hired-pawn `cSAVE_DATA_CMC` block we read `mArisenName` back out to
  find the source archive, then `pawnxfs` patches the encoded `.pawn`
  (Blowfish + zlib + SHA-1 pipeline) with current gear records plus
  the two 322-entry `mStudyFlag` / `mLocalStudyFlag` u32 arrays — the
  latter copied verbatim, since save XML and XFS bytes match bit-for-bit
  for those fields.

## Troubleshooting

- **No `pawndb.log` appears**: either `logging = disabled` in the INI,
  or the DLL isn't being loaded. Confirm gbe_fork is installed correctly
  and that `steam_settings/load_dlls/pawndb.dll` exists. gbe_fork's
  own `STEAM_LOG_*.log` (next to DDDA.exe) should mention loading it.
- **Rift is empty even though archives exist**: check `pawndb.log` for
  `rescan_all_levels: loaded N fake pawn(s)` — if N is 0, the `<NNN>`
  folder naming is wrong (needs zero-padded 3-digit level), the stems
  aren't 8-hex, or no `.pawn` files are present.
- **A pawn in the rift refuses to summon**: if it's an archive created in
  the current session, that's by design — restart the game and it will
  summon fine. If it's an older archive, check the log for
  `UGCRead: fopen(...) failed` entries pointing at a missing/moved file.

## Scope and non-goals

- Single-player Arisen workflow only; no true multiplayer.
- JSON descriptors cover level/vocation/stats/inclinations/equipped-
  skills sourced from live game memory. Equipment and accumulated
  knowledge (`mStudyFlag` / `mLocalStudyFlag`) are persisted into the
  `.pawn` archive itself, not mirrored into the JSON.
- 32-bit Windows only. Never intended to run outside a Proton-wrapped
  DDDA process.
