/* osystem.h — the architecture abstraction for the HumansRE port.
 *
 * RULE (port_structure.md §1): the game core never touches SDL, the GPU, the
 * OS or a file path.  It calls OSystem.  If the core cannot express a
 * behaviour through this interface, the interface is wrong, not the core.
 *
 * One function per machine behaviour the DOS original actually used, not one
 * per DOS mechanism (port_structure.md §2):
 *
 *   DOS mechanism                          OSystem call
 *   the mode set (13h / mode X)            videoSetMode(mode)
 *   two 320x200 pages + CRTC flip          videoPage()/videoPresent()
 *   DAC palette (OUT 3C8h/3C9h)            videoSetPalette()
 *   keyboard/joystick ISRs                 eventsPoll()
 *   PIT + INT 8 ISR heartbeat              timerStart(rate, tick)
 *   INT 60h audio driver ABI               audioInstall/... (one per command)
 */
#ifndef HUMANSRE_OSYSTEM_H
#define HUMANSRE_OSYSTEM_H

#include <stdint.h>

/* The game's two input paths (re_formats.md §12, re_map.md input section):
 * the original feeds both from its own keyboard ISR; the backend translates
 * host events into the same shape. */
typedef struct InputState {
    uint8_t
        joyBits; /* +0x5ACE equivalent: bit0=up bit1=down bit2=left bit3=right, bit7=fire */
    uint8_t lastKey; /* +0x48A3's per-make code: the translated key of this
                        * poll's key-down event, 0 when none.  The event
                        * screens read-and-clear it (1000:AA4E/AA50). */
    uint8_t keyScan; /* the PC set-1 make scancode (the key list [0x1989]'s
                        * index; 0 = none).  lastKey is the translated ASCII
                        * code the text screens read — scan_id_list (1000:386E)
                        * indexes [0x42B7 + scancode] instead, so F1..F10 and
                        * the punctuation keys need this separate field. */
    uint8_t keyHeld; /* +0x48A3's held level: 1 while a key the ISR maps
                        * nonzero is physically down, 0 after any release.
                        * The level-triggered readers test this (b2ba/c0c6,
                        * attract_exit, cd_file_op); lastKey is the one-shot
                        * copy the event screens consume. */
    uint8_t quit; /* the backend-level quit (window closed, ^C)          */
} InputState;

/* The fixed-tick callback: everything the original's INT 8 ISR body did, in
 * the same order (the tick counter, the palette-fade step, the pending page
 * flip apply) happens here, once per tick. */
typedef void (*TickFn)(void);

/* The two VGA modes the game sets; the core keeps its 320x200x8 pages and
 * only says which field the CRTC would scan out, so the backend can present
 * that field (and nothing past it — the mode-X pages carry stale rows below
 * the 192nd):
 *   VIDEO_MODE_13H  320x200 — BIOS mode 13h (1000:88FB) and the attract's
 *                   vga_set_mode_13h (1000:8300): CRTC 0x12 = 0x8F,
 *                   0x13 = 0x28.
 *   VIDEO_MODE_X    320x192 — reset_display_mode (1000:82C6), the room /
 *                   party / level field: CRTC 0x12 = 0x7F, 0x13 = 0x2C
 *                   (the level's 40-row status bar ends on row 191, and
 *                   the DOSBox raw frames of those screens are 320x192).
 */
typedef enum VideoMode {
    VIDEO_MODE_13H = 0,
    VIDEO_MODE_X = 1
} VideoMode;

/* The tick-boundary gate: the arch owns the tick counter (the ISR's
 * [0x5AC8]) and tickSync is the foreground's [0x48BA] mechanism — it waits
 * until the tick source has advanced past *lastSeen, then stores the new
 * count there.  The foreground calls it once per loop iteration and runs
 * exactly one flow step per tick; the display refresh rate must never pace
 * it (the original's ISR re-arms the PIT one-shot and re-syncs to vsync, so
 * one tick = one display period at GAME_TICK_HZ, independent of the host). */
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
    TickSyncFn tickSync; /* the [0x5AC8]/[0x48BA] pair */

    /* --- audio ------------------------------------------------------- *
     * One member per INT 60h command the game sends (re_snd_drv.md §2.4,
     * port_structure.md §2d).  The module behind them is the resident
     * driver's replacement; the backend owns the sample stream and pulls
     * the module's generate().                                          */
    int32_t (*audioInstall)(void); /* the install entry (1E95:00DD)      */
    int32_t (*audioLoadTune)(const uint8_t *image, uint32_t size); /* 0x0C */
    void (*audioResume)(void);     /* cmd 5, CX = 0 (music_resume)       */
    void (*audioStop)(void);       /* cmd 6 (music_stop)                 */
    void (*audioSfx)(uint16_t id); /* cmd 5, CX = the event id           */
    void (*audioTempo)(uint8_t cl); /* cmd 0x0A (master volume)          */
    void (*audioTick)(void);       /* cmd 2 (the music tick)             */

    /* --- misc -------------------------------------------------------- */
    void (*fatalError)(const char *msg); /* does not return            */
} OSystem;

/* Implemented by each arch/<port>/; the arch layer's main() picks one and
 * hands it to game_main(). */
int32_t game_main(const OSystem *osys);

#endif /* HUMANSRE_OSYSTEM_H */
