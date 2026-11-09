/* script.h - the interface of src/script.c: the frame-keyed cue interpreter
 * The boot intro's event script: 25 verbatim cues from 202F:0x843C,
 * dispatched on the exact frame (JE) through a 7-command dispatch. */

#ifndef HUMANSRE_SCRIPT_H
#define HUMANSRE_SCRIPT_H

#include "osystem.h"

typedef struct {
    uint16_t frame; /* the current frame (the player advances it) */
    uint16_t groupDwell; /* cmd 7: frames per display-list group       */
    uint8_t dwellPending; /* cmd 7 fired: [0x6462] must reload        */
    uint8_t scrollOn; /* cmd 3 on / cmd 2 off                       */
    uint16_t scrollDelta; /* cmd 6: the scroll advance per frame        */
    uint16_t transitionIdx; /* cmd 1: run_screen_transition index         */
    uint8_t quit; /* cmd 5: the quit flag                       */
    uint8_t done; /* the frame target reached                   */
    uint8_t seekPending; /* cmd 4 fired: the player must seek          */
    uint16_t seekFrame; /*    ...to this frame                        */
    const uint16_t *cues; /* [frame][cmd][arg] triples, verbatim        */
    uint16_t cueCount;
    uint16_t cursor; /* the next cue to fire                       */
} CueScript;

/* --- src/script.c - interpreter skeleton -------------------------------- */

/**
 * @brief Bind the cue-script module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t script_init(const OSystem *osys);

/**
 * @brief Start the intro cue script (the verbatim 202F:0x843C template).
 *
 * The boot intro's verbatim cue list (25 cues, frame target 387).
 *
 * @param s the cue script
 */
void script_intro_start(CueScript *s);

/** @brief Fire every cue whose frame == the current frame (the game's JE dispatch -
 * a passed cue blocks the cursor until a seek rewinds the counter).  The
 * comparison operand is the LIVE counter [0x6466]: the seek handler rewrites
 * it mid-scan (b92d), and the loop continues (`JMP b80d`) against the NEW
 * value - a cue whose frame the seek jumped past does not fire in this
 * pass.
 *
 * @param s the cue script
 * @param frame the frame index
 */
void script_fire(CueScript *s, uint16_t frame);

/**
 * @brief Accessor: the script requests a quit.
 *
 * @param s the cue script
 * @return The script requests a quit.
 */
uint8_t script_quit(const CueScript *s);

/**
 * @brief Accessor: the cue script finished.
 *
 * @param s the cue script
 * @return The cue script finished.
 */
uint8_t script_done(const CueScript *s);

/**
 * @brief Accessor: the group dwell count (1000:B94E set [0x6460]).
 *
 * @param s the cue script
 * @return The group dwell count.
 */
uint16_t script_group_dwell(const CueScript *s);

/**
 * @brief Accessor: a dwell cue fired (its handler set [0x6460] and [0x6462]).
 *
 * @param s the cue script
 * @return A dwell cue fired since the last script_dwell_done().
 */
uint8_t script_dwell_pending(const CueScript *s);

/**
 * @brief Clear the script's dwell-pending flag.
 *
 * @param s the cue script
 */
void script_dwell_done(CueScript *s);

/**
 * @brief Accessor: the scroll gate.
 *
 * @param s the cue script
 * @return The scroll gate.
 */
uint8_t script_scroll_on(const CueScript *s);

/**
 * @brief Accessor: the scroll delta.
 *
 * @param s the cue script
 * @return The scroll delta.
 */
uint16_t script_scroll_delta(const CueScript *s);

/**
 * @brief Accessor: the script's transition index.
 *
 * @param s the cue script
 * @return The script's transition index.
 */
uint16_t script_transition_index(const CueScript *s);

/**
 * @brief Accessor: a seek is pending.
 *
 * @param s the cue script
 * @return A seek is pending.
 */
uint8_t script_seek_pending(const CueScript *s);

/**
 * @brief Accessor: the seek frame.
 *
 * @param s the cue script
 * @return The seek frame.
 */
uint16_t script_seek_frame(const CueScript *s);

/**
 * @brief Clear the script's seek-pending flag.
 *
 * @param s the cue script
 */
void script_seek_done(CueScript *s);

#endif /* HUMANSRE_SCRIPT_H */
