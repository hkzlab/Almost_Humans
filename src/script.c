#include "script.h"
#include "osystem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCRIPT_FRAME_TARGET 387u   // the live frame counter ran 0..386

/* Cue commands (stored directly in the cue record, 1..7) */
#define CUE_TRANSITION 1
#define CUE_SCROLL_OFF 2
#define CUE_SCROLL_ON  3
#define CUE_SEEK       4
#define CUE_QUIT       5
#define CUE_SCROLL     6
#define CUE_DWELL      7

/* --- intro cue list (verbatim from 202F:0x843C) ------------------------- */
static const uint16_t _intro_cues[25u * 3u] = {
    /* frame  cmd  arg */
    0,   7, 1, // dwell 1
    0,   6, 3, // scroll delta 3
    0,   3, 0, // scroll on
    0,   4, 0, // seek 0
    0,   1, 0, // transition 0
    64,  2, 0, // scroll off
    65,  1, 1, // transition 1
    76,  3, 0, // scroll on
    170, 2, 0, // scroll off
    171, 1, 2, // transition 2
    186, 3, 0, // scroll on
    186, 6, 4, // scroll delta 4
    245, 2, 0, // scroll off
    246, 1, 3, // transition 3
    261, 3, 0, // scroll on
    261, 6, 8, // scroll delta 8
    261, 7, 1, // dwell 1
    294, 2, 0, // scroll off
    295, 1, 4, // transition 4
    307, 3, 0, // scroll on
    323, 4, 314, // seek to frame 314 (the tail loop)
    323, 4, 314, // seek to frame 314 again
    325, 2, 0, // scroll off
    355, 1, 5, // transition 5
    385, 5, 0, // quit
};

int32_t script_init(void) { return 0; }

/**
 * @brief Wire a cue list (flattened [frame][cmd][arg] u16 triples).
 *
 * @param s the cue script
 * @param cues the cue script array
 * @param cueCount the cue count
 */
void script_start(CueScript *s, const uint16_t *cues, uint16_t cueCount)
{
    memset(s, 0, sizeof *s);
    s->cues = cues;
    s->cueCount = cueCount;
    s->groupDwell = 1u;              // the dwell cue at frame 0 sets it
}

void script_fire(CueScript *s, uint16_t frame)
{
    s->frame = frame;

    while (s->cursor < s->cueCount) {
        uint16_t f = s->cues[s->cursor * 3u];
        uint16_t cmd = s->cues[s->cursor * 3u + 1u];
        uint16_t arg = s->cues[s->cursor * 3u + 2u];
        if (f != s->frame) break;               // JE vs the LIVE counter

        s->cursor++;
        switch (cmd) {
        case CUE_TRANSITION: s->transitionIdx = arg; break;
        case CUE_SCROLL_OFF: s->scrollOn = 0; break;
        case CUE_SCROLL_ON: s->scrollOn = 1; break;
        case CUE_SEEK:
            s->seekFrame = arg;
            s->seekPending = 1;
            s->frame = arg; // b92d: [0x6466] = arg, the scan continues
            break;
        case CUE_QUIT: s->quit = 1; break;
        case CUE_SCROLL: s->scrollDelta = arg; break;
        case CUE_DWELL:
            s->groupDwell = arg;
            s->dwellPending = 1; // b94e: [0x6460] = [0x6462] = arg
            break;
        }
        if (s->quit) return;
    }
    if (s->cursor >= s->cueCount && s->frame + 1 >= SCRIPT_FRAME_TARGET)
        s->done = 1;
}

uint8_t script_quit(const CueScript *s) { return s->quit; }

uint8_t script_done(const CueScript *s) { return s->done; }

uint16_t script_group_dwell(const CueScript *s) { return s->groupDwell; }

uint8_t script_dwell_pending(const CueScript *s) { return s->dwellPending; }

void script_dwell_done(CueScript *s) { s->dwellPending = 0; }

uint8_t script_scroll_on(const CueScript *s) { return s->scrollOn; }

uint16_t script_scroll_delta(const CueScript *s) { return s->scrollDelta; }

uint16_t script_transition_index(const CueScript *s)
{ return s->transitionIdx; }

uint8_t script_seek_pending(const CueScript *s) { return s->seekPending; }

uint16_t script_seek_frame(const CueScript *s) { return s->seekFrame; }

void script_seek_done(CueScript *s) { s->seekPending = 0; }

void script_intro_start(CueScript *s) { script_start(s, _intro_cues, 25u); }
