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
project(HumansRE C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The strict warnings, per toolchain spelling.  MSVC needs /utf-8 (the
# sources carry UTF-8 text in their comments).
function(humansre_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# The game core: pure C99, no port #ifdefs, no dependencies beyond the CRT
# (port_structure.md §5).  It talks to the machine only through OSystem.
add_library(core STATIC
    src/game_main.c
    src/res.c
    src/video.c
    src/palette.c
    src/font.c
    src/attract.c
    src/statusbar.c
    src/scene.c
    src/level.c
    src/map.c
    src/score.c
    src/hiscore.c
    src/entity.c
    src/object.c
    src/object_ds.c
    src/script.c
    src/audio.c
    src/frontend.c
    src/menu.c
    src/textpage.c
    src/save.c
    src/party_intro.c
)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
humansre_warnings(core)

# The headless tests: assert-based, no framework.
add_executable(test_loop tests/test_loop.c
    src/arch/null/OSystem.c)          # the null backend: the attract composes
target_include_directories(test_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_loop PRIVATE core fs_posix)
humansre_warnings(test_loop)
add_test(NAME flow COMMAND test_loop)

# The byteorder module's known-value check: the expected byte layouts are
# absolute, so the same asserts prove both orders on a LE host and a BE host.
add_executable(test_byteorder tests/test_byteorder.c)
target_include_directories(test_byteorder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
humansre_warnings(test_byteorder)
add_test(NAME byteorder COMMAND test_byteorder)

# The resource fixture test runs against the real game data (HUMANS.00N);
# point HUMANS_DATA_DIR at a directory holding them.  Missing data = skip.
set(HUMANS_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/volumes/ENG"
    CACHE PATH "Game data directory with HUMANS.000/001/002")
set(HUMANS_FIX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../binaries/resources/ENG"
    CACHE PATH "Extracted-tree fixture for the byte-exact expectations")
add_executable(test_res tests/test_res.c)
target_include_directories(test_res PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_res PRIVATE core fs_posix)
humansre_warnings(test_res)
add_test(NAME res COMMAND test_res ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The party screen's bake-frame law test: the composed page = the extracted
# .BIT verbatim (the layout law y=20+q / x=4n+p, closed 2026-10-29).
add_executable(test_party tests/test_party.c
    src/arch/null/OSystem.c)
target_include_directories(test_party PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_party PRIVATE core fs_posix)
humansre_warnings(test_party)
add_test(NAME party COMMAND test_party ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})
add_executable(test_party_reset tests/test_party_reset.c
    src/arch/null/OSystem.c)
target_include_directories(test_party_reset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_party_reset PRIVATE core fs_posix)
humansre_warnings(test_party_reset)
add_test(NAME party_reset COMMAND test_party_reset ${HUMANS_DATA_DIR})

# The fade-ladder harness: the port's own DAC progression per tick against
# the original's recorded hardware reads (the offline palette instrument).
add_executable(test_fadeladder tests/test_fadeladder.c
    src/arch/null/OSystem.c)
target_include_directories(test_fadeladder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_fadeladder PRIVATE core fs_posix)
humansre_warnings(test_fadeladder)
add_test(NAME fadeladder COMMAND test_fadeladder ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})


# The scene player test: level 0 plays ANI00.ANI's 35 script entries onto
# page 0; the final page must match the reference playback checksums.
add_executable(test_scene tests/test_scene.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_scene PRIVATE core fs_posix)
humansre_warnings(test_scene)
add_test(NAME scene COMMAND test_scene ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The full-game_main smoke test: a scripted input sequence drives the flow
# (attract -> menu -> level -> intro -> quit) under the null backend.
add_executable(test_smoke tests/test_smoke.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_smoke PRIVATE core fs_posix)
humansre_warnings(test_smoke)
add_test(NAME smoke COMMAND test_smoke ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The level-end transition: draw_level_text's grey/gradient/black fades (D-27,
# D-29) and check_highscore's outcome gate (D-28), driven through game_main.
add_executable(test_endflow tests/test_endflow.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_endflow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_endflow PRIVATE core fs_posix)
humansre_warnings(test_endflow)
add_test(NAME endflow_won COMMAND test_endflow ${HUMANS_DATA_DIR} 1)
add_test(NAME endflow_lost COMMAND test_endflow ${HUMANS_DATA_DIR} 0)
add_test(NAME endflow_restart COMMAND test_endflow ${HUMANS_DATA_DIR} 2)

# The boot-intro test: load_intro_scene's display-list player over the
# scrolled bac.bit background; the final page must match the reference.
add_executable(test_boot tests/test_boot.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_boot PRIVATE core fs_posix)
humansre_warnings(test_boot)
add_test(NAME boot COMMAND test_boot ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The level test: all 160 records from LEVELS\L*.BIN, the census + the
# byte-exact maps vs the extracted files.
add_executable(test_level tests/test_level.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_level PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_level PRIVATE core fs_posix)
humansre_warnings(test_level)
add_test(NAME level COMMAND test_level ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The status-bar test: the band + the five digit marks against the shipped
# PANELN1/PANELN2/PANEL4 assets, the roll/slide arithmetic, the odometer,
# the time-up, and the level_load + level_tick integration.
add_executable(test_statusbar tests/test_statusbar.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_statusbar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_statusbar PRIVATE core fs_posix)
humansre_warnings(test_statusbar)
add_test(NAME statusbar COMMAND test_statusbar ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The entity test: the closed §12.6 system — the lb#1 spawn fields, the
# 0xB4 join, the exact-flip walk over the baked waypoint words, and the
# 160-level spawn census.
add_executable(test_entity tests/test_entity.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_entity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_entity PRIVATE core fs_posix)
humansre_warnings(test_entity)
add_test(NAME entity COMMAND test_entity ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})

# The in-level pause menu + SAVE GAME: the dialog behaviour (202F:0x5BBC /
# 0x5CD8), the writer's header and the read-back.
add_executable(test_pause tests/test_pause.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_pause PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_pause PRIVATE core fs_posix)
humansre_warnings(test_pause)
add_test(NAME pause COMMAND test_pause ${HUMANS_DATA_DIR})

# The save-game slot load (entry:004C): the room's RESTORE GAME counter
# reaches save.c, the save's level/time/members replace the room's.
add_executable(test_save tests/test_save.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_save PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_save PRIVATE core fs_posix)
humansre_warnings(test_save)
add_test(NAME save COMMAND test_save ${HUMANS_DATA_DIR})

# The front-end room's option read side: walk to each board, press ACTION,
# and assert the counter/badge pixels and each setting's consumer.
add_executable(test_frontend tests/test_frontend.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_frontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_frontend PRIVATE core fs_posix)
humansre_warnings(test_frontend)
add_test(NAME frontend COMMAND test_frontend ${HUMANS_DATA_DIR})

# The attract scroller test: effect 4's strip + presenter against the
# reference dump from tools/attract_scroll_sim.py (regenerate it by
# running that script; absent dumps = skip).
set(HUMANS_REF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/attract_scroll"
    CACHE PATH "attract_scroll_sim.py reference dumps (strip + frames)")
add_executable(test_attract tests/test_attract.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_attract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_attract PRIVATE core fs_posix)
humansre_warnings(test_attract)
add_test(NAME attract COMMAND test_attract ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR} ${HUMANS_REF_DIR})

enable_testing()

# The filesystem modules (port_structure.md §2): the arch backends link
# exactly one at build time; both export the same `fs` interface (fs/fs.h).
add_library(fs_null  src/arch/fs/null/fs_null.c)
add_library(fs_posix src/arch/fs/posix/fs_posix.c)
target_include_directories(fs_null  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_include_directories(fs_posix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)

# The headless reference port: no SDL dependency, always builds.
add_executable(humans-null
    src/arch/null/OSystem.c
    src/arch/null/main.c
)
target_link_libraries(humans-null PRIVATE core fs_posix)
humansre_warnings(humans-null)

# The first real port (optional so the tree builds on machines without SDL3).
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    add_executable(humans-sdl3
        src/arch/sdl3/OSystem.c
        src/arch/sdl3/video.c
        src/arch/sdl3/events.c
        src/arch/sdl3/timer.c
        src/arch/sdl3/audio.c
    )
    target_link_libraries(humans-sdl3 PRIVATE core fs_posix SDL3::SDL3)
    humansre_warnings(humans-sdl3)
else()
    message(STATUS "SDL3 not found - building only humans-null")
endif()

# The flow test: level_run's need > members gate, entry LAB_00CD's
# post-level spine ([0x22]++/[0x26]-- -> the next level or COMPLETE), the
# level-end hook ([0x138E]) and draw_level_text's two pages.
add_executable(test_flow tests/test_flow.c
    src/arch/null/OSystem.c)          # the null backend's test hooks
target_include_directories(test_flow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_flow PRIVATE core fs_posix)
humansre_warnings(test_flow)
add_test(NAME level_flow COMMAND test_flow ${HUMANS_DATA_DIR} ${HUMANS_FIX_DIR})
