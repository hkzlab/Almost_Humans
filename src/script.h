/* script.h — the interface of src/script.c: the frame-keyed cue interpreter (§8.7a)
 * The boot intro's event script: 25 verbatim cues from 202F:0x843C,
 * dispatched on the exact frame (JE) through a 7-command dispatch. */

#ifndef HUMANSRE_SCRIPT_H
#define HUMANSRE_SCRIPT_H

#include "osystem.h"

typedef struct {
    uint32_t frame; /* the current frame (the player advances it) */
    uint32_t groupDwell; /* cmd 7: frames per display-list group       */
    int32_t scrollOn; /* cmd 3 on / cmd 2 off                       */
    uint32_t scrollDelta; /* cmd 6: the scroll advance per frame        */
    uint32_t transitionIdx; /* cmd 1: run_screen_transition index         */
    int32_t quit; /* cmd 5: the quit flag                       */
    int32_t done; /* the frame target reached                   */
    int32_t seekPending; /* cmd 4 fired: the player must seek          */
    uint32_t seekFrame; /*    ...to this frame                        */
    const uint16_t *cues; /* [frame][cmd][arg] triples, verbatim        */
    uint32_t cueCount;
    uint32_t cursor; /* the next cue to fire                       */
} CueScript;

/* --- src/script.c — interpreter skeleton (re_scripts.md) -------------- */
int32_t script_init(const OSystem *osys);

/* The boot intro's verbatim cue list (25 cues, frame target 387). */
void script_introStart(CueScript *s);
/* Fire every cue whose frame == `frame` (JE dispatch). */
void script_fire(CueScript *s, uint32_t frame);
int32_t script_quit(const CueScript *s);
int32_t script_done(const CueScript *s);
uint32_t script_groupDwell(const CueScript *s);
int32_t script_scrollOn(const CueScript *s);
uint32_t script_scrollDelta(const CueScript *s);
uint32_t script_transitionIndex(const CueScript *s);
int32_t script_seekPending(const CueScript *s);
uint32_t script_seekFrame(const CueScript *s);
void script_seekDone(CueScript *s);

#endif /* HUMANSRE_SCRIPT_H */
