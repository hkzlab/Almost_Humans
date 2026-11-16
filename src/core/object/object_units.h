/* The stack and group verbs, the record dissolution and the row tails,
 * including the fall machine.
 */

#ifndef HUMANSRE_OBJECT_UNITS_H
#define HUMANSRE_OBJECT_UNITS_H

#include <stdint.h>


/**
 * @brief 1000:E697 (inside 1000:E609, the walk-up state): push the acting
 * record onto the 0x6F22 group it stands under.
 *
 * The original reaches this with AX = actor.x and BX = actor.y + 0x18 after
 * the 0x6F92 span scan failed.  A 0x6F22 entry has +0/+2 x/y, bit 15 = free,
 * +4 the animator state, +5 the owner index and the bit-7-terminated member
 * list from +6.  The 0x64AD/0x64C1 stack masks (1000:F38F/F3CA) are the TER
 * slots' +0x12 high bytes (1000:C8A4), so a marked record rides the platform.
 *
 * @param actor the object record
 * @return 1 when the record was pushed, 0 otherwise
 */
int32_t object_units_stack_push(uint8_t *actor);


/** @brief 1000:E58B (the tail of 1000:E4F8, the step-down state): grab the group the
 * actor stands at - within 8 px in x and 4 px in y - and append it to the
 * member list (frame 0x23, type 6).  With no group matching, register the
 * actor's position in the last free `0x6F22` entry (frame 0x20, type 8).
 * Returns 1 = grabbed, 0 = registered, -1 = nothing (no group, no free entry:
 * the original would write through a stale `[0x423C]`, the port refuses).
 *
 * @param actor the object record
 * @return 1 when a group was grabbed, 0 when already grouped, -1 on failure
 */
int32_t object_units_stack_grab(uint8_t *actor);


/** @brief 1000:FDED object_script_select (the pop): find the acting record in a
 * `0x6F22` list and, if it is the topmost member, remove it.  Returns the
 * original's AX: -1 = not present or not the top, 0 = the record below is within
 * 8 px of `ax` (the caller's x) so the list is left alone, 1 = removed.
 * `*below` = the record the original leaves in DI (NULL when the entry was
 * freed).
 *
 * @param ax the x accumulator
 * @param below the cell below
 * @param entry the entry
 * @return 1 when a member was popped, 0 when aligned, -1 on failure
 */
int32_t object_units_stack_pop(uint16_t ax, uint8_t **below, uint8_t **entry);


/** @brief 1000:E860 (handler 10): step off the group.  Finds the entry with the actor's
 * x, the actor in its list, and on `entry.y - 0x18 == actor.y` moves the actor
 * down 8 (frame 0x20, type 8) and the list byte below it likewise (frame 0x21).
 * Returns 1 = acted, 0 = not in a group, -1 = the `y` mismatch branch (1000:E8D4
 * -> 1000:FD91, the walk frame step).
 *
 * @param actor the object record
 * @return 1 when the record stepped off, 0 when not grouped, -1 on failure
 */
int32_t object_units_stack_step_off(uint8_t *actor);


/** @brief 1000:E8DD (handler 11): leave the group.  Same walk, and on `entry.y ==
 * actor.y` the actor becomes a standing human (frame 0x10, type 2) and the list
 * is cut at the slot the push stored in `actor+0xE`.
 * Returns 1 = left, 0 = not in a group, -1 = the `y` mismatch branch (1000:E934
 * -> 1000:FD91).
 *
 * @param actor the object record
 * @return 1 when the group was left, 0 when not grouped, -1 on failure
 */
int32_t object_units_stack_leave(uint8_t *actor);


/** @brief 1000:E963 (handler 14): snap onto the group's x.  Only the topmost member
 * (the byte after it is a terminator) moves: `actor.x = entry.x`, frame 0x20.
 *
 * @param actor the object record
 * @return 1 when the record snapped, 0 otherwise
 */
int32_t object_units_stack_snap(uint8_t *actor);


/** @brief 1000:E99D (index 15): stand back on the group.  The topmost member takes the
 * entry's x, `y -= 7`, frame 0x25 and type 7 - and stores its slot in `+0xE` -
 * while the list byte below it moves up 8 (frame 0x20).  Like the pop, its
 * "below" lookup reads `match-1` literally, so for the owner itself that byte is
 * the entry's `+4` state.
 *
 * @param actor the object record
 * @return 1 when the record stood, 0 otherwise
 */
int32_t object_units_stack_stand(uint8_t *actor);


/** @brief 1000:F453 scan_groups_for_record with the callers' `DX = 5`: the first
 * 0x6F22 entry whose member list (from +5) contains `recIndex` gets dissolved,
 * then the `DL & 1` class-transfer tail runs (whether or not a group matched).
 *
 * @param recIndex the record index
 * @return 1 when a group was found, 0 otherwise
 */
int32_t object_units_stack_scan_dissolve(uint32_t recIndex);


/** @brief Test seam: 1000:FA13 object_group_dissolve by group slot.
 *
 * The slot is the 0x6F22 entry the tail holds in BP when it calls it.
 *
 * @param slot the slot
 */
void object_units_group_dissolve_at(uint32_t slot);


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/** @brief Test seam: run `1000:F3ED` on `actor`.
 *
 * 1000:F3ED's gate, for the test: clears the mask, applies the 0x6EBE type
 * gate, then tests the two TER slots' attach boxes (1000:F423).
 * A box match sets the record's carry bit in the slot and returns 0 (settle
 * refused).
 *
 * @param actor the object record
 * @return 1 when the settle may run, 0 when a TER slot claimed the actor
 */
uint8_t object_units_settle_gate_for_probe(uint8_t *actor);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_OBJECT_UNITS_H */
