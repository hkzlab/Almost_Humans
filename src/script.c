/* script.c — frame-keyed cue interpreter (re_scripts.md §2):
 * one cursor + a 7-command dispatch + per-frame fire, shared by the boot
 * intro's display-list player, the entity bytecode, and the music-event
 * scripts.
 *
 * The intro cue list is ported VERBATIM from the static template at
 * 202F:0x843C (file 0x18B2C in the unpacked exe): 25 records
 * [u16 frame][u16 cmd][u16 arg], the cmd stored DIRECTLY, 1-based (the
 * docs' "cmd-1" encoding was wrong — the DEC BX at b816 is the dispatch
 * table's own 0-basing; see re_formats.md §8.7a).
 *
 * Dispatch semantics (the game's interpreter, 1000:B809): a cue fires when
 * its frame EQUALS the current frame (JE) — a cue whose frame was passed
 * never fires and blocks the cursor until a seek rewinds the counter.  The
 * seek handler rewrites the counter MID-SCAN (b92d) and the loop continues
 * against the new value, so after a seek the remaining cues compare against
 * the seek's target, not the frame the call started with.  The player owns
 * the counter and passes it to script_fire().
 */
#include "script.h"
#include "osystem.h"

#include <stddef.h>
#include <string.h>

#define SCRIPT_FRAME_TARGET 387u   /* the live frame counter ran 0..386 */

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
    0,   7, 1, /* dwell 1                                        */
    0,   6, 3, /* scroll delta 3                                 */
    0,   3, 0, /* scroll on                                      */
    0,   4, 0, /* seek 0                                         */
    0,   1, 0, /* transition 0                                   */
    64,  2, 0, /* scroll off                                     */
    65,  1, 1, /* transition 1                                   */
    76,  3, 0, /* scroll on                                      */
    170, 2, 0, /* scroll off                                     */
    171, 1, 2, /* transition 2                                   */
    186, 3, 0, /* scroll on                                      */
    186, 6, 4, /* scroll delta 4                                 */
    245, 2, 0, /* scroll off                                     */
    246, 1, 3, /* transition 3                                   */
    261, 3, 0, /* scroll on                                      */
    261, 6, 8, /* scroll delta 8                                 */
    261, 7, 1, /* dwell 1                                        */
    294, 2, 0, /* scroll off                                     */
    295, 1, 4, /* transition 4                                   */
    307, 3, 0, /* scroll on                                      */
    323, 4, 314, /* seek to frame 314 (the tail loop)              */
    323, 4, 314, /* seek to frame 314 again                        */
    325, 2, 0, /* scroll off                                     */
    355, 1, 5, /* transition 5                                   */
    385, 5, 0, /* quit                                           */
};

int32_t script_init(const OSystem *osys)
{
    (void)osys;
    return 0;
}

/* Wire a cue list (flattened [frame][cmd][arg] u16 triples). */
void script_start(CueScript *s, const uint16_t *cues, uint32_t cueCount)
{
    memset(s, 0, sizeof *s);
    s->cues = cues;
    s->cueCount = cueCount;
    s->groupDwell = 1u;              /* the dwell cue at frame 0 sets it   */
}

/* Fire every cue whose frame == the current frame (the game's JE dispatch —
 * a passed cue blocks the cursor until a seek rewinds the counter).  The
 * comparison operand is the LIVE counter [0x6466]: the seek handler rewrites
 * it mid-scan (b92d), and the loop continues (`JMP b80d`) against the NEW
 * value — a cue whose frame the seek jumped past does not fire in this
 * pass. */
void script_fire(CueScript *s, uint32_t frame)
{
    s->frame = frame;

    while (s->cursor < s->cueCount) {
        uint16_t f = s->cues[s->cursor * 3u];
        uint16_t cmd = s->cues[s->cursor * 3u + 1u];
        uint16_t arg = s->cues[s->cursor * 3u + 2u];
        if (f != s->frame) break;               /* JE vs the LIVE counter */

        s->cursor++;
        switch (cmd) {
        case CUE_TRANSITION: s->transitionIdx = arg; break;
        case CUE_SCROLL_OFF: s->scrollOn = 0; break;
        case CUE_SCROLL_ON: s->scrollOn = 1; break;
        case CUE_SEEK:
            s->seekFrame = arg;
            s->seekPending = 1;
            s->frame = arg; /* b92d: [0x6466] = arg, the scan continues */
            break;
        case CUE_QUIT: s->quit = 1; break;
        case CUE_SCROLL: s->scrollDelta = arg; break;
        case CUE_DWELL: s->groupDwell = arg; break;
        }
        if (s->quit) return;
    }
    if (s->cursor >= s->cueCount && s->frame + 1 >= SCRIPT_FRAME_TARGET)
        s->done = 1;
}

int32_t script_quit(const CueScript *s) { return s->quit; }
int32_t script_done(const CueScript *s) { return s->done; }
uint32_t script_group_dwell(const CueScript *s) { return s->groupDwell; }
int32_t script_scroll_on(const CueScript *s) { return s->scrollOn; }
uint32_t script_scroll_delta(const CueScript *s) { return s->scrollDelta; }
uint32_t script_transition_index(const CueScript *s)
{ return s->transitionIdx; }
int32_t script_seek_pending(const CueScript *s) { return s->seekPending; }
uint32_t script_seek_frame(const CueScript *s) { return s->seekFrame; }
void script_seek_done(CueScript *s) { s->seekPending = 0; }

/* Start the intro cue script (the verbatim 202F:0x843C template). */
void script_intro_start(CueScript *s) { script_start(s, _intro_cues, 25u); }
