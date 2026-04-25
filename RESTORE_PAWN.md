# restore_pawn — promote any archived pawn into your main-pawn slot

`restore_pawn.exe` ships alongside the mod. It takes any pawn you've
archived and slots them in as your **main pawn** in the save you point
it at.

## Why bother

Once a pawn is your main pawn, the in-game UI lets you freely change
things that are normally locked behind hire-and-release: vocation,
primary/secondary skills, augments, inclinations. So if you've got a
pawn you love but you want to take them in a different combat
direction, you can: restore them, load up, edit them in-game, rest at
an inn, and they're re-archived with the new build.

## How to use

1. Close DDDA.
2. Open a command prompt and run:
   ```
   tools\restore_pawn.exe path\to\<archive>.pawn path\to\DDDA.sav
   ```
   You can pass either the `.pawn` file (the tool finds the matching
   `.xml` sidecar automatically) or the `.xml` directly. With gbe_fork,
   the save lives under your `%APPDATA%` folder, at
   `%APPDATA%\GSE Saves\367500\remote\DDDA.sav` — that's the path you
   pass to the tool. (On Linux/Proton the same path resolves inside the
   Wine prefix's `drive_c\users\steamuser\AppData\Roaming\…`.)
3. The tool writes a `DDDA.sav.bak` first, then overwrites the save.
4. Launch the game and load. Your main pawn is now the imported one —
   gear, skills, vocation, knowledge, the lot.
5. **Save at an inn before doing anything risky.** This refreshes the
   game's internal checkpoint snapshot. Otherwise, reloading from the
   last checkpoint will revert your main pawn.

Flags:

- `--no-backup` — skip the `.bak` (not recommended).
- `--force` — reserved; no effect in this version.

## Two things to know

**It only works on pawns archived after you install this version of
the mod.** Older archives (from previous pawndb releases) don't have
the data the tool needs. Easy fix: re-summon the pawn from the Rift
once, hire them, rest at an inn — that regenerates the sidecar and
they become restorable.

**Your main pawn keeps their face.** The imported pawn's appearance
(face, body, voice, name) does not transfer — the visual model stays
the one you already had. Everything else (skills, gear, vocation,
knowledge, augments, inclinations) does transfer. If you do want a
full visual swap, your option for now is the in-game stylist. (Or 
use dinput8.dll)

## If something goes wrong

The tool always writes `DDDA.sav.bak` next to your save before
touching it (unless you pass `--no-backup`). To roll back, close the
game, delete `DDDA.sav`, and rename `DDDA.sav.bak` to `DDDA.sav`.
