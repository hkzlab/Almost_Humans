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

static InputState _cur;

/**
 * @brief Map a key event onto the joystick bit word.
 *
 * @param k the loop counter
 * @param down the key-down flag
 */
static void _joy_bits(SDL_Keycode k, int32_t down)
{
    uint32_t mask;
    switch (k) {
    case SDLK_UP: mask = 0x01u; break;
    case SDLK_DOWN: mask = 0x02u; break;
    case SDLK_LEFT: mask = 0x04u; break;
    case SDLK_RIGHT: mask = 0x08u; break;
    case SDLK_Z:
        mask = 0x80u;
        break;   /* the configured action key
                                              * ([0x5B05] = scancode 0x2C
                                              * = Z) — ONLY this key sets
                                              * the action bit; Enter and
                                              * friends must not (the
                                              * attract's action-bit
                                              * escape fired on a stub-era
                                              * Enter mapping) */
    default: return;
    }
    if (down) _cur.joyBits |= mask;
    else _cur.joyBits &= (uint8_t)~mask;
}

/** @brief The PC set-1 scancode of a make, for the keys the level's key list
 * (DS:0x1989, scan_id_list 1000:386E) binds.  Only those are mapped: the
 * port's other screens read the translated ASCII in lastKey.
 *
 * @param k the loop counter
 * @return the PC set-1 scancode, or 0 when unmapped
 */
static uint8_t _scan_code(SDL_Keycode k)
{
    switch (k) {
    case SDLK_ESCAPE: return 0x01u;
    case SDLK_COMMA: return 0x33u;
    case SDLK_PERIOD: return 0x34u;
    case SDLK_SPACE: return 0x39u;
    case SDLK_F1: return 0x3Bu;
    case SDLK_F2: return 0x3Cu;
    case SDLK_F3: return 0x3Du;
    case SDLK_F4: return 0x3Eu;
    case SDLK_F5: return 0x3Fu;
    case SDLK_F6: return 0x40u;
    case SDLK_F7: return 0x41u;
    case SDLK_F8: return 0x42u;
    case SDLK_F9: return 0x43u;
    case SDLK_F10: return 0x44u;
    default: break;
    }
    return 0u;
}

/**
 * @brief Map an SDL keycode onto the DOS scan code.
 *
 * @param k the loop counter
 * @return the DOS key code, or 0 when unhandled
 */
static uint8_t _key_code(SDL_Keycode k)
{
    switch (k) {
    case SDLK_ESCAPE: return 0x1Bu;   /* ESC — the 0x5F11 table's key   */
    case SDLK_BACKSPACE: return 0x0Eu;   /* the erase action's key         */
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 0x0Du;   /* the confirm action's key       */
    default: break;
    }
    /* the ISR's XLAT table (0x4260) delivers letters UPPERCASE ('a' -> 0x41,
     * re_map.md's input row) — the name screen's dispatch expects those */
    if (k >= 'a' && k <= 'z') return (uint8_t)(k - 0x20u);
    if (k >= 0x20 && k < 0x80) return (uint8_t)k;   /* the printables     */
    return 0u;
}

/** @brief Does the original's scancode-to-code table (`202F:0x4260`) map this
 * make to a nonzero `[0x48A3]`?  Its zero entries are the modifier/system
 * keys plus Tab, backtick, F11 and F12; the level-triggered readers only
 * test the byte for nonzero.  The shipped game's keys are covered.
 * shortcut: the PC extended block's zero entries are not enumerated, add
 * them if a held-key consumer ever needs one.
 *
 * @param k the SDL keycode
 * @return 1 when the make sets `[0x48A3]` nonzero, 0 otherwise
 */
static int32_t _key_sets_held(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UNKNOWN:
    case SDLK_TAB:
    case SDLK_GRAVE:
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
    case SDLK_PRINTSCREEN:
    case SDLK_SYSREQ:
    case SDLK_F11:
    case SDLK_F12: return 0;
    default: return 1;
    }
}

void sdl3_events_poll(InputState *out)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT: _cur.quit = 1; break;
        case SDL_EVENT_KEY_DOWN:
            _joy_bits(ev.key.key, 1);
            _cur.lastKey = _key_code(ev.key.key);
            _cur.keyScan = _scan_code(ev.key.key);
            _cur.keyHeld = (uint8_t)_key_sets_held(ev.key.key);
            break;
        case SDL_EVENT_KEY_UP:
            /* the ISR's 942F: EVERY key-up clears the flag — a tapped
             * key must not linger as "held" (the stub-era code re-set
             * the code on release, which made one ENTER tap cascade:
             * abort the boot intro AND immediately escape the attract) */
            _joy_bits(ev.key.key, 0);
            _cur.lastKey = 0u;
            _cur.keyScan = 0u;
            _cur.keyHeld = 0u;
            break;
        default: break;
        }
    }
    *out = _cur;
    /* The ISR's [0x48A3] is rewritten by every make — the BIOS typematic
     * repeats included — and the front-end consumes it (AA4E/AA50); between
     * makes it holds.  The SDL queue's KEY_DOWN events (the OS's typematic
     * repeats included) are those makes: deliver each once in lastKey, then
     * clear it until the next make lands.  Without this a held key latches
     * and the name screen fills at tick rate instead of the typematic's
     * delay-then-repeat.  keyHeld is the other half of the same byte: the
     * level the boot/attract tests read, cleared only by the release. */
    _cur.lastKey = 0u;
    _cur.keyScan = 0u;
}

void sdl3_events_init(void) { memset(&_cur, 0, sizeof _cur); }
