cmake_minimum_required(VERSION 3.24)

set(CMAKE_C_COMPILER clang)
project(HumansRE C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The game core: pure C99, no port #ifdefs, no dependencies beyond the CRT
# (port_structure.md §5).  It talks to the machine only through OSystem.
add_library(core STATIC
    src/game_main.c
    src/res.c
    src/video.c
    src/palette.c
    src/font.c
    src/statusbar.c
    src/scene.c
    src/level.c
    src/entity.c
    src/script.c
    src/audio.c
    src/frontend.c
    src/save.c
)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_compile_options(core PRIVATE -Wall -Wextra -Werror)

# The headless tests: assert-based, no framework.
add_executable(test_loop tests/test_loop.c)
target_include_directories(test_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/arch)
target_link_libraries(test_loop PRIVATE core fs_posix)
target_compile_options(test_loop PRIVATE -Wall -Wextra -Werror)
add_test(NAME flow COMMAND test_loop)
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
target_compile_options(humans-null PRIVATE -Wall -Wextra -Werror)

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
    target_compile_options(humans-sdl3 PRIVATE -Wall -Wextra -Werror)
else()
    message(STATUS "SDL3 not found - building only humans-null")
endif()
