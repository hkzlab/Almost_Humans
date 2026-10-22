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

# The headless reference port: no dependencies, always builds.
add_executable(humans-null
    src/arch/null/OSystem.c
    src/arch/null/main.c
)
target_link_libraries(humans-null PRIVATE core)
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
        src/arch/sdl3/fs.c
    )
    target_link_libraries(humans-sdl3 PRIVATE core SDL3::SDL3)
    target_compile_options(humans-sdl3 PRIVATE -Wall -Wextra -Werror)
else()
    message(STATUS "SDL3 not found - building only humans-null")
endif()
