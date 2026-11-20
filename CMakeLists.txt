cmake_minimum_required(VERSION 3.24)

# The toolchain
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(HUMANSRE_CLANG clang)
    if(HUMANSRE_CLANG)
        set(CMAKE_C_COMPILER "${HUMANSRE_CLANG}")
    endif()
endif()

# The port's title and version
project(HumansRE VERSION 1.0.0 LANGUAGES C)
set(HUMANSRE_PROJECT_TITLE "Almost Humans")
add_compile_definitions(HUMANSRE_PROJECT_TITLE="${HUMANSRE_PROJECT_TITLE}"
                        HUMANSRE_VERSION="${PROJECT_VERSION}")

# The module roots: the core, the arch ABI and the platform backends.
set(HUMANSRE_CORE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
set(HUMANSRE_ARCH_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
set(HUMANSRE_PLATFORM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/arch/platform)

# Enforce C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strictness: warnings as errors
function(humansre_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /utf-8) # TODO: Need to verify this is working...
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# The unused-include audit: with this ON, clang-tidy's include-cleaner fails
# the build on every #include whose declarations the file never uses.  On by
# default wherever clang-tidy is on PATH
# -DHUMANSRE_UNUSED_INCLUDES=OFF skips the tidy pass
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
    # Skip linting for third party code (changing anything would make it harder to pull in changes)
    # the pcspeaker code is actually adapted and can be linted
    set_source_files_properties(
        src/arch/audio/adlib/thirdparty/opl3.c
        PROPERTIES SKIP_LINTING TRUE)
endif()

# The narrowing audit: with this ON, every implicit
# integer truncation or sign change aborts the program at the site.
# The test suite checks for this.
option(HUMANSRE_SANITIZE_IMPLICIT "Abort on implicit integer conversions" OFF)
if(HUMANSRE_SANITIZE_IMPLICIT)
    add_compile_options(-fsanitize=implicit-conversion
                        -fno-sanitize-recover=implicit-conversion)
    add_link_options(-fsanitize=implicit-conversion)
    set_source_files_properties(
        src/arch/audio/adlib/thirdparty/opl3.c
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=implicit-conversion")
endif()

# The wide level viewport compiled in: ON includes the doubled view (the
# `--view wide` runtime switch, D-274); OFF compiles it out
option(HUMANSRE_VIEWPORT_WIDE "Compile in the doubled (wide) level viewport" ON)
if(HUMANSRE_VIEWPORT_WIDE)
    message(STATUS "viewport: classic + wide (the level view doubled)")
else()
    message(STATUS "viewport: classic only - the wide viewport is compiled out")
endif()

############ THE CORE #############

# The game core: pure C99
set(HUMANSRE_CORE_SOURCES
    src/core/game_main.c
    src/arch/osystem.c
    src/core/res.c
    src/core/lzw.c
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

# One core library at a given viewport build
function(humansre_core_library name wide)
    add_library(${name} STATIC ${HUMANSRE_CORE_SOURCES})
    target_include_directories(${name} PUBLIC ${HUMANSRE_ARCH_DIR}
                                             ${HUMANSRE_PLATFORM_DIR}
                                             ${HUMANSRE_CORE_DIR})
    if(wide)
        target_compile_definitions(${name} PUBLIC HUMANSRE_VIEWPORT_WIDE=1)
    else()
        target_compile_definitions(${name} PUBLIC HUMANSRE_VIEWPORT_WIDE=0)
    endif()
    humansre_warnings(${name})

    # Every implicit integer narrowing in the core must be an explicit cast.
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wconversion -Wno-sign-conversion)
    endif()
endfunction()

# The shipping core: the test seams (the *_for_probe accessors and the
# test-only accessors in src/*.h) are compiled out.
humansre_core_library(core ${HUMANSRE_VIEWPORT_WIDE})

# The test core: the same sources with HUMANSRE_TEST_PROBES, so the test
# executables see the seam declarations and definitions.  No release binary
# links this library; the seams cannot reach a shipped build.
humansre_core_library(core_test ${HUMANSRE_VIEWPORT_WIDE})
target_compile_definitions(core_test PRIVATE HUMANSRE_TEST_PROBES)

# The classic-only core for the sdl3 pair's regression binary: built only when
# the configured viewport is wider, so there are two genuinely different cores.
if(HUMANSRE_VIEWPORT_WIDE)
    humansre_core_library(core_classic 0)
endif()

############ THE ARCH MODULES #############

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
# 1.8 cfedb09, plus the port's key-off latch - see docs/deviations.md D-249);
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
# mt32emu library, which is a hard requirement for this module.
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

############ THE PORTS #############

# The headless null backend is test/debug tooling (the tests compile it
# directly): like the tests, it is not part of a Release build.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_humansre_null_default OFF)
else()
    set(_humansre_null_default ON)
endif()
option(HUMANSRE_BUILD_NULL "Build the headless null port" ${_humansre_null_default})

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
# Two binaries: `humans-sdl3` runs at the configured scale (extended by
# default, `--view classic|wide` still selecting at run time), and
# `humans-sdl3-classic` is a scale-1 build of the same sources and backend -
# no wide code, 320x200 pages - so a side-by-side run isolates regressions.
find_package(SDL3 3.2 CONFIG QUIET)
set(HUMANSRE_SDL3_SOURCES
    src/arch/platform/sdl3/OSystem.c
    src/arch/platform/sdl3/video.c
    src/arch/platform/sdl3/events.c
    src/arch/platform/sdl3/timer.c
    src/arch/platform/sdl3/audio.c
)
if(SDL3_FOUND)
    add_executable(humans-sdl3 ${HUMANSRE_SDL3_SOURCES})
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

    if(HUMANSRE_VIEWPORT_WIDE)
        add_executable(humans-sdl3-classic ${HUMANSRE_SDL3_SOURCES})
        target_link_libraries(humans-sdl3-classic PRIVATE core_classic fs_posix audio_adlib audio_beeper arch_mem SDL3::SDL3)
        target_include_directories(humans-sdl3-classic PRIVATE ${HUMANSRE_CORE_DIR})
        target_compile_definitions(humans-sdl3-classic PRIVATE $<$<CONFIG:Debug>:HUMANSRE_TEST_PROBES>)
        if(HUMANSRE_MT32)
            target_link_libraries(humans-sdl3-classic PRIVATE audio_mt32)
            target_compile_definitions(humans-sdl3-classic PRIVATE HUMANSRE_HAVE_MT32=1)
        endif()
        humansre_warnings(humans-sdl3-classic)
    else()
        message(STATUS "HUMANSRE_VIEWPORT_WIDE=OFF - humans-sdl3 is the classic build, no separate classic binary")
    endif()
else()
    message(STATUS "SDL3 not found - no SDL3 port in this build")
endif()

############ THE TESTS #############

# The test suite lives in its own file; it needs every library above.
include(cmake/tests.cmake)
