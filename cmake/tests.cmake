# The test suite: Unity, vendored in test/unity (MIT, v2.7.0).
#
# Included by the root CMakeLists.txt (same directory scope, so the lint and
# sanitizer settings and the source-file properties set there still apply).
# Every test is an executable named test_*; `humansre_add_test` registers it
# with ctest.  The tests link `core_test` - the core with its test seams
# compiled in.

# The vendored framework: third-party source, so no project warnings and no
# lint pass (the same rule the vendored audio modules follow).
add_library(unity STATIC test/unity/unity.c)
target_include_directories(unity PUBLIC test/unity)
set_source_files_properties(test/unity/unity.c PROPERTIES SKIP_LINTING TRUE)

# The suite's glue: the argv hand-off and the formatted skip/fail macros.
add_library(test_support STATIC test/test_support.c)
target_include_directories(test_support PUBLIC test)
target_link_libraries(test_support PUBLIC unity)
humansre_warnings(test_support)

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

# One test executable: the module include roots, the probe declarations the
# core headers expose to tests (HUMANSRE_TEST_PROBES, with core_test linked by
# the caller), and the strict warnings.  Sources and link libraries stay the
# caller's; humansre_add_test registers it with ctest.
function(humansre_test_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${HUMANSRE_CORE_DIR}
                                               ${HUMANSRE_ARCH_DIR}
                                               ${HUMANSRE_PLATFORM_DIR})
    target_compile_definitions(${name} PRIVATE HUMANSRE_TEST_PROBES)
    target_link_libraries(${name} PRIVATE test_support)
    humansre_warnings(${name})
    if(NOT HUMANSRE_BUILD_TESTS)
        set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endfunction()

function(humansre_add_test)
    if(HUMANSRE_BUILD_TESTS)
        add_test(${ARGN})
    endif()
endfunction()

if(HUMANSRE_BUILD_TESTS)
    enable_testing()
else()
    set_target_properties(core_test unity test_support
        PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

set(HUMANS_VOLUMES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/volumes"
    CACHE PATH "Game data root with one directory per shipped language")

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

# The headless tests: Unity (test/unity), one test case per executable.
humansre_test_executable(test_loop test/test_loop.c
    src/arch/platform/null/OSystem.c)          # the null backend: the attract composes
target_link_libraries(test_loop PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME flow COMMAND test_loop)

# The byteorder module's known-value check: the expected byte layouts are
# absolute, so the same asserts prove both orders on a LE host and a BE host.
humansre_test_executable(test_byteorder test/test_byteorder.c)
humansre_add_test(NAME byteorder COMMAND test_byteorder)

# The text table's structural invariants (no game data needed): the printf
# markers, the in-place digit fields and the dialog line counts in every
# language column.
humansre_test_executable(test_game_strings test/test_game_strings.c src/core/game_strings.c)
humansre_add_test(NAME game_strings COMMAND test_game_strings)

# The language detector: the FNV-1a vectors, the digest -> column map and
# each shipped volume set (one subdirectory per language under the root).
humansre_test_executable(test_ver_detect test/test_ver_detect.c
    src/arch/platform/null/OSystem.c)          # the null backend: the warning's destination
target_link_libraries(test_ver_detect PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME ver_detect COMMAND test_ver_detect ${HUMANS_VOLUMES_DIR})

humansre_test_executable(test_res test/test_res.c
    src/arch/platform/null/OSystem.c)
target_link_libraries(test_res PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME res COMMAND test_res ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The party screen's bake-frame law test: the composed page = the extracted
# .BIT verbatim (the layout law y=20+q / x=4n+p, closed 2026-10-29).
humansre_test_executable(test_party test/test_party.c
    src/arch/platform/null/OSystem.c)
target_link_libraries(test_party PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME party COMMAND test_party ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})
humansre_test_executable(test_party_reset test/test_party_reset.c
    src/arch/platform/null/OSystem.c)
target_link_libraries(test_party_reset PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME party_reset COMMAND test_party_reset ${HUMANS_DATA_DIR})

# The fade-ladder harness: the port's own DAC progression per tick against
# the original's recorded hardware reads.
humansre_test_executable(test_fadeladder test/test_fadeladder.c
    src/arch/platform/null/OSystem.c)
target_link_libraries(test_fadeladder PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME fadeladder COMMAND test_fadeladder ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The scene player test: level 0 plays ANI00.ANI's 35 script entries onto
# page 0; the final page must match the reference playback checksums.
humansre_test_executable(test_scene test/test_scene.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_scene PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME scene COMMAND test_scene ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The full-game_main smoke test: a scripted input sequence drives the flow
# (attract -> menu -> level -> intro -> quit) under the null backend.
humansre_test_executable(test_smoke test/test_smoke.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_smoke PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME smoke COMMAND test_smoke ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The level-end transition: draw_level_text's grey/gradient/black fades (D-27,
# D-29) and check_highscore's outcome gate (D-28), driven through game_main.
humansre_test_executable(test_endflow test/test_endflow.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_endflow PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME endflow_won COMMAND test_endflow ${HUMANS_DATA_DIR} 1)
humansre_add_test(NAME endflow_lost COMMAND test_endflow ${HUMANS_DATA_DIR} 0)
humansre_add_test(NAME endflow_restart COMMAND test_endflow ${HUMANS_DATA_DIR} 2)

# The boot-intro test: load_intro_scene's display-list player over the
# scrolled bac.bit background; the final page must match the reference.
humansre_test_executable(test_boot test/test_boot.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_boot PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME boot COMMAND test_boot ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# D-245: [0x48A3] is a held-key level; a held Return must abort the logo to
# the attract path instead of needing a second press.
humansre_test_executable(test_boot_key test/test_boot_key.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_boot_key PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME boot_key COMMAND test_boot_key ${HUMANS_DATA_DIR})

# D-246: the frame-0 transition flash must run over the rendered scene, not
# the stale ITB card.
humansre_test_executable(test_boot_flash test/test_boot_flash.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_boot_flash PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME boot_flash COMMAND test_boot_flash ${HUMANS_DATA_DIR})

# D-247: the INTRO.RSC scene's two held-key polls - c0c6 after the frame
# wait (1000:b787) and c0af in the transition fade (1000:b863).
humansre_test_executable(test_boot_scene_key test/test_boot_scene_key.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_boot_scene_key PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME boot_scene_key_trans COMMAND test_boot_scene_key ${HUMANS_DATA_DIR} trans)
humansre_add_test(NAME boot_scene_key_frame COMMAND test_boot_scene_key ${HUMANS_DATA_DIR} frame)

# The level test: all 160 records from LEVELS\L*.BIN, the census + the
# byte-exact maps vs the extracted files.
humansre_test_executable(test_level test/test_level.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_level PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME level COMMAND test_level ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The status-bar test: the band + the five digit marks against the shipped
# PANELN1/PANELN2/PANEL4 assets, the roll/slide arithmetic, the tribe counter,
# the time-up, and the level_load + level_tick integration.
humansre_test_executable(test_statusbar test/test_statusbar.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_statusbar PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME statusbar COMMAND test_statusbar ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The entity test: the closed entity system - the lb#1 spawn fields, the
# 0xB4 join, the exact-flip walk over the baked waypoint words, and the
# 160-level spawn census.
humansre_test_executable(test_entity test/test_entity.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_entity PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME entity COMMAND test_entity ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The in-level pause menu + SAVE GAME: the dialog behaviour (202F:0x5BBC /
# 0x5CD8), the writer's header and the read-back.
humansre_test_executable(test_pause test/test_pause.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_pause PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME pause COMMAND test_pause ${HUMANS_DATA_DIR})

# menu_flag_tick's win pan blocks input_dispatch_loop (1000:3540): a key
# pressed during it (ESC here) is not seen until the pan returns.
humansre_test_executable(test_pan_input test/test_pan_input.c
    src/arch/platform/null/OSystem.c)          # the null backend: the input script
target_link_libraries(test_pan_input PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME pan_input COMMAND test_pan_input ${HUMANS_DATA_DIR})

# The save-game slot load (entry:004C): the room's RESTORE GAME counter
# reaches save.c, the save's level/time/members replace the room's.
humansre_test_executable(test_save test/test_save.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_save PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME save COMMAND test_save ${HUMANS_DATA_DIR})

# pause and save both drive slot 2 (SAVEB) in the data directory: a parallel
# ctest must not run them at the same time (they skip only when a real SAVEB
# is already there).
if(HUMANSRE_BUILD_TESTS)
    set_tests_properties(pause save PROPERTIES RESOURCE_LOCK save_slot_b)
endif()

# The front-end room's option read side: walk to each board, press ACTION,
# and assert the counter/badge pixels and each setting's consumer.
humansre_test_executable(test_frontend test/test_frontend.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_frontend PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME frontend COMMAND test_frontend ${HUMANS_DATA_DIR})

# The attract scroller test: effect 4's strip + presenter against a
# reference dump (absent dumps = skip).
set(HUMANS_REF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../analysis_artifacts/attract_scroll"
    CACHE PATH "reference dumps for the attract scroller (strip + frames)")
humansre_test_executable(test_attract test/test_attract.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_attract PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME attract COMMAND test_attract ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR} ${HUMANS_REF_DIR})

# The core's audio state machine over the real AdLib module: play_tune,
# music_resume/stop, the fade pair, driver_call and the 2-of-3 tick, with
# rendered samples as the oracle.
humansre_test_executable(test_audio_link test/test_audio_link.c)
target_link_libraries(test_audio_link PRIVATE core_test fs_posix audio_adlib)
humansre_add_test(NAME audio_link COMMAND test_audio_link ${HUMANS_DATA_DIR})

# The module's private state against a live ADLIB.DRV snapshot (white-box:
# the test includes the module).  Pins the row decode, the cmd 0x0A volume
# decay, the F-numbers and the instrument registers.
humansre_test_executable(test_adlib_live test/test_adlib_live.c
    src/arch/audio/adlib/thirdparty/opl3.c)
target_link_libraries(test_adlib_live PRIVATE m arch_mem)
humansre_add_test(NAME adlib_live COMMAND test_adlib_live ${HUMANS_FIX_DIR}/HUMANS.000/MUS/TITLE.ALB)

# The AdLib driver module: a real .ALB through the INT 60h ABI, so the
# sequencer decodes the image and the vendored OPL3 renders it.
humansre_test_executable(test_adlib test/test_adlib.c)
target_link_libraries(test_adlib PRIVATE audio_adlib)
humansre_add_test(NAME adlib COMMAND test_adlib ${HUMANS_FIX_DIR})

# The Beeper driver module against a real .BEP: the loader geometry, the row
# decode (both voices), the note path, the vendored impulse model and a live
# state snapshot.  White-box (the test includes the module) so the slot
# state and the rendered pitch are both checked.
humansre_test_executable(test_beeper test/test_beeper.c
    src/arch/audio/beeper/thirdparty/pcspeaker_impulse.c)
target_link_libraries(test_beeper PRIVATE core_test fs_posix arch_mem)
if(NOT MSVC)
    target_link_libraries(test_beeper PRIVATE m)
endif()
humansre_add_test(NAME beeper COMMAND test_beeper ${HUMANS_FIX_DIR} ${HUMANS_DATA_DIR})

# The settable arch heap: OSystem's pair and the arch modules share it; a
# counting pair must see both, and a driver's create/destroy must balance.
humansre_test_executable(test_arch_mem test/test_arch_mem.c)
target_link_libraries(test_arch_mem PRIVATE arch_mem audio_adlib)
humansre_add_test(NAME arch_mem COMMAND test_arch_mem)

# The MT-32 driver module against a real .RLD and a real ROM set.  The ROMs
# are not shipped: ctest passes HUMANS_MT32_ROM_DIR when it exists, and the
# test skips when it does not (MT32_CONTROL.ROM / MT32_PCM.ROM or any
# compatible pair, including MAME-style split images).
if(HUMANSRE_MT32)
    humansre_test_executable(test_mt32 test/test_mt32.c)
    target_include_directories(test_mt32 PRIVATE ${MT32EMU_INCLUDE_DIR})
    target_link_libraries(test_mt32 PRIVATE ${MT32EMU_LIBRARY} fs_posix arch_mem)
    humansre_add_test(NAME mt32 COMMAND test_mt32 ${HUMANS_FIX_DIR})
    if(HUMANSRE_BUILD_TESTS AND HUMANS_MT32_ROM_DIR AND EXISTS "${HUMANS_MT32_ROM_DIR}")
        set_tests_properties(mt32 PROPERTIES
            ENVIRONMENT "HUMANS_MT32_ROMS=${HUMANS_MT32_ROM_DIR}")
    endif()
endif()

# The flow test: level_run's need > members gate, entry LAB_00CD's
# post-level spine ([0x22]++/[0x26]-- -> the next level or COMPLETE), the
# level-end hook ([0x138E]) and draw_level_text's two pages.
humansre_test_executable(test_flow test/test_flow.c
    src/arch/platform/null/OSystem.c)          # the null backend's test hooks
target_link_libraries(test_flow PRIVATE core_test fs_posix arch_mem)
humansre_add_test(NAME level_flow COMMAND test_flow ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The optional doubled level viewport (--view wide): the scale switch, the 2x
# expand, level 0's 640x400 framing (the world's 64-px margin, the bar at the
# field's bottom), the loading screen, the front-end room's 192*scale field
# and the witchdoctor's claim.  A classic-only build has no wide path to test.
if(HUMANSRE_VIEWPORT_WIDE)
    humansre_test_executable(test_wide test/test_wide.c
        src/arch/platform/null/OSystem.c)
    target_link_libraries(test_wide PRIVATE core_test fs_posix arch_mem)
    humansre_add_test(NAME wide COMMAND test_wide ${HUMANS_DATA_DIR})
endif()
