cmake_minimum_required(VERSION 3.24)

# The toolchain: clang stays the default wherever it is on PATH (the Linux
# build, and the msys64 clangarm64 one on Windows/ARM64); an explicit
# -DCMAKE_C_COMPILER or $CC always wins, and a configure with neither falls
# back to CMake's own detection (MSVC on Windows without clang).
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(HUMANSRE_CLANG clang)
    if(HUMANSRE_CLANG)
        set(CMAKE_C_COMPILER "${HUMANSRE_CLANG}")
    endif()
endif()
project(HumansRE VERSION 1.0.0 LANGUAGES C)

# The port's title and version, printed by each arch's main() at startup.
set(HUMANSRE_PROJECT_TITLE "Almost Humans")
add_compile_definitions(HUMANSRE_PROJECT_TITLE="${HUMANSRE_PROJECT_TITLE}"
                        HUMANSRE_VERSION="${PROJECT_VERSION}")

# The module roots: the core, the arch ABI and the platform backends.
set(HUMANSRE_CORE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
set(HUMANSRE_ARCH_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
set(HUMANSRE_PLATFORM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/arch/platform)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The strict warnings, per toolchain spelling: warnings are errors.  MSVC
# needs /utf-8 so a non-ASCII byte in a string literal is not a hard error.
function(humansre_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# Tests assert.  Release's NDEBUG compiles assert() to (void)0, which both
# empties the check and leaves its state variables unused under -Werror.
# Undefine NDEBUG after the release flags for the asserting test targets.
function(humansre_test_asserts tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${tgt} PRIVATE -UNDEBUG)
    endif()
endfunction()

# The unused-include audit: with this ON, clang-tidy's include-cleaner fails
# the build on every #include whose declarations the file never uses.  On by
# default wherever clang-tidy is on PATH (a machine without it still builds;
# -DHUMANSRE_UNUSED_INCLUDES=OFF skips the tidy pass).  Only the unused half
# runs - the missing-include half would demand one header per symbol, which
# contradicts the umbrella headers the projects ship (SDL3/SDL.h, mt32emu.h);
# those two are ignored so the convention does not read as an unused include.
find_program(HUMANSRE_CLANG_TIDY NAMES clang-tidy)
if(HUMANSRE_CLANG_TIDY)
    set(HUMANSRE_UNUSED_INCLUDES_DEFAULT ON)
else()
    set(HUMANSRE_UNUSED_INCLUDES_DEFAULT OFF)
endif()
option(HUMANSRE_UNUSED_INCLUDES "Fail the build on unused #includes"
       ${HUMANSRE_UNUSED_INCLUDES_DEFAULT})
if(HUMANSRE_UNUSED_INCLUDES)
    if(NOT HUMANSRE_CLANG_TIDY)
        message(FATAL_ERROR "HUMANSRE_UNUSED_INCLUDES=ON needs clang-tidy on PATH")
    endif()
    set(CMAKE_C_CLANG_TIDY "${HUMANSRE_CLANG_TIDY}"
        "--checks=-*,misc-include-cleaner"
        "--config={CheckOptions: {misc-include-cleaner.MissingIncludes: false, misc-include-cleaner.IgnoreHeaders: '(SDL3/SDL[.]h$|mt32emu[.]h$)'}}"
        "--warnings-as-errors=misc-include-cleaner")
    # the vendored emulators keep their upstream #include set (CMake >= 3.27)
    set_source_files_properties(
        src/arch/audio/adlib/thirdparty/opl3.c
        src/arch/audio/beeper/thirdparty/pcspeaker_impulse.c
        PROPERTIES SKIP_LINTING TRUE)
endif()

# The narrowing audit: with this ON, every implicit
# integer truncation or sign change aborts the program at the site, so the test
# suite proves the narrowed locals never silently wrap.  The third-party OPL3
# emulator keeps its intentional 16-bit sample conversions (excluded below).
option(HUMANSRE_SANITIZE_IMPLICIT "Abort on implicit integer conversions" OFF)
if(HUMANSRE_SANITIZE_IMPLICIT)
    add_compile_options(-fsanitize=implicit-conversion
                        -fno-sanitize-recover=implicit-conversion)
    add_link_options(-fsanitize=implicit-conversion)
    set_source_files_properties(
        src/arch/audio/adlib/thirdparty/opl3.c
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=implicit-conversion")
endif()

# The game core: pure C99, no port #ifdefs, no dependencies beyond the CRT.
# It talks to the machine only through OSystem.
set(HUMANSRE_CORE_SOURCES
    src/core/game_main.c
    src/arch/osystem.c
    src/core/res.c
    src/core/game_resources.c
    src/core/video.c
    src/core/palette.c
    src/core/font.c
    src/core/attract.c
    src/core/statusbar.c
    src/core/scene/scene_ani.c
    src/core/scene/scene_play.c
    src/core/scene/scene_boot.c
    src/core/level/level_load.c
    src/core/level/level_assets.c
    src/core/level/level_play.c
    src/core/level/level_view.c
    src/core/level/level_tribe.c
    src/core/level/level_palette.c
    src/core/level/level_loading.c
    src/core/level/level_spark.c
    src/core/level/level_disc.c
    src/core/map/map_wheel.c
    src/core/map/map_band.c
    src/core/map/map_list.c
    src/core/map/map_flow.c
    src/core/score.c
    src/core/hiscore.c
    src/core/game_strings.c
    src/core/entity.c
    src/core/object/object_core.c
    src/core/object/object_render.c
    src/core/object/object_sprites.c
    src/core/object/object_spawn.c
    src/core/object/object_sequence.c
    src/core/object/object_command.c
    src/core/object/object_walk.c
    src/core/object/object_units.c
    src/core/script.c
    src/core/audio.c
    src/core/frontend.c
    src/core/menu.c
    src/core/textpage.c
    src/core/save.c
    src/core/party_intro.c
    src/core/ver_detect.c
)

# The shipping core: the test seams (the *_for_probe accessors and the
# test-only accessors in src/*.h) are compiled out.
add_library(core STATIC ${HUMANSRE_CORE_SOURCES})
target_include_directories(core PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR}
                                      ${HUMANSRE_CORE_DIR})
humansre_warnings(core)

# The test core: the same sources with HUMANSRE_TEST_PROBES, so the test
# executables see the seam declarations and definitions.  No release binary
# links this library; the seams cannot reach a shipped build.
add_library(core_test STATIC ${HUMANSRE_CORE_SOURCES})
target_include_directories(core_test PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR}
                                           ${HUMANSRE_CORE_DIR})
target_compile_definitions(core_test PRIVATE HUMANSRE_TEST_PROBES)
humansre_warnings(core_test)

# The tests are development tooling: a single-config Release configure leaves
# them out, so its default build produces the shipping binaries only.  Pass
# -DHUMANSRE_BUILD_TESTS=ON to build them anyway, or OFF to skip them in any
# configuration.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_humansre_tests_default OFF)
else()
    set(_humansre_tests_default ON)
endif()
option(HUMANSRE_BUILD_TESTS "Build the test executables" ${_humansre_tests_default})

# The null backend is test/debug tooling (the tests compile it directly):
# like the tests, it is not part of a Release build.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_humansre_null_default OFF)
else()
    set(_humansre_null_default ON)
endif()
option(HUMANSRE_BUILD_NULL "Build the headless null port" ${_humansre_null_default})

function(humansre_test_executable name)
    add_executable(${name} ${ARGN})
    if(NOT HUMANSRE_BUILD_TESTS)
        set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endfunction()

function(humansre_add_test)
    if(HUMANSRE_BUILD_TESTS)
        add_test(${ARGN})
    endif()
endfunction()

if(NOT HUMANSRE_BUILD_TESTS)
    set_target_properties(core_test PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# The width audit: every implicit integer narrowing in
# the core must be an explicit cast.  -Wno-sign-conversion keeps the many
# deliberate word/byte sign tests out of the noise.
if(NOT MSVC)
    target_compile_options(core PRIVATE -Wconversion -Wno-sign-conversion)
    target_compile_options(core_test PRIVATE -Wconversion -Wno-sign-conversion)
endif()

# The headless tests: assert-based, no framework.
humansre_test_executable(test_loop tests/test_loop.c
    src/arch/platform/null/OSystem.c)          # the null backend: the attract composes
target_include_directories(test_loop PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_loop PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_loop)
humansre_add_test(NAME flow COMMAND test_loop)

# The byteorder module's known-value check: the expected byte layouts are
# absolute, so the same asserts prove both orders on a LE host and a BE host.
humansre_test_executable(test_byteorder tests/test_byteorder.c)
target_include_directories(test_byteorder PRIVATE ${HUMANSRE_CORE_DIR})
humansre_warnings(test_byteorder)
humansre_add_test(NAME byteorder COMMAND test_byteorder)

# The text table's structural invariants (no game data needed): the printf
# markers, the in-place digit fields and the dialog line counts in every
# language column.
humansre_test_executable(test_game_strings tests/test_game_strings.c src/core/game_strings.c)
target_include_directories(test_game_strings PRIVATE ${HUMANSRE_CORE_DIR})
humansre_warnings(test_game_strings)
humansre_add_test(NAME game_strings COMMAND test_game_strings)

# The language detector: the FNV-1a vectors, the digest -> column map and
# each shipped volume set (one subdirectory per language under the root).
set(HUMANS_VOLUMES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/volumes"
    CACHE PATH "Game data root with one directory per shipped language")
humansre_test_executable(test_ver_detect tests/test_ver_detect.c
    src/arch/platform/null/OSystem.c)          # the null backend: the warning's destination
target_include_directories(test_ver_detect PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_ver_detect PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_ver_detect)
humansre_add_test(NAME ver_detect COMMAND test_ver_detect ${HUMANS_VOLUMES_DIR})

# The resource fixture test runs against the real game data (HUMANS.00N);
# point HUMANS_DATA_DIR at a directory holding them.  Missing data = skip.
set(HUMANS_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/volumes/ENG"
    CACHE PATH "Game data directory with HUMANS.000/001/002")
set(HUMANS_FIX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/resources/ENG"
    CACHE PATH "Extracted-tree fixture for the byte-exact expectations")
# The MT-32 ROMs are not part of the repository.  Default to the checkout's
# set (or a HUMANS_MT32_ROMS already exported); the mt32 test skips when this
# is empty or does not exist.
set(HUMANS_MT32_ROM_DIR "$ENV{HUMANS_MT32_ROMS}"
    CACHE PATH "MT-32 ROM directory (MT32_CONTROL.ROM/MT32_PCM.ROM)")
if(NOT HUMANS_MT32_ROM_DIR)
    set(_humans_mt32_guess "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/MT32/MT32_legacy")
    if(EXISTS "${_humans_mt32_guess}")
        set(HUMANS_MT32_ROM_DIR "${_humans_mt32_guess}")
    endif()
endif()
humansre_test_executable(test_res tests/test_res.c
    src/arch/platform/null/OSystem.c)
target_include_directories(test_res PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_res PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_res)
humansre_add_test(NAME res COMMAND test_res ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The party screen's bake-frame law test: the composed page = the extracted
# .BIT verbatim (the layout law y=20+q / x=4n+p, closed 2026-10-29).
humansre_test_executable(test_party tests/test_party.c
    src/arch/platform/null/OSystem.c)
target_include_directories(test_party PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_party PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_party)
humansre_add_test(NAME party COMMAND test_party ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})
humansre_test_executable(test_party_reset tests/test_party_reset.c
    src/arch/platform/null/OSystem.c)
target_include_directories(test_party_reset PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_party_reset PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_party_reset)
humansre_add_test(NAME party_reset COMMAND test_party_reset ${HUMANS_DATA_DIR})

# The fade-ladder harness: the port's own DAC progression per tick against
# the original's recorded hardware reads.
humansre_test_executable(test_fadeladder tests/test_fadeladder.c
    src/arch/platform/null/OSystem.c)
target_include_directories(test_fadeladder PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_fadeladder PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_fadeladder)
humansre_add_test(NAME fadeladder COMMAND test_fadeladder ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})


# The scene player test: level 0 plays ANI00.ANI's 35 script entries onto
# page 0; the final page must match the reference playback checksums.
humansre_test_executable(test_scene tests/test_scene.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_scene PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_scene PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_scene)
humansre_add_test(NAME scene COMMAND test_scene ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The full-game_main smoke test: a scripted input sequence drives the flow
# (attract -> menu -> level -> intro -> quit) under the null backend.
humansre_test_executable(test_smoke tests/test_smoke.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_smoke PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_smoke PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_smoke)
humansre_add_test(NAME smoke COMMAND test_smoke ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The level-end transition: draw_level_text's grey/gradient/black fades (D-27,
# D-29) and check_highscore's outcome gate (D-28), driven through game_main.
humansre_test_executable(test_endflow tests/test_endflow.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_endflow PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_endflow PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_endflow)
humansre_add_test(NAME endflow_won COMMAND test_endflow ${HUMANS_DATA_DIR} 1)
humansre_add_test(NAME endflow_lost COMMAND test_endflow ${HUMANS_DATA_DIR} 0)
humansre_add_test(NAME endflow_restart COMMAND test_endflow ${HUMANS_DATA_DIR} 2)

# The boot-intro test: load_intro_scene's display-list player over the
# scrolled bac.bit background; the final page must match the reference.
humansre_test_executable(test_boot tests/test_boot.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_boot PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_boot PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_boot)
humansre_add_test(NAME boot COMMAND test_boot ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# D-245: [0x48A3] is a held-key level; a held Return must abort the logo to
# the attract path instead of needing a second press.
humansre_test_executable(test_boot_key tests/test_boot_key.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_boot_key PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_boot_key PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_boot_key)
humansre_add_test(NAME boot_key COMMAND test_boot_key ${HUMANS_DATA_DIR})

# D-246: the frame-0 transition flash must run over the rendered scene, not
# the stale ITB card.
humansre_test_executable(test_boot_flash tests/test_boot_flash.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_boot_flash PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_boot_flash PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_boot_flash)
humansre_add_test(NAME boot_flash COMMAND test_boot_flash ${HUMANS_DATA_DIR})

# D-247: the INTRO.RSC scene's two held-key polls - c0c6 after the frame
# wait (1000:b787) and c0af in the transition fade (1000:b863).
humansre_test_executable(test_boot_scene_key tests/test_boot_scene_key.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_boot_scene_key PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_boot_scene_key PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_boot_scene_key)
humansre_add_test(NAME boot_scene_key_trans COMMAND test_boot_scene_key ${HUMANS_DATA_DIR} trans)
humansre_add_test(NAME boot_scene_key_frame COMMAND test_boot_scene_key ${HUMANS_DATA_DIR} frame)

# The level test: all 160 records from LEVELS\L*.BIN, the census + the
# byte-exact maps vs the extracted files.
humansre_test_executable(test_level tests/test_level.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_level PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_level PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_level)
humansre_add_test(NAME level COMMAND test_level ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The status-bar test: the band + the five digit marks against the shipped
# PANELN1/PANELN2/PANEL4 assets, the roll/slide arithmetic, the tribe counter,
# the time-up, and the level_load + level_tick integration.
humansre_test_executable(test_statusbar tests/test_statusbar.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_statusbar PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_statusbar PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_statusbar)
humansre_add_test(NAME statusbar COMMAND test_statusbar ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The entity test: the closed entity system - the lb#1 spawn fields, the
# 0xB4 join, the exact-flip walk over the baked waypoint words, and the
# 160-level spawn census.
humansre_test_executable(test_entity tests/test_entity.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_entity PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_entity PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_entity)
humansre_add_test(NAME entity COMMAND test_entity ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The in-level pause menu + SAVE GAME: the dialog behaviour (202F:0x5BBC /
# 0x5CD8), the writer's header and the read-back.
humansre_test_executable(test_pause tests/test_pause.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_pause PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_pause PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_pause)
humansre_add_test(NAME pause COMMAND test_pause ${HUMANS_DATA_DIR})

# menu_flag_tick's win pan blocks input_dispatch_loop (1000:3540): a key
# pressed during it (ESC here) is not seen until the pan returns.
humansre_test_executable(test_pan_input tests/test_pan_input.c
    src/arch/platform/null/OSystem.c)          # the null backend: the input script
target_include_directories(test_pan_input PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_pan_input PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_pan_input)
humansre_add_test(NAME pan_input COMMAND test_pan_input ${HUMANS_DATA_DIR})

# The save-game slot load (entry:004C): the room's RESTORE GAME counter
# reaches save.c, the save's level/time/members replace the room's.
humansre_test_executable(test_save tests/test_save.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_save PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_save PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_save)
humansre_add_test(NAME save COMMAND test_save ${HUMANS_DATA_DIR})

# pause and save both drive slot 2 (SAVEB) in the data directory: a parallel
# ctest must not run them at the same time (they skip only when a real SAVEB
# is already there).
if(HUMANSRE_BUILD_TESTS)
    set_tests_properties(pause save PROPERTIES RESOURCE_LOCK save_slot_b)
endif()

# The front-end room's option read side: walk to each board, press ACTION,
# and assert the counter/badge pixels and each setting's consumer.
humansre_test_executable(test_frontend tests/test_frontend.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_frontend PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_frontend PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_frontend)
humansre_add_test(NAME frontend COMMAND test_frontend ${HUMANS_DATA_DIR})

# The attract scroller test: effect 4's strip + presenter against a
# reference dump (absent dumps = skip).
set(HUMANS_REF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/attract_scroll"
    CACHE PATH "reference dumps for the attract scroller (strip + frames)")
humansre_test_executable(test_attract tests/test_attract.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_attract PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_attract PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_attract)
humansre_add_test(NAME attract COMMAND test_attract ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR} ${HUMANS_REF_DIR})

if(HUMANSRE_BUILD_TESTS)
    enable_testing()
endif()

# The filesystem modules: the arch backends link
# exactly one at build time; both export the same `fs` interface (fs/fs.h).
# fs_null is the intentionally blank backend (every call fails); it is built
# but linked into no current target, and is kept as the reference module for a
# no-data port and as the original's deliberate "no disc" state.
add_library(fs_null  src/arch/fs/null/fs_null.c)
add_library(fs_posix src/arch/fs/posix/fs_posix.c)
target_include_directories(fs_null  PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_include_directories(fs_posix PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})

# The process heap (arch/mem.h): OSystem's memAlloc/memFree point at these
# forwarders, and the arch modules call them directly.  It is the only
# non-thirdparty place that calls the CRT allocator.
add_library(arch_mem src/arch/arch_mem.c)
target_include_directories(arch_mem PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})

# The sound-driver modules (audio/sound_driver.h): a port links the ones it
# offers and selects one at run time.  adlib/sound_driver_adlib.c is
# ADLIB.DRV's sequencer over a vendored Nuked-OPL3-fast 1.8-fast.3
# (thirdparty/, LGPL-2.1-or-later; a bit-exact performance fork of Nuked-OPL3
# 1.8 cfedb09, plus the port's key-off latch - see port_deviations.md D-249);
# beeper/sound_driver_beeper.c is BEEPER.DRV's sequencer
# over the vendored PC-speaker impulse model (thirdparty/,
# GPL-2.0-or-later); null/ prints every command and renders silence.  The
# audio_null module is intentionally blank and linked into no executable.
add_library(audio_adlib
    src/arch/audio/adlib/sound_driver_adlib.c
    src/arch/audio/adlib/thirdparty/opl3.c)
add_library(audio_beeper
    src/arch/audio/beeper/sound_driver_beeper.c
    src/arch/audio/beeper/thirdparty/pcspeaker_impulse.c)
add_library(audio_null src/arch/audio/null/sound_driver_null.c)
target_include_directories(audio_adlib PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_include_directories(audio_adlib PRIVATE ${HUMANSRE_CORE_DIR})
target_include_directories(audio_beeper PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_include_directories(audio_beeper PRIVATE ${HUMANSRE_CORE_DIR})
target_include_directories(audio_null PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(audio_adlib PRIVATE arch_mem)
target_link_libraries(audio_beeper PRIVATE arch_mem)
target_link_libraries(audio_null PRIVATE arch_mem)
if(NOT MSVC)
    # the vendored impulse model builds its sinc LUT with sin/cos
    target_link_libraries(audio_beeper PRIVATE m)
endif()

# The MT-32 driver (audio/mt32): MT32.DRV's sequencer over the system
# mt32emu library, which is a hard requirement for this module - the port
# simply does not offer `--audio mt32` when it is missing.  mt32emu is
# LGPL-2.1-or-later, same as the vendored Nuked-OPL3.
find_path(MT32EMU_INCLUDE_DIR mt32emu.h)
find_library(MT32EMU_LIBRARY NAMES mt32emu)
if(MT32EMU_INCLUDE_DIR AND MT32EMU_LIBRARY)
    set(HUMANSRE_MT32 TRUE)
    add_library(audio_mt32 src/arch/audio/mt32/sound_driver_mt32.c)
    target_include_directories(audio_mt32 PUBLIC ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
    target_include_directories(audio_mt32 PRIVATE ${HUMANSRE_CORE_DIR}
                                                  ${MT32EMU_INCLUDE_DIR})
    target_link_libraries(audio_mt32 PUBLIC ${MT32EMU_LIBRARY})
    target_link_libraries(audio_mt32 PRIVATE arch_mem)
    message(STATUS "mt32emu: ${MT32EMU_LIBRARY} - --audio mt32 available")
else()
    set(HUMANSRE_MT32 FALSE)
    message(STATUS "mt32emu not found - --audio mt32 unavailable")
endif()

# The core's audio state machine over the real AdLib module: play_tune,
# music_resume/stop, the fade pair, driver_call and the 2-of-3 tick, with
# rendered samples as the oracle.
humansre_test_executable(test_audio_link tests/test_audio_link.c)
target_include_directories(test_audio_link PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_audio_link PRIVATE core_test fs_posix audio_adlib)
humansre_warnings(test_audio_link)
humansre_test_asserts(test_audio_link)
humansre_add_test(NAME audio_link COMMAND test_audio_link ${HUMANS_DATA_DIR})

# The module's private state against a live ADLIB.DRV snapshot (white-box:
# the test includes the module).  Pins the row decode, the cmd 0x0A volume
# decay, the F-numbers and the instrument registers.
humansre_test_executable(test_adlib_live tests/test_adlib_live.c
    src/arch/audio/adlib/thirdparty/opl3.c)
target_include_directories(test_adlib_live PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_adlib_live PRIVATE m arch_mem)
humansre_warnings(test_adlib_live)
humansre_test_asserts(test_adlib_live)
humansre_add_test(NAME adlib_live COMMAND test_adlib_live ${HUMANS_FIX_DIR}/HUMANS.000/MUS/TITLE.ALB)

# The AdLib driver module: a real .ALB through the INT 60h ABI, so the
# sequencer decodes the image and the vendored OPL3 renders it.
humansre_test_executable(test_adlib tests/test_adlib.c)
target_include_directories(test_adlib PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_adlib PRIVATE audio_adlib)
humansre_warnings(test_adlib)
humansre_test_asserts(test_adlib)
humansre_add_test(NAME adlib COMMAND test_adlib ${HUMANS_FIX_DIR})

# The Beeper driver module against a real .BEP: the loader geometry, the row
# decode (both voices), the note path, the vendored impulse model and a live
# state snapshot.  White-box (the test includes the module) so the slot
# state and the rendered pitch are both checked.
humansre_test_executable(test_beeper tests/test_beeper.c
    src/arch/audio/beeper/thirdparty/pcspeaker_impulse.c)
target_include_directories(test_beeper PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_beeper PRIVATE core_test fs_posix arch_mem)
if(NOT MSVC)
    target_link_libraries(test_beeper PRIVATE m)
endif()
humansre_warnings(test_beeper)
humansre_test_asserts(test_beeper)
humansre_add_test(NAME beeper COMMAND test_beeper ${HUMANS_FIX_DIR} ${HUMANS_DATA_DIR})

# The settable arch heap: OSystem's pair and the arch modules share it; a
# counting pair must see both, and a driver's create/destroy must balance.
humansre_test_executable(test_arch_mem tests/test_arch_mem.c)
target_include_directories(test_arch_mem PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_arch_mem PRIVATE arch_mem audio_adlib)
humansre_warnings(test_arch_mem)
humansre_test_asserts(test_arch_mem)
humansre_add_test(NAME arch_mem COMMAND test_arch_mem)

# The MT-32 driver module against a real .RLD and a real ROM set.  The ROMs
# are not shipped: ctest passes HUMANS_MT32_ROM_DIR when it exists, and the
# test skips when it does not (MT32_CONTROL.ROM / MT32_PCM.ROM or any
# compatible pair, including MAME-style split images).
if(HUMANSRE_MT32)
    humansre_test_executable(test_mt32 tests/test_mt32.c)
    target_include_directories(test_mt32 PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR} ${MT32EMU_INCLUDE_DIR})
    target_link_libraries(test_mt32 PRIVATE ${MT32EMU_LIBRARY} arch_mem)
    humansre_warnings(test_mt32)
    humansre_add_test(NAME mt32 COMMAND test_mt32 ${HUMANS_FIX_DIR})
    if(HUMANSRE_BUILD_TESTS AND HUMANS_MT32_ROM_DIR AND EXISTS "${HUMANS_MT32_ROM_DIR}")
        set_tests_properties(mt32 PROPERTIES
            ENVIRONMENT "HUMANS_MT32_ROMS=${HUMANS_MT32_ROM_DIR}")
    endif()
endif()

# The headless reference port: no SDL dependency, Debug/test only (its
# backend records counters no release build needs).
if(HUMANSRE_BUILD_NULL)
    add_executable(humans-null
        src/arch/platform/null/OSystem.c
        src/arch/platform/null/main.c
    )
    target_link_libraries(humans-null PRIVATE core fs_posix arch_mem)
    humansre_warnings(humans-null)
endif()

# The first real port (optional so the tree builds on machines without SDL3).
find_package(SDL3 3.2 CONFIG QUIET)
if(SDL3_FOUND)
    add_executable(humans-sdl3
        src/arch/platform/sdl3/OSystem.c
        src/arch/platform/sdl3/video.c
        src/arch/platform/sdl3/events.c
        src/arch/platform/sdl3/timer.c
        src/arch/platform/sdl3/audio.c
    )
    target_link_libraries(humans-sdl3 PRIVATE core fs_posix audio_adlib audio_beeper arch_mem SDL3::SDL3)
    target_include_directories(humans-sdl3 PRIVATE ${HUMANSRE_CORE_DIR})
    # The env-var probes (HUMANSRE_AUDIO_PROBE / HUMANSRE_RATE_PROBE) are
    # test-framework code too: a Debug configure keeps them for measuring,
    # a release/default one compiles them out.
    target_compile_definitions(humans-sdl3 PRIVATE $<$<CONFIG:Debug>:HUMANSRE_TEST_PROBES>)
    if(HUMANSRE_MT32)
        target_link_libraries(humans-sdl3 PRIVATE audio_mt32)
        target_compile_definitions(humans-sdl3 PRIVATE HUMANSRE_HAVE_MT32=1)
    endif()
    humansre_warnings(humans-sdl3)
else()
    message(STATUS "SDL3 not found - no SDL3 port in this build")
endif()

# The flow test: level_run's need > members gate, entry LAB_00CD's
# post-level spine ([0x22]++/[0x26]-- -> the next level or COMPLETE), the
# level-end hook ([0x138E]) and draw_level_text's two pages.
humansre_test_executable(test_flow tests/test_flow.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_flow PRIVATE ${HUMANSRE_CORE_DIR} ${HUMANSRE_ARCH_DIR} ${HUMANSRE_PLATFORM_DIR})
target_link_libraries(test_flow PRIVATE core_test fs_posix arch_mem)
humansre_warnings(test_flow)
humansre_add_test(NAME level_flow COMMAND test_flow ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# Every test executable includes the probe declarations in the core headers
# (guarded by HUMANSRE_TEST_PROBES) and links the probe-enabled core_test.
if(HUMANSRE_BUILD_TESTS)
    get_property(_humansre_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_humansre_t IN LISTS _humansre_targets)
        if(_humansre_t MATCHES "^test_")
            target_compile_definitions(${_humansre_t} PRIVATE HUMANSRE_TEST_PROBES)
        endif()
    endforeach()
endif()
