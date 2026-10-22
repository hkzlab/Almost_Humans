/* arch/sdl3/events.c — host events -> the game's two input paths
 * (+0x5ACE fire/joystick bits, +0x48A3 last key; re_map.md input section). */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <string.h>

static InputState cur;

void sdl3_eventsPoll(InputState *out)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT: cur.quit = 1; break;
        case SDL_EVENT_KEY_DOWN: cur.lastKey = (uint8_t)ev.key.key; break;
        case SDL_EVENT_KEY_UP:   cur.lastKey = 0; break;
        default: break;
        }
    }
    *out = cur;
}

void sdl3_eventsInit(void) { memset(&cur, 0, sizeof cur); }
