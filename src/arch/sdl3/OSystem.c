/* arch/sdl3/OSystem.c — the SDL3 vtable + main (port_structure.md §2). */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
#include <direct.h> /* chdir = _chdir on the Windows CRT */
#define chdir _chdir
#else
#include <unistd.h>
#endif

void sdl3_videoInit(void);
uint8_t *sdl3_videoPage(int which);
void sdl3_videoPresent(int which);
void sdl3_videoSetPalette(const uint8_t *p6, unsigned first, unsigned n);
void sdl3_eventsInit(void);
void sdl3_eventsPoll(InputState *out);
void sdl3_timerStart(unsigned ticksPerSec, TickFn tick);
void sdl3_tickSync(unsigned *lastSeen);
int  sdl3_audioStart(void);
int  sdl3_audioPlayTune(unsigned id);
void sdl3_audioStop(void);

static void fatal(const char *msg)
{
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", msg);
    exit(1);
}

const OSystem osys_sdl3 = {
    sdl3_videoInit, sdl3_videoPage, sdl3_videoPresent, sdl3_videoSetPalette,
    sdl3_eventsPoll, sdl3_timerStart, sdl3_tickSync,
    sdl3_audioStart, sdl3_audioPlayTune, sdl3_audioStop,
    fatal
};

/* The arch layer owns main() (port_structure.md §2).
 *
 * Usage: humans-sdl3 [path-to-game-resources]
 * The path is chdir'd into so the filesystem module finds the volume files
 * (HUMANS.000/001/002); without it the current directory is used. */
int main(int argc, char **argv)
{
    if (argc > 1) {
        if (chdir(argv[1]) != 0) {
            fprintf(stderr, "cannot chdir to '%s'\n", argv[1]);
            return 1;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    sdl3_eventsInit();
    int r = game_main(&osys_sdl3);
    SDL_Quit();
    return r;
}
