# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Common Changelog](https://common-changelog.org),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 1.0.0 - 2026-11-08

_First release: a C99 port of the 1992 DOS game Humans, reimplemented from the original's (GOG's CD version) disassembly._

### Added

- Port the game flow: the boot intro, attract mode, the level-select room, the level start/end chain, the score
  screen, the MAP and the high-score table
- Port the level engine: the `L*.BIN` maps and records, the tile table, the entities, the placed-object behaviours
  and the full action-command walk
- Port the level-intro party screen, the status bar, the pause menu and the save/restore, with save files
  byte-compatible with the original's
- Port the resource layer: the `HUMANS.00N` volumes, the chunk table, LZW, the `.ANI`/`.RSC` scene player, the
  palettes and the fade machine
- Add the `OSystem` abstraction with an SDL3 backend, a headless null backend and a test suite that runs the
  ported subsystems against the shipped game data
- Add AdLib, Beeper and MT-32 sound drivers behind the INT 60h-shaped ABI, selectable at run time
- Add the version banner, the SDL3 `--version`/`-v` option and the version in the window title
- Add the `--ticks` limit to the headless executable so a smoke run terminates
