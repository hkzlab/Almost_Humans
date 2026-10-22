/* arch/sdl3/timer.c — the fixed-tick source: the PIT/INT 8 heartbeat's
 * equivalent.  The main loop paces at ticksPerSec and calls tick() once
 * per tick (port_structure.md §3: the ISR body becomes the callback). */
#include "osystem.h"
#include <SDL3/SDL.h>

static unsigned tick_ms = 55;   /* the PIT 18.2 Hz equivalent, rounded */

void sdl3_timerStart(unsigned ticksPerSec, TickFn tick)
{
    (void)ticksPerSec;
    tick_ms = 55;
    Uint64 next = SDL_GetTicks() + tick_ms;
    for (;;) {
        SDL_Delay(2);
        Uint64 now = SDL_GetTicks();
        while (now >= next) {
            tick();
            next += tick_ms;
        }
    }
}
