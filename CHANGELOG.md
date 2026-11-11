# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Common Changelog](https://common-changelog.org),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 1.0.0 - **UNRELEASED**

_First release: a C99 port of the 1992 DOS game Humans, reimplemented from the original's (GOG's CD version) disassembly._

### Added

- Game flow: the boot intro, attract mode, the level-select room, the level start/end chain, the score
  screen, the MAP and the high-score table
- Level engine: the `L*.BIN` maps and records, the tile table, the entities, the placed-object behaviours
  and the full action-command walk
- Port the resource layer: the `HUMANS.00N` volumes, the chunk table, LZW, the `.ANI`/`.RSC` scene player, the
  palettes and the fade machine
- Add the `OSystem` abstraction with an SDL3 backend, a headless null backend and a test suite that runs the
  ported subsystems against the shipped game data
- Add AdLib, Beeper and MT-32 sound drivers behind the INT 60h-shaped ABI, selectable at run time
- Detect the installed language from the first 2048 bytes of `HUMANS.001` (FNV-1a 32)

### Fixed

- A restart kept the win pan's marker mode, so the `0xBB` marker blocks the pan removed stayed
  erased (and passable) after RESTART THIS LEVEL; the port now clears it at `1000:35A2`'s
  unconditional arm
- The tribe count was re-seeded from the LEVEL board on every level load; it now carries across
  levels and a restart restores the level-start count, as `entry 1000:009C..00B1` and
  `level_run 306D` do
- The play loop no longer reads ESC/ACTION, opens the time-up dialog or dispatches a pending
  code while the win pan runs (`1000:3540`'s blocking loop defers them)
