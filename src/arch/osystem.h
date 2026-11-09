/* osystem.h - the architecture abstraction for the HumansRE port.
 *
 * The game core never touches SDL, the GPU, the OS or a file path: 
 * It calls OSystem.
 * If the core cannot express a behaviour through this interface,
 * the interface is wrong, not the core.
 *
 */
#ifndef HUMANSRE_OSYSTEM_H
#define HUMANSRE_OSYSTEM_H

#include "fs/fs.h"

#include <stdint.h>

/* One poll's input: the two bytes the game's foreground reads, plus the
 * host's quit request (re_formats.md §12, re_map.md "Two input channels").
 *
 *   [0x5ACE]  the control bits.  poll_input (1000:8526) rebuilds them every
 *             frame: read_input_state (1000:85BD) clears the byte and loads
 *             the joystick state when the stick is enabled ([0x5B00]); then
 *             the five bound keys [0x5B05..01] are ORed in.  The port calls
 *             the result `joyBits` and keeps it as a level.
 *   [0x48A3]  the translated key code.  The INT 9 ISR (1000:93A1) writes it
 *             on every key event and 0 on a release, so nonzero means "a key
 *             is held at this instant".  The port splits it into the one-shot
 *             make fields `lastKey`/`keyScan` and the level `keyHeld`.
 *
 * `lastKey`/`keyScan` describe only the make this poll delivers (the last key
 * event wins; a release leaves 0).  `joyBits`, `keyHeld` and `quit` stay set
 * while the condition holds. */
typedef struct InputState {
    uint8_t joyBits; /* [0x5ACE] control level: bit0 up, bit1 down, bit2 left,
                      * bit3 right, bit7 action/fire.  The arrows and the
                      * bound action key (Z in the shipped config) set it; a
                      * real stick ORs into the same bits. */
    uint8_t lastKey; /* this poll's make as the ISR's translated code:
                      * uppercase letters; ESC 0x1B, Enter 0x0D, Backspace
                      * 0x0E; 0 when no make is delivered or the make's code
                      * is 0 (modifiers, E0-extended keys).  One-shot: the
                      * event screens read and clear it (1000:AA4E/1000:AA50). */
    uint8_t keyScan; /* PC set-1 scancode of this poll's make, for the keys
                      * the backend maps by scancode: ESC 0x01, ',' 0x33,
                      * '.' 0x34, SPACE 0x39, F1..F10 0x3B..0x44 (the key
                      * list at DS:0x1989 binds all but F9); 0 for every
                      * other make.  The list is matched by scancode
                      * (scan_id_list 1000:386E), so lastKey cannot stand in
                      * for it.  One-shot. */
    uint8_t keyHeld; /* [0x48A3]'s level form: 1 while the key down now
                      * translates nonzero, 0 after any release (1000:942F
                      * clears it even if another key is still down).  The
                      * readers that test "any key" without consuming make
                      * use it (1000:B2BA, 1000:C0C6, attract_exit,
                      * cd_file_op). */
    uint8_t quit;    /* host quit request (window close, ^C, --ticks); sticky,
                      * not a game input; the flow loop reads input.quit. */
} InputState;

/* The fixed-tick callback: everything the original's INT 8 ISR body did, in
 * the same order (the tick counter, the palette-fade step, the pending page
 * flip apply) happens here, once per tick. */
typedef void (*TickFn)(void);

/* The two VGA modes the game sets; the core keeps its 320x200x8 pages and
 * only says which field the CRTC would scan out, so the backend can present
 * that field (and nothing past it as the mode-X pages carry stale rows below
 * the 192nd):
 *   VIDEO_MODE_13H  320x200 -> BIOS mode 13h (1000:88FB) and the attract's
 *                   vga_set_mode_13h (1000:8300): CRTC 0x12 = 0x8F,
 *                   0x13 = 0x28.
 *   VIDEO_MODE_X    320x192 -> reset_display_mode (1000:82C6), the room /
 *                   party / level field: CRTC 0x12 = 0x7F, 0x13 = 0x2C
 *                   (the level's 40-row status bar ends on row 191, and
 *                   the DOSBox raw frames of those screens are 320x192).
 */
typedef enum VideoMode {
    VIDEO_MODE_13H = 0,
    VIDEO_MODE_X = 1
} VideoMode;

/* The page videoPage() hands out: flat 8bpp, one byte per pixel.  The field a
 * mode scans out may be shorter (mode X); the page layout stays 320x200.
 * Stride == width because the page is 8bpp. */
#define VIDEO_PAGE_W         320u
#define VIDEO_PAGE_H_MODE13H 200u
#define VIDEO_PAGE_H_MODEX   192u
#define VIDEO_PAGE_BYTES     (VIDEO_PAGE_W * VIDEO_PAGE_H_MODE13H)

/* The tick-boundary gate, the core's only pacing source.
 *
 * The arch owns the tick counter (the port's [0x5AC8]) and advances it once
 * per tick at the point the original ISR does (1000:8AA4, before the tick
 * body's later work).  *lastSeen is in/out: tickSync blocks until the
 * counter differs from *lastSeen, then writes the observed count back.  Any
 * difference counts, not only an increase (the counter wraps), and a tick
 * that already landed since the previous call returns at once.
 *
 * This is the foreground's snapshot-and-compare spin on [0x5AC8],
 * not the [0x48BA] vsync-abort flag and not a display wait:
 * the host display refresh must never pace the game.
 * timerStart installs the tick body; tickSync only observes its
 * counter, which may be advanced on another thread, so the gate must be
 * safe to call from the foreground.
 * 
 * Call it once per foreground iteration, before that iteration's
 * flow step — one flow step per tick boundary. */
typedef void (*TickSyncFn)(uint32_t *lastSeen);

typedef struct OSystem {
    /* --- video ------------------------------------------------------- */
    void (*videoSetMode)(VideoMode mode); /* every mode set (13h/X)    */
    uint8_t *(*videoPage)(int32_t which); /* which: 0 or 1; 320x200x8   */
    void (*videoPresent)(int32_t which); /* the CRTC-flip equivalent   */
    void (*videoSetPalette)(const uint8_t *p6, uint32_t first, uint32_t n);

    /* --- input ------------------------------------------------------- */
    void (*eventsPoll)(InputState *out);

    /* --- timing ------------------------------------------------------ */
    void (*timerStart)(uint32_t ticksPerSec, TickFn tick);
    TickSyncFn tickSync; /* the [0x5AC8] counter gate, not [0x48BA] */

    /* --- audio ------------------------------------------------------- *
     * One member per INT 60h command the game sends (re_snd_drv.md §2.4,
     * port_structure.md §2d).  The module behind them is the resident
     * driver's replacement; the backend owns the sample stream and pulls
     * the module's generate().                                          */
    int32_t (*audioInstall)(void); /* the install entry (1E95:00DD)      */
    int32_t (*audioLoadTune)(const uint8_t *image, uint32_t size); /* 0x0C */
    void (*audioResume)(void); /* cmd 5, CX = 0 (music_resume)       */
    void (*audioStop)(void); /* cmd 6 (music_stop)                 */
    void (*audioSfx)(uint16_t id); /* cmd 5, CX = the event id           */
    void (*audioTempo)(uint8_t cl); /* cmd 0x0A (master volume)          */
    void (*audioTick)(void); /* cmd 2 (the music tick)             */
    /* [0x5AFE] sound_driver_id: -1 none, 0 Beeper, 1 AdLib, other MT-32.
     * play_tune chooses the tune extension and driver_call its gate from it
     * (1000:83CC, 1000:8462), exactly as the original's launcher global. */
    int32_t (*audioDriverId)(void);

    /* --- misc -------------------------------------------------------- */
    /* A non-fatal diagnostic: the core formats the message, the backend owns
     * the destination (SDL log, stderr, nothing).  Not a DOS behaviour — the
     * original had no logging — but the core must not know a stream either. */
    void (*warn)(const char *msg);
    void (*fatalError)(const char *msg); /* does not return            */

    /* The filesystem module the core reads the volumes and saves through
     * It is defined by the arch layer. */
    const FSModule *fs;
} OSystem;

/**
 * @brief The application entry: boot, then the foreground flow loop.
 *
 * Implemented by each arch/<port>/; the arch layer's main() picks one and
 * hands it to game_main().
 *
 * @param osys the backend interface
 * @return 0 when the quit request ends the loop
 */
int32_t game_main(const OSystem *osys);

#endif /* HUMANSRE_OSYSTEM_H */
