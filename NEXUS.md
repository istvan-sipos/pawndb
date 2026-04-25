# pawndb — a personal Rift for your own pawns

If you've ever wanted to roll a full party of *your own* pawns from previous
playthroughs, this is for you.

Every time you rest at an inn, pawndb quietly saves a snapshot of your
current main pawn to a local archive. Those archived pawns then show up as
hirable entries in the Rift search board, filed by level, forever. Summon
them just like you'd summon a friend's pawn from Steam. The exported pawns
can be used in subsequent playthroughs.

When you hire one of your archived pawns and change their gear, or take
them out to fight new monsters, pawndb writes those changes *back* into the
archive on save. Gear, equipment upgrades, and the pawn's accumulated
monster/area/quest knowledge all persist. The pawn you summon next time
remembers everything.

## Features

- Every inn rest auto-archives your main pawn
- Archived pawns appear in the Rift search, filtered by level, up to 100
  per level (configurable)
- Gear and knowledge changes on hired pawns write back to the source
  archive
- Bonus tool: **restore_pawn** — promote any archived pawn into your
  main-pawn slot, so you can edit vocation / skills / augments /
  inclinations in-game (see below)
- Archives are plain files on disk — you can delete them, copy them
  between installs, or share them with a friend
- Toggles to disable archiving or writeback if you just want to browse
  read-only

## Bonus tool: restore_pawn

The release ships with `tools/restore_pawn.exe`, a small CLI that
promotes any of your archived pawns into your **main-pawn slot** in
your save. Once a pawn is your main pawn, you can edit vocation, skills,
augments, and inclinations. Skills, gear, vocation, and knowledge
transfer; the visual model (face, body, voice) stays your existing main
pawn's, so you don't lose the look you crafted in the editor.

Full usage and caveats: see `RESTORE_PAWN.md` in the release archive.

## Note on releasing hired pawns

Releasing a hired pawn through the normal in-game dialog is safe, but the
release gift and star rating you'd normally send to the pawn's owner go
nowhere (there is no owner; it's your own archive). Only the pawn's 
**last saved state** before release is persisted: the gear they had
equipped, the knowledge they'd gained. If you want a change to stick,
make sure the game has saved (inn rest or manual save) while the pawn is
still in your party.

## Requirements

- Dragon's Dogma: Dark Arisen (Steam, 32-bit)
- [gbe_fork](https://github.com/Detanup01/gbe_fork) Steam emulator —
  pawndb is a gbe_fork plugin and will not work on retail Steam (but
  probably works with other GBE forks/releases)

Single-player only. Does not touch or interact with the real Steam pawn
network.

## Source

Open source, MIT-licensed: <https://github.com/istvan-sipos/pawndb>

## Credits

Thanks to everyone involved in
<https://github.com/Detanup01/gbe_fork>
<https://github.com/kubik-jaroslav/ddda-dinput8>

[FluffyQuack](https://www.fluffyquack.com/) for DDsavetool

