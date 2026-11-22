# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Common Changelog](https://common-changelog.org),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 1.1.0 - 22/11/2026

_PC-speaker level music now resolves the shipped tunes, plus fixes in tune selection for cutscenes._

### Changed

- Stop using magic numbers for audio driver IDs: The numbers were taken from the original binary and used as-is, but make for confusing code. Moved to an enum

### Added

- PC-speaker mode (`--audio beeper`) maps the level tune to the shipped `LEV1..LEV9.BEP` arrangements instead of the original's `LEVA..LEVR` requests, so we actually get pc speaker music in levels. This is an actual variation from the original PC CD version: pc speaker was not selectable, and was broken if forced.

### Fixed

- The level cutscene loads its own static `mus\levA` / `mus\title` names, this avoid trying to load the wrong tune file during cutscenes


## 1.0.0 - 21/11/2026

_First release: a C99/SDL3 port of the 1992 DOS game "The Humans", reimplemented from the original's (GOG's CD version) disassembly._

### Added

- Game flow, Level engine, Resource handling
- `OSystem` abstraction with an SDL3 backend
- headless null backend and a test suite that runs the ported subsystems against the shipped game data
- AdLib, pc speaker and MT-32 sound drivers selectable via command line
- Detect the resources language from the hash of the first 2048 bytes of `HUMANS.001`
- Experimental "wide" viewmode added: it doubles the viewport so the whole level is viewable during gameplay
