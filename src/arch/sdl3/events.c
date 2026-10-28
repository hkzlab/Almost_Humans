/* arch/sdl3/events.c — host events -> the game's two input paths
 * (+0x5ACE fire/joystick bits, +0x48A3 last key; re_map.md input section).
 *
 * The arrows + the ACTION key map to the joystick bits; the printable keys
 * pass through as their ASCII codes (the room's name/code screen).  The
 * action bit is set ONLY by the configured action key (the shipped
 * [0x5B05] default = scancode 0x2C = Z).  The control keys map to the INT 9
 * translate table's codes (re_map.md → the two-channel input row): ESC =
 * 0x1B (the 0x5F11 table's ESC — the room's act0 clears the name), Enter =
 * 0x0D (the name screen's confirm), Backspace = 0x0E (the erase).  Every
 * key-UP clears the code (the ISR's 942F) — a tap must not linger. */
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
    case SDLK_Z:       mask = 0x80u; break;   /* the configured action key
                                              * ([0x5B05] = scancode 0x2C
                                              * = Z) — ONLY this key sets
                                              * the action bit; Enter and
                                              * friends must not (the
                                              * attract's action-bit
                                              * escape fired on a stub-era
                                              * Enter mapping) */
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
    /* the ISR's XLAT table (0x4260) delivers letters UPPERCASE ('a' -> 0x41,
     * re_map.md's input row) — the name screen's dispatch expects those */
    if (k >= 'a' && k <= 'z') return (uint8_t)(k - 0x20u);
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
            /* the ISR's 942F: EVERY key-up clears the flag — a tapped
             * key must not linger as "held" (the stub-era code re-set
             * the code on release, which made one ENTER tap cascade:
             * abort the boot intro AND immediately escape the attract) */
            joyBits(ev.key.key, 0);
            cur.lastKey = 0u;
            break;
        default: break;
        }
    }
    *out = cur;
    /* The ISR's [0x48A3] is rewritten by every make — the BIOS typematic
     * repeats included — and the front-end consumes it (AA4E/AA50); between
     * makes it holds.  The SDL queue's KEY_DOWN events (the OS's typematic
     * repeats included) are those makes: deliver each once, then the byte
     * is clear until the next make lands.  Without this a held key latches
     * and the name screen fills at tick rate instead of the typematic's
     * delay-then-repeat. */
    cur.lastKey = 0u;
}

void sdl3_eventsInit(void) { memset(&cur, 0, sizeof cur); }
