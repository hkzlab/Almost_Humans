/* Host events -> the game's two input paths ([0x5ACE] bits, [0x48A3] key).
 *
 * Arrows + the configured action key (default Z, [0x5B05]) set the joystick
 * bits; printable keys pass as ASCII; ESC/Enter/Backspace use the INT 9 codes
 * (poll_input 1000:8526, int9_keyboard_isr 1000:93A1).  A key-up clears the
 * code.
 */
#include "osystem.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>

static InputState _cur;

/**
 * @brief Map a key event onto the joystick bit word.
 *
 * @param k the SDL keycode
 * @param down the key-down flag
 */
static void _set_joy_bits(SDL_Keycode k, uint8_t down)
{
    uint32_t mask;
    switch (k) {
    case SDLK_UP: mask = 0x01u; break;
    case SDLK_DOWN: mask = 0x02u; break;
    case SDLK_LEFT: mask = 0x04u; break;
    case SDLK_RIGHT: mask = 0x08u; break;
    case SDLK_Z:
        /* The configured action key ([0x5B05] = scancode 0x2C = Z).  Only
         * this key sets the action bit: an Enter mapping here made the
         * attract's action-bit escape fire on Enter. */
        mask = 0x80u;
        break;
    default: return;
    }
    if (down) _cur.joyBits |= mask;
    else _cur.joyBits &= (uint8_t)~mask;
}

/** @brief The PC set-1 scancode of a make, for the keys the level's key list
 * (DS:0x1989, scan_id_list 1000:386E) dispatches on: ESC, ',', '.', SPACE,
 * F1..F8 and F10.  F9 is mapped too (0x43) but has no list entry; every
 * other key leaves 0, and the port's other screens read the translated code
 * in lastKey.
 *
 * @param k the SDL keycode
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
 * @param k the SDL keycode
 * @return the DOS key code, or 0 when unhandled
 */
static uint8_t _key_code(SDL_Keycode k)
{
    switch (k) {
    case SDLK_ESCAPE: return 0x1Bu;   // ESC - the 0x5F11 table's key
    case SDLK_BACKSPACE: return 0x0Eu;   // the erase action's key
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 0x0Du;   // the confirm action's key
    default: break;
    }
    /* the ISR's XLAT table (0x4260) delivers letters UPPERCASE ('a' -> 0x41,
     * input row) - the name screen's dispatch expects those */
    if (k >= 'a' && k <= 'z') return (uint8_t)(k - 0x20u);
    if (k >= 0x20 && k < 0x80) return (uint8_t)k;   // the printables
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
static uint8_t _key_sets_held(SDL_Keycode k)
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
            _set_joy_bits(ev.key.key, 1);
            _cur.lastKey = _key_code(ev.key.key);
            _cur.keyScan = _scan_code(ev.key.key);
            _cur.keyHeld = _key_sets_held(ev.key.key);
            break;
        case SDL_EVENT_KEY_UP:
            /* the ISR's 942F: EVERY key-up clears the flag - a tapped
             * key must not linger as "held" (the stub-era code re-set
             * the code on release, which made one ENTER tap cascade:
             * abort the boot intro AND immediately escape the attract) */
            _set_joy_bits(ev.key.key, 0);
            _cur.lastKey = 0u;
            _cur.keyScan = 0u;
            _cur.keyHeld = 0u;
            break;
        default: break;
        }
    }
    *out = _cur;
    /* A make is delivered once, then cleared until the next one: the ISR
     * rewrites [0x48A3] on every typematic repeat, so a held key must not
     * refill the name screen at tick rate.  keyHeld carries the level the
     * boot/attract tests read; every release clears it (1000:942F). */
    _cur.lastKey = 0u;
    _cur.keyScan = 0u;
}

void sdl3_events_init(void) { memset(&_cur, 0, sizeof _cur); }
