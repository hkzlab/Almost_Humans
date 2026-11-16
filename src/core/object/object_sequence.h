/* The claim animation and the ACTION program machine.
 */

#ifndef HUMANSRE_OBJECT_SEQUENCE_H
#define HUMANSRE_OBJECT_SEQUENCE_H

#include <stdint.h>

/* The adjacent-object op of the entity bytecode (1000:3A6A): the gate and the
 * scan around the *acting* record (`actor`, the entity's own slot), then the
 * red/white flash, the blinks, the animation, the append and the settle.
 * `recipe` = 1/0/3 for the three handlers.  object_core_tick drives the sequence
 * and supplies the level's palette (the fades return to it). */


/** @brief The program header `1000:98F8` resolves for a raw id: the six word
 * pointers at `202F:0x5B2B` select the overlapping runs at 0x5B37..0x5B53,
 * read as {count, cursor, icon...}.
 *
 * An id outside 1..6 is bounded (D-186).
 *
 * @param group the raw program id
 * @return the 12-byte header, or NULL outside 1..6
 */
const uint8_t *object_sequence_program_header(uint32_t group);


/**
 * @brief Accessor: the action program header.
 *
 * @return the action program header, or NULL outside 1..6
 */
const uint8_t *object_sequence_action_header(void);


/** @brief One byte of the acting program's header sequence, `prog[k]` (one header
 * byte, 0 past the region).
 *
 * @param k the byte offset
 * @return the byte, or 0 outside the region
 */
uint8_t object_sequence_action_byte(uint32_t k);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/** @brief The resolved header pointer (SI) for the current raw id (image address, 0
 * outside 1..6).
 *
 * @return SI, or 0 outside 1..6
 */
uint32_t object_sequence_action_program_si(void);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 1000:3933's tail (0x3941-0x394D): advance the selected cell with the wrap
 * to 1 past the count. It is the program's cursor byte, so the next ACTION
 * press runs the op the bar's highlight shows (1000:3941: SPACE steps the
 * cursor).
 */
void object_sequence_step_action_cell(void);


#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief The throw's 0x5BA4 word for one type (the test seam).
 *
 * @param type the type
 * @return the per-type dx value, or 0 when out of range
 */
int32_t object_sequence_throw_dx_for_probe(uint32_t type);
#endif /* HUMANSRE_TEST_PROBES */


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/** @brief the 0x6EB6 read window, for the tests (D-160's closure): behaviour -> the
 * state byte `test_class_against_6eb6` compares against (DS:0x6EB6 window).
 *
 * @param beh the behaviour byte
 * @return the behaviour state
 */
uint8_t object_sequence_beh_state_for_probe(uint32_t beh);
#endif /* HUMANSRE_TEST_PROBES */


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/** @brief Test seam: 1000:F2E0's per-candidate dissolve + conversion.
 *
 * @param actor the object record
 */
void object_sequence_find_overlap_for_probe(uint8_t *actor);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief Begin the claim animation for one recipe.
 *
 * @param recipe the recipe index
 * @param actor the object record
 * @return the claim record index, or -1 when it cannot start
 */
int32_t object_sequence_claim_begin(uint32_t recipe, int32_t actor);


/**
 * @brief Accessor: the claim is running.
 *
 * @return 1 while the claim runs, 0 otherwise
 */
uint8_t object_sequence_claim_active(void);


/**
 * @brief Accessor: the claim's pan phase (the 1000:3D04 recolour frames).
 *
 * @return 1 during the pan, 0 otherwise
 */
uint8_t object_sequence_claim_pan_active(void);


/**
 * @brief Accessor: the claim's animation phase (1000:3BD6, the 56 frames).
 *
 * @return 1 while the claim is animating the neighbour.
 */
uint8_t object_sequence_claim_anim_active(void);


/**
 * @brief Accessor: the claim's page re-blit phase (1000:3CB5's re-blits).
 *
 * @return 1 while the world must be re-composed.
 */
uint8_t object_sequence_claim_flip_active(void);


/**
 * @brief Accessor: the claim has reached the pan and its page is frozen.
 *
 * The three phases before the pan (`CLAIM_TO_RED`/`RED_BACK`/`TO_WHITE`) only
 * fade the DAC in the original, so the last drawn page must be left alone
 * (1000:3B34..1000:3B8E); from the pan onward the recoloured page is presented.
 *
 * @return 1 from CLAIM_WHITE_BACK through CLAIM_SETTLE.
 */
uint8_t object_sequence_claim_page_frozen(void);


/**
 * @brief Accessor: the claim's settle fade (1000:3CDE).
 *
 * The page is already the final `1000:3CB5` frame (world + objects); the
 * settle only fades the DAC, so it must not be presented over the plane.
 *
 * 1 during CLAIM_SETTLE: the page is the final 0x3CB5 frame, leave it.
 *
 * @return 1 while the settle fade runs.
 */
uint8_t object_sequence_claim_settle_active(void);


/**
 * @brief The scan at 1000:3A91..1000:3AD9 / 4E91, for the acting record.
 *
 * The first record beside `actor` (|dx| <= 0x20, |dy| <= 8, type <= 2, active),
 * or -1.
 *
 * @param actor the object record
 * @return the record index, or -1 when none
 */
int32_t object_sequence_find_neighbour(int32_t actor);


/**
 * @brief script_op_dispatch: one ACTION press.
 *
 * The ACTION program machine (script_op_dispatch, 1000:3979): ACTION with no
 * direction held runs the acting record's program.
 * The acting record is [0x3E22]; object_sequence_action_run() performs one press,
 * object_sequence_cycle_entity(+/-1) is the object selector (1000:410A/4147).
 */
void object_sequence_action_run(void);


/** @brief The pending-event dispatch (`[0x1979]` -> the table at CS:0x197B).  The port
 * only needs the two gameplay codes: 4 is the jump/throw charge loop and 5 is
 * the program-cursor rewind; 1/2/3/6 are the screen-level dispatch control
 * (lock, "return 0", a bare RET, spin-wait), none of which the port's tick
 * drives.
 */
void object_sequence_pending_run(void);


/**
 * @brief One frame of the event-4 charge loop (1000:3702).
 *
 * Call after the world frame.
 *
 * @return 1 while the charge loop continues, 0 when it ends
 */
uint8_t object_sequence_charge_tick(void);


/** @brief Accessor: the charge loop is running (1000:3702).
 *
 * The status bar must not repaint the band/action row while it is (D-81).
 *
 * @return 1 while the charge loop is active, 0 otherwise
 */
uint8_t object_sequence_charge_active(void);


/**
 * @brief 0x410A (resolve_next_eligible_object_sprite): cycle the 8 static records.
 *
 * @param step the step
 * @return the new index, or -1 when none is eligible
 */
int32_t object_sequence_cycle_entity(int32_t step);


/** @brief 1000:38C3 (resolve_object_sprite_guarded): the key list's F1..F8 handler.
 * AX is the scancode and the record is `scancode - 0x3B`; a slot with `+6`
 * bit 7 set or type 0x1D is skipped, otherwise 1000:4147 resolves it.
 * Returns 1 when a record was selected.
 *
 * @param index the index
 * @return 1 when the record was selected, 0 otherwise
 */
uint8_t object_sequence_select_by_index(uint32_t index);


/** @brief 1000:3933 (the key list's id 8, SPACE): convert or step the cell.
 *
 * 1000:392D runs the home-state conversion on the acting record
 * (get_object_ptr + 0xEDB7) and returns when it converted; otherwise it plays
 * driver effect 0 and steps the action row's selected cell (1000:3941).
 *
 * @return 1 (the make is consumed)
 */
uint8_t object_sequence_space_action(void);


/**
 * @brief Accessor: the acting record's index.
 *
 * @return the index
 */
uint16_t object_sequence_action_entity(void);


/** @brief 1000:3343..336C (level_fade_in): slot 8 (`0x3F05`) takes frame 0x17, type 0x1A,
 * +0xA = 0x1006 and +0xC = 0xFF.  `scan_object_rows` starts its walk there and
 * counts `[0x3E73]-8+1` records, so this slot is drawn.
 * Slot 8 is the active-human ARROW: 1000:D64F (object_sequence_marker_tick, called from
 * update_world) positions and bobs it every frame, and the selector arms it in
 * 1000:4188 (_obj_sequence_marker_arm), so the x/y the array init leaves at 0xFFFF are
 * replaced on the first level frame.
 */
void object_sequence_level_start_record(void);


/** @brief 1000:D64F (update_world's 1000:80C9): the active-human marker.
 *
 * Record 8 follows the acting record: its type word is subtracted from y and
 * steps 0x1A,0x18,0x16,0x14,0x12 (two every third frame - the bob); a 32-px
 * acting sprite shifts x by +8; the word at +0xA is the lifetime, and when its
 * high byte underflows the marker is disabled (+6 = 0x80) and scan_object_rows
 * stops drawing it.
 */
void object_sequence_marker_tick(void);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Set the acting record.
 *
 * @param index the acting record's index
 */
void object_sequence_set_action_entity(uint16_t index);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief Accessor: the action program index ([0x5B56]).
 *
 * @return the program id
 */
uint8_t object_sequence_action_program(void);


/**
 * @brief Accessor: the pending event ([0x1979]).
 *
 * @return the event code
 */
uint8_t object_sequence_action_pending(void);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief [0x1988]: the program cursor byte (the pending action 5 writes it, 0x382D).
 *
 * @param cursor the cursor
 */
void object_sequence_action_set_cursor(uint8_t cursor);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief [0x5B56] (a raw byte), for tests and for a program restored from a
 * save.  The shipped writers produce 1..6; the collision tail can store the
 * shared scratch byte (any value; an out-of-table id is bounded, D-186).
 *
 * @param index the index
 */
void object_sequence_action_set_program(uint32_t index);

#endif /* HUMANSRE_OBJECT_SEQUENCE_H */
