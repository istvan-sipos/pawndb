# Install (binary release)

Project repo: <https://github.com/istvan-sipos/pawndb>

Target: 32-bit **Dragon's Dogma: Dark Arisen** (Steam App ID `367500`) on
top of the [gbe_fork](https://github.com/Detanup01/gbe_fork) Steam emulator.
pawndb is a gbe_fork plugin; it does not work with the retail Steam DLL.

Grab the latest release from
<https://github.com/Detanup01/gbe_fork/releases> before you start; you'll
need files from it in both steps below. Leave DDDA's original
`steam_api.dll` in place until step 2.

## 1. Generate the interfaces file (from the *original* DLL)

DDDA ships with a pre-2016 Steamworks; gbe_fork needs the interface list
extracted from the stock DLL. **Do this before overwriting it.** Drag the
game's original `steam_api.dll` onto `generate_interfaces_file.exe`
(shipped with gbe_fork). That produces `steam_interfaces.txt`.

Place it at:

```
.../DDDA/steam_settings/steam_interfaces.txt
```

Also create `steam_settings/steam_appid.txt` containing a single line:

```
367500
```

## 2. Install gbe_fork into the game folder

Now overwrite the game's `steam_api.dll` with gbe_fork's **32-bit** build:

```
.../steamapps/common/DDDA/steam_api.dll        <- gbe_fork's steam_api.dll (x86)
```

Back up the original first if you want an easy rollback.

## 3. Drop in pawndb

From the pawndb release archive, copy `pawndb.dll` and `pawndb.ini` into
gbe_fork's auto-load directory:

```
.../DDDA/steam_settings/load_dlls/pawndb.dll
.../DDDA/steam_settings/load_dlls/pawndb.ini
```

gbe_fork's `steam_api.dll` scans `steam_settings/load_dlls/` at startup and
loads every DLL it finds, so no further registration is needed.

Edit `pawndb.ini` if you want to change the archive root, rift result cap,
logging mode, or the `enable_exports` / `enable_updates` toggles. Defaults
are fine for a first run.

The release archive also contains `tools/restore_pawn.exe` — a separate
CLI for promoting an archived pawn into your save's main-pawn slot.
Place it wherever you like (it doesn't have to live next to the DLL);
see `RESTORE_PAWN.md` for usage.

## 4. Verify

Launch DDDA. On a successful load you should see:

- `pawndb.log` next to `DDDA.exe`, starting with a
  `=== pawndb <version> loaded, log at '...' (mode=...) ===` banner and
  a `config:` summary line.
- After the first inn rest: a new `pawndb/<level>/<HEX>.pawn` (plus
  `.meta`, `.json`, and `.xml`) under `steam_settings/load_dlls/`.
- In the rift search board for your pawn's level: archived pawns from
  previous sessions appear as hirable entries.

If `pawndb.log` is missing, either `logging = disabled` is set in the INI,
or the DLL never loaded — check that it's a 32-bit build and that gbe_fork
itself is working (the game reaching the main menu is a good signal).
