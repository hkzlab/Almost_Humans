/* The command dispatch and its type rows
 */

#ifndef HUMANSRE_OBJECT_COMMAND_H
#define HUMANSRE_OBJECT_COMMAND_H

#include <stdint.h>

/* 1000:E4B0's outcome for one command. */
#define OBJ_CMD_NONE     0 // neither a handler nor a tail ran
#define OBJ_CMD_HANDLER  1 // the type row's handler ran
#define OBJ_CMD_TAIL     2 // the row's tail handler ran
#define OBJ_CMD_UNPORTED (-1) // no port

/* 1000:E3F6's command dispatch: the acting record's handler for one input word
 * (`[0x5ACE]`).  `handler` is the in-segment address the original calls
 * (0 = none, so the row's `tail` applies); `command` is the 0..7 number the
 * type row was indexed by.  Index 2 is 1000:E609, whose tail holds the human
 * stack's push.  Every handler body is ported (the former `0xE7C0` and
 * `0xEBCA` gaps were closed). */
typedef struct {
    uint16_t handler;
    uint16_t tail;
    uint16_t hook; // the row's +0xA: run 1000:D8BD before the tail
    uint8_t command;
} ObjCommand;


/** @brief 1000:D860 collision_scan: update_world's last call (1000:80D5), the
 * per-frame row pass over the eight static records.  The acting record is
 * skipped (it ran its row through 1000:E4B0 already); every other record whose
 * x is not 0xFFFF runs its type row's +0xA settle hook (1000:D8BD) and then the
 * row's +0xC tail - the *first* half of the row, with no input-bit-7
 * selection.  `CALL word ptr [0x6845]` is unconditional, but every shipped
 * row's tail is a real handler (the common one is the bare RET 0xD9C7).
 */
void object_command_row_pass(void);


/**
 * @brief Accessor: one pending event's offset (the 0x67FF step table).
 *
 * @param i the index
 * @return the offset
 */
#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_command_anim_move_off_for_probe(uint32_t i);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief Accessor: one pending event's wait.
 *
 * @param i the index
 * @return the wait
 */
#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_command_anim_move_wait_for_probe(uint32_t i);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 1000:E3F6 dispatch_actor_command: select the acting record's command
 * for one input word.
 *
 * One global counter, not one per command: [0x6DEA].
 *
 * @param rec the record
 * @param input the input state
 * @return the selected command descriptor
 */
ObjCommand object_command_select(uint8_t *rec, uint8_t input);


/** @brief 1000:E4B0..E4F7: run one command for the acting record.  The type row's
 * handler runs first, and when it changes the record's type the tail is skipped
 * (1000:E4CC); otherwise the row's +0xA hook (1000:D8BD, the terrain settle)
 * and then the tail handler run.
 *
 * This is the ACTING record's path.  Every other static record runs its row
 * from object_command_row_pass (collision_scan 1000:D860, the last call of
 * update_world), which the port runs after object_core_world_tick.
 *
 * @param actor the object record
 * @param input the input state
 * @return the command class that ran, or OBJ_CMD_UNPORTED
 */
int32_t object_command_run(uint8_t *actor, uint8_t input);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief Test seams for the two handler bodies (the row selector's one-shot gate
 * keeps 1000:E7C0 from being driven directly in the unit test).
 *
 * @param actor the object record
 */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_stream_command(uint8_t *actor);
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test seam: clear the record's hold.
 *
 * @param actor the object record
 */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_clear_countdown(uint8_t *actor);
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test seam: set the command timer.
 *
 * @param v the value
 */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_set_command_timer(uint32_t v);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 0x6F22 entry: word0 = base x (bit 15 free), +4 = direction<<4 | count,
 * +5.. = member list (bit-7 terminated). Every 6th call the count steps and
 * each member record whose frame is 0x20..0x22 gets x = group.word0 +
 * _obj_anim_x[length-3][count][member] (1000:D6B2 phase 3).
 */
void object_command_animator_tick(void);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: one group record (0x6F22 entry).
 *
 * @param i the index
 * @return the record
 */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t *object_command_group_at(uint32_t i);
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Reset the animator's state.
 */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_animator_reset(void);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 1000:F17C script_op_select_on_zero: the object selector, and the stack's
 * "walk off" path.  `xoff` = [0x4234], `frame` = [0x4238], `type` = [0x423C] -
 * the two wrappers 1000:E93D (8/1) and 1000:E952 (0/0) set them and jump here.
 * Returns 0 = done, -1 = nothing found (the original's `JS`); the terrain
 * branch (`1000:F1D7`'s probe, `1000:F223`'s special-tile test) is in.
 *
 * @param actor the object record
 * @param xoff the x offset
 * @param frame the frame index
 * @param type the type
 * @return -1 when the selector cannot move, 0 otherwise
 */
int32_t object_command_selector(uint8_t *actor, uint16_t xoff, uint16_t frame,
                                uint16_t type);


/** @brief 1000:E4F8 (handler 1): the step-down state.
 *
 * The cell at the feet (`x + 8`, `y + 0x16`) decides: a special tile (field K,
 * [0x6F20]/[0x6F21]) snaps the actor to the tile grid, a 0x6F92 span at that
 * column takes it onto the span, else the grab (1000:E58B) runs.  Returns
 * 1 tile rule, 2 span, 3 grabbed, 4 registered, -1 nothing.
 *
 * @param actor the object record
 * @return 1-4 for the state reached, or -1 on failure
 */
int32_t object_command_step_down(uint8_t *actor);


/** @brief 1000:E609 (handler 2): the walk-up state.
 *
 * Same three-way split, with `x + 8, y + 0x19` as the feet cell; the span test
 * uses `y + 0x18` as the line and a 0x10-px column window, and the fall-through
 * is the push (1000:E697).
 * Returns 1 tile rule, 2 span, 3 pushed, 4 nothing.
 *
 * @param actor the object record
 * @return 1-4 for the state reached
 */
int32_t object_command_step_up(uint8_t *actor);

#endif /* HUMANSRE_OBJECT_COMMAND_H */
