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
- Archives are plain files on disk — you can delete them, copy them
  between installs, or share them with a friend
- Per-archive `.json` sidecar with stats/vocation/skills for human
  browsing
- Toggles to disable archiving or writeback if you just want to browse
  read-only

## Note on releasing hired pawns

Releasing a hired pawn through the normal in-game dialog is safe — the
game won't crash and the archive stays intact. But the release gift and
star rating you'd normally send to the pawn's owner go nowhere (there is
no owner; it's your own archive). Only the pawn's **last saved state**
before release is persisted: the gear they had equipped, the knowledge
they'd gained. If you want a change to stick, make sure the game has
saved (inn rest or manual save) while the pawn is still in your party.

## Requirements

- Dragon's Dogma: Dark Arisen (Steam, 32-bit)
- [gbe_fork](https://github.com/Detanup01/gbe_fork) Steam emulator —
  pawndb is a gbe_fork plugin and will not work on retail Steam

Single-player only. Does not touch or interact with the real Steam pawn
network.

## Source

Open source, MIT-licensed: <https://github.com/istvan-sipos/pawndb>

