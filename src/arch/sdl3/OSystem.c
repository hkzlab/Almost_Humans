/* arch/sdl3/OSystem.c — the SDL3 vtable (port_structure.md §2). */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

void sdl3_videoInit(void);
uint8_t *sdl3_videoPage(int which);
void sdl3_videoPresent(int which);
void sdl3_videoSetPalette(const uint8_t *p6, unsigned first, unsigned n);
void sdl3_eventsInit(void);
void sdl3_eventsPoll(InputState *out);
void sdl3_timerStart(unsigned ticksPerSec, TickFn tick);
int  sdl3_audioStart(void);
int  sdl3_audioPlayTune(unsigned id);
void sdl3_audioStop(void);
FILE *sdl3_openGameFile(const char *name);
FILE *sdl3_openSaveFile(unsigned slot, const char *mode);

static void fatal(const char *msg)
{
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", msg);
    exit(1);
}

const OSystem osys_sdl3 = {
    sdl3_videoInit, sdl3_videoPage, sdl3_videoPresent, sdl3_videoSetPalette,
    sdl3_eventsPoll, sdl3_timerStart,
    sdl3_audioStart, sdl3_audioPlayTune, sdl3_audioStop,
    sdl3_openGameFile, sdl3_openSaveFile,
    fatal
};

/* The arch layer owns main() (port_structure.md §2). */
int main(void)
{
    sdl3_eventsInit();
    return game_main(&osys_sdl3);
}
