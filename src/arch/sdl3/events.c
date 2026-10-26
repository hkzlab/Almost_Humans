/* arch/sdl3/events.c — host events -> the game's two input paths
 * (+0x5ACE fire/joystick bits, +0x48A3 last key; re_map.md input section).
 *
 * The arrows + the fire button map to the joystick bits; the printable keys
 * pass through as their ASCII codes (the room's name/code screen).  The
 * control keys map to the INT 9 translate table's codes (re_map.md → the
 * two-channel input row): ESC = 0x1B (the 0x5F11 table's ESC — the room's
 * act0 clears the name; the stub-era 0x01 "back-out" guess was wrong),
 * Enter = 0x0D (the name screen's confirm), Backspace = 0x0E (the erase).
 * Enter/Space/Ctrl also set the fire bit (the tests' confirm convenience —
 * the original's action key is the configured scancode 0x2C). */
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

static uint8_t keyCode(SDL_Keycode k)
{
    switch (k) {
    case SDLK_ESCAPE:    return 0x1Bu;   /* ESC — the 0x5F11 table's key   */
    case SDLK_BACKSPACE: return 0x0Eu;   /* the erase action's key         */
    case SDLK_RETURN: case SDLK_KP_ENTER:
                        return 0x0Du;   /* the confirm action's key       */
    default: break;
    }
    if (k >= 0x20 && k < 0x80) return (uint8_t)k;   /* the printables     */
    return 0u;
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
            cur.lastKey = keyCode(ev.key.key);
            break;
        case SDL_EVENT_KEY_UP:
            joyBits(ev.key.key, 0);
            cur.lastKey = keyCode(ev.key.key);
            break;
        default: break;
        }
    }
    *out = cur;
}

void sdl3_eventsInit(void) { memset(&cur, 0, sizeof cur); }
