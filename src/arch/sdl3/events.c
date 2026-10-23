/* arch/sdl3/events.c — host events -> the game's two input paths
 * (+0x5ACE fire/joystick bits, +0x48A3 last key; re_map.md input section).
 *
 * The arrows + the fire button map to the joystick bits; ESC maps to the
 * game's key code 0x01 (the menus' back-out key); the printable keys pass
 * through as the ASCII codes (the code-entry screen). */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <string.h>

static InputState cur;

static void joyBits(SDL_Keycode k, int down)
{
    unsigned mask;
    switch (k) {
    case SDLK_UP:      mask = 0x01u; break;
    case SDLK_DOWN:    mask = 0x02u; break;
    case SDLK_LEFT:    mask = 0x04u; break;
    case SDLK_RIGHT:   mask = 0x08u; break;
    case SDLK_SPACE: case SDLK_LCTRL: case SDLK_RCTRL:
    case SDLK_RETURN: case SDLK_KP_ENTER:
        mask = 0x80u; break;                   /* the fire button */
    default: return;
    }
    if (down) cur.joyBits |= mask;
    else cur.joyBits &= (uint8_t)~mask;
}

void sdl3_eventsPoll(InputState *out)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            cur.quit = 1;
            break;
        case SDL_EVENT_KEY_DOWN:
            joyBits(ev.key.key, 1);
            if (ev.key.key == SDLK_ESCAPE) cur.lastKey = 0x01u;
            else if (ev.key.key >= 0x20 && ev.key.key < 0x80)
                cur.lastKey = (uint8_t)ev.key.key;
            break;
        case SDL_EVENT_KEY_UP:
            joyBits(ev.key.key, 0);
            if (ev.key.key == SDLK_ESCAPE) cur.lastKey = 0u;
            else if (ev.key.key >= 0x20 && ev.key.key < 0x80) cur.lastKey = 0u;
            break;
        default: break;
        }
    }
    *out = cur;
}

void sdl3_eventsInit(void) { memset(&cur, 0, sizeof cur); }
