/* arch/sdl3/OSystem.c — the SDL3 vtable + main (port_structure.md §2). */
#include "osystem.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_oldnames.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
#    include <direct.h> /* chdir = _chdir on the Windows CRT */
/* the macro keeps the CRT's spelling on purpose: it maps the POSIX name */
#    define chdir _chdir
#else
#    include <unistd.h>
#endif

void sdl3_video_set_mode(VideoMode mode);
uint8_t *sdl3_video_page(int32_t which);
void sdl3_video_present(int32_t which);
void sdl3_video_set_palette(const uint8_t *p6, uint32_t first, uint32_t n);
void sdl3_events_init(void);
void sdl3_events_poll(InputState *out);
void sdl3_timer_start(uint32_t ticksPerSec, TickFn tick);
void sdl3_tick_sync(uint32_t *lastSeen);
int32_t sdl3_audio_start(void);
int32_t sdl3_audio_play_tune(uint32_t id);
void sdl3_audio_stop(void);
static void _fatal(const char *msg);

const OSystem g_osys_sdl3 = { sdl3_video_set_mode,
                              sdl3_video_page,
                              sdl3_video_present,
                              sdl3_video_set_palette,
                              sdl3_events_poll,
                              sdl3_timer_start,
                              sdl3_tick_sync,
                              sdl3_audio_start,
                              sdl3_audio_play_tune,
                              sdl3_audio_stop,
                              _fatal };

static void _fatal(const char *msg)
{
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", msg);
    exit(1);
}

/* The arch layer owns main() (port_structure.md §2).
 *
 * Usage: humans-sdl3 [path-to-game-resources]
 * The path is chdir'd into so the filesystem module finds the volume files
 * (HUMANS.000/001/002); without it the current directory is used. */
int main(int argc, char **argv)
{
    if (argc == 2) {
        if (chdir(argv[1]) != 0) {
            fprintf(stderr, "cannot chdir to '%s'\n", argv[1]);
            return 1;
        }
    } else {
        fprintf(stderr, "USAGE: %s /path/to/HUMANS/volumes\n", argv[1]);
        SDL_Quit();
        return -1;
    }


    // Initialize video mode
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Initialize event structures
    sdl3_events_init();

    // Hand over to game code
    int32_t r = game_main(&g_osys_sdl3);

    // Time to go
    SDL_Quit();
    return r;
}
