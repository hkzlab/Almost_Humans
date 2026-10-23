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
 *   mode 13h -> unchained mode-X           videoSetMode()
 *   two 320x200 pages + CRTC flip          videoPage()/videoPresent()
 *   DAC palette (OUT 3C8h/3C9h)            videoSetPalette()
 *   keyboard/joystick ISRs                 eventsPoll()
 *   PIT + INT 8 ISR heartbeat              timerStart(rate, tick)
 *   INT 60h audio driver ABI               audioStart/audioPlayTune/audioStop
 */
#ifndef HUMANSRE_OSYSTEM_H
#define HUMANSRE_OSYSTEM_H

#include <stdint.h>

/* The game's two input paths (re_formats.md §12, re_map.md input section):
 * the original feeds both from its own keyboard ISR; the backend translates
 * host events into the same shape. */
typedef struct InputState {
    uint8_t joyBits;   /* +0x5ACE equivalent: bit0=up bit1=down bit2=left bit3=right, bit7=fire */
    uint8_t lastKey;   /* +0x48A3 equivalent: the last key code, 0 when none  */
    uint8_t quit;      /* the backend-level quit (window closed, ^C)          */
} InputState;

/* The fixed-tick callback: everything the original's INT 8 ISR body did, in
 * the same order (the tick counter, the palette-fade step, the pending page
 * flip apply) happens here, once per tick. */
typedef void (*TickFn)(void);

/* The tick-boundary gate: the arch owns the tick counter (the ISR's
 * [0x5AC8]) and tickSync is the foreground's [0x48BA] mechanism — it waits
 * until the tick source has advanced past *lastSeen, then stores the new
 * count there.  The foreground calls it once per loop iteration and runs
 * exactly one flow step per tick; the display refresh rate must never pace
 * it (the original's ISR re-arms the PIT one-shot and re-syncs to vsync, so
 * one tick = one display period at GAME_TICK_HZ, independent of the host). */
typedef void (*TickSyncFn)(unsigned *lastSeen);

typedef struct OSystem {
    /* --- video ------------------------------------------------------- */
    void     (*videoSetMode)(void);                 /* once at boot               */
    uint8_t *(*videoPage)(int which);               /* which: 0 or 1; 320x200x8   */
    void     (*videoPresent)(int which);            /* the CRTC-flip equivalent   */
    void     (*videoSetPalette)(const uint8_t *p6, unsigned first, unsigned n);

    /* --- input ------------------------------------------------------- */
    void     (*eventsPoll)(InputState *out);

    /* --- timing ------------------------------------------------------ */
    void     (*timerStart)(unsigned ticksPerSec, TickFn tick);
    TickSyncFn tickSync;                            /* the [0x5AC8]/[0x48BA] pair */

    /* --- audio ------------------------------------------------------- */
    int      (*audioStart)(void);                   /* 0 = ok                     */
    int      (*audioPlayTune)(unsigned id);
    void     (*audioStop)(void);

    /* --- misc -------------------------------------------------------- */
    void     (*fatalError)(const char *msg);        /* does not return            */
} OSystem;

/* Implemented by each arch/<port>/; the arch layer's main() picks one and
 * hands it to game_main(). */
int game_main(const OSystem *osys);

#endif /* HUMANSRE_OSYSTEM_H */
