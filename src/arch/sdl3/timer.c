/* arch/sdl3/timer.c — the fixed-tick source: the PIT/INT 8 heartbeat's
 * equivalent.  SDL's timer thread calls the tick callback at ticksPerSec;
 * the game's foreground loop runs on the main thread (the same split as
 * the original's ISR/foreground pair). */
#include "osystem.h"
#include <SDL3/SDL.h>

static TickFn theTick;

static Uint32 SDLCALL timerCb(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)userdata; (void)id;
    theTick();
    return interval;                       /* repeat */
}

void sdl3_timerStart(unsigned ticksPerSec, TickFn tick)
{
    theTick = tick;
    SDL_AddTimer(1000u / ticksPerSec, timerCb, NULL);
}
