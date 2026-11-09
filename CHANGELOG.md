# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Common Changelog](https://common-changelog.org),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 1.0.2 - 2026-11-09

### Changed

- Size the tests' page buffers with `VIDEO_PAGE_BYTES` instead of the repeated `320 * 200` literal

## 1.0.1 - 2026-11-09

### Changed

- Centralise the VGA page geometry in `osystem.h` as `VIDEO_PAGE_W`, `VIDEO_PAGE_H` and
  `VIDEO_PAGE_BYTES`, replacing the repeated 320/200/64000 literals across the core and both
  backends

## 1.0.0 - 2026-11-08

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

