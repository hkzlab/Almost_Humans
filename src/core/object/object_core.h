/* The record array, its state, the goal predicates and the save ranges
 */

#ifndef HUMANSRE_OBJECT_CORE_H
#define HUMANSRE_OBJECT_CORE_H

#include <stdint.h>


/**
 * @brief The goal scan over the placed records.
 *
 * 1000:C2CE count_tile_collisions: the goal scan; sets [0x6530] when the count
 * of records standing on a 0xBA tile reaches the level's goal byte.
 */
void object_core_goal_scan(void);


/**
 * @brief Accessor: the goal flag ([0x6530]).
 *
 * @return 1 when the goal fired, 0 otherwise
 */
uint8_t object_core_goal_flag(void);


/** @brief [0x6531]: menu_flag_tick (1000:3540) raises it after the goal pan's hold
 * and clears it when [0x6530] falls; 1000:C541's mode arm reads it.
 *
 * @param on the on/off flag
 */
void object_core_set_marker_mode(uint8_t on);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: the marker mode flag ([0x6531]).
 *
 * @return 1 while the marker mode holds, 0 otherwise
 */
uint8_t object_core_marker_mode(void);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief The array's initial state (`1000:4CAB`): every one of the 29 records gets
 * `x = y = 0xFFFF` and `+6 = 0x80` (flags bit 7 = free, class 0), the rest 0;
 * `[0x3E20]`/`[0x3E22]`/`[0x32B9]` are cleared and the recipes re-armed.
 * The live level confirms it: slots 4..7 read `x = y = 0xFFFF, flags = 0x80`.
 */
void object_core_init(void);


/**
 * @brief Accessor: one record slot.
 *
 * @param i the index
 * @return the slot, or NULL past object_core_slots()
 */
uint8_t *object_core_slot(uint32_t i);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: the record slot count.
 *
 * @return the count
 */
uint32_t object_core_slots(void);
#endif /* HUMANSRE_TEST_PROBES */


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: the frame counter ([0x4103]).
 *
 * @return the counter
 */
uint16_t object_core_frame_counter_probe(void);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief Save 0x3E20..0x407E: [0x3E20] placed humans, [0x3E22] entity index,
 * the 29 fixed pointers, the 20 class bytes, [0x3E73] live count and the 29
 * 0x12 records at 0x3E75.
 *
 * The pointer table is the image's constant run.
 *
 * @param dst the destination buffer
 */
void object_core_save_table(uint8_t *dst);


/**
 * @brief Load the behaviour table.
 *
 * @param src the source buffer
 */
void object_core_load_table(const uint8_t *src);


/** @brief 0x6F20..0x82EA: the special tiles, the 8 group templates at 0x6F22 and the
 * two 0x1C spans at 0x6F92; the image is zero elsewhere, and so is this.
 *
 * @param dst the destination buffer
 */
void object_core_save_pool(uint8_t *dst);


/**
 * @brief Load the shared sprite pool.
 *
 * @param src the source buffer
 */
void object_core_load_pool(const uint8_t *src);


/**
 * @brief Save the action programs.
 *
 * @param dst the destination buffer
 */
void object_core_save_programs(uint8_t *dst);


/**
 * @brief Load the action programs.
 *
 * @param src the source buffer
 */
void object_core_load_programs(const uint8_t *src);


/**
 * @brief 0x649A..0x64C2: the two TER.DAT slots (2 x 0x14).
 *
 * @param dst the destination buffer
 */
void object_core_save_ter(uint8_t *dst);


/**
 * @brief Load the terrain table.
 *
 * @param src the source buffer
 */
void object_core_load_ter(const uint8_t *src);


/**
 * @brief 0x6F92..0x6FE6's second copy: the two spans, then zero.
 *
 * @param dst the destination buffer
 */
void object_core_save_spans(uint8_t *dst);


/**
 * @brief Load the span table.
 *
 * @param src the source buffer
 */
void object_core_load_spans(const uint8_t *src);


/**
 * @brief Accessor: the current record index ([0x1AF2], -1 = none).
 *
 * @return the index
 */
int16_t object_core_current(void);


/**
 * @brief Set the current record.
 *
 * @param slot the slot
 */
void object_core_set_current(int16_t slot);


/**
 * @brief Accessor: the player record index ([0x180A], -1 = none).
 *
 * @return the index
 */
int16_t object_core_player(void);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Set the player record.
 *
 * @param slot the slot
 */
void object_core_set_player(int16_t slot);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief 1000:3640 - -1 when NO record's type is 0x1D.
 *
 * @return -1 when no record is type 0x1D, 0 otherwise
 */
int16_t object_core_none_type1d(void);


/** @brief 1000:3659 - the current record's +7 indexes the source-slot table 0x3E5E;
 * a zero slot is false, and the slot's low six bits are compared with the
 * test's byte.
 *
 * @param want the wanted value
 * @return -1 on a match, 0 otherwise
 */
int16_t object_core_class_matches(uint32_t want);


/**
 * @brief 1000:3678 - the current record's type equals the test's word.
 *
 * @param type the type
 * @return -1 when the type matches, 0 otherwise
 */
int16_t object_core_type_equals(uint32_t type);


/** @brief 1000:36AB - the record's type is <= 2 and the map cell under it equals the
 * test's byte.
 *
 * @param cell the cell value to test against
 * @return -1 when the cell matches, 0 otherwise
 */
int16_t object_core_map_cell_equals(uint32_t cell);


/**
 * @brief Accessor: the static record count (the 8 the predicates scan).
 *
 * @return the count
 */
uint8_t object_core_static_slots(void);


/**
 * @brief Accessor: the live record count ([0x3E73]; 9 at the level start).
 *
 * @return the count
 */
uint16_t object_core_live_count(void);


/**
 * @brief Accessor: one slot's class byte ([0x3E5E + slot]).
 *
 * @param slot the slot
 * @return the class byte
 */
uint8_t object_core_slot_class(uint32_t slot);


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Test seam: resolve a link offset (the +0xE/+0xC link encoding).
 *
 * @param off the offset
 * @return the pointer, or NULL when unavailable
 */
uint8_t *object_core_link_for_probe(uint32_t off);
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: one span entry (the 0x6F92 span entry).
 *
 * @param i the index
 * @return the entry
 */
uint8_t *object_core_span_at_for_probe(uint32_t i);
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
/** @brief test seam: the stripe's seed row, which 1000:FC81 sets when it seeds a new
 * entry (`+6 == 0`, 1000:FD6C).
 *
 * @param i the index
 * @param top the top value
 */
void object_core_span_seed_for_probe(uint32_t i, uint32_t top);
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Test seam: the landing probe.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param cellOut the output map cell
 * @return the landed y, or -1 when off the map
 */
int32_t object_core_land_y_for_probe(int32_t x, int32_t y, uint32_t *cellOut);
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
/** @brief 1000:FD7B: the span's +0x12 flag is 0xFFFF when d equals the entry's
 * frame, 0 below it and 1 above it.
 *
 * @param d the stored frame
 * @param frame the frame index
 * @return the span flag (0, 1 or 0xFFFF)
 */
uint16_t object_core_span_flag_for_probe(int32_t d, uint32_t frame);
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: one recipe parameter.
 *
 * @param recipe the recipe index
 * @param k the parameter index
 * @return the parameter
 */
uint16_t object_core_recipe_param_for_probe(uint32_t recipe, uint32_t k);
#endif /* HUMANSRE_TEST_PROBES */

/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Test seam: set [0x3E5E + slot] (the behaviour bytes).
 *
 * @param slot the slot
 * @param cls the class byte
 */
void object_core_set_slot_class_for_probe(uint32_t slot, uint8_t cls);
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Test seam: set one slot's class.
 *
 * @param slot the slot
 * @param cls the class byte
 */
void object_core_set_slot_class(uint32_t slot, uint32_t cls);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief object_core_test_dispatch (1000:35D6): the level record's goal test.
 *
 * The level record's `+0x04` word (the 13-byte header copied to `202F:0x17F2`)
 * is the whole goal test: the low byte is the mode, the high byte its
 * argument (the handlers read `DI = 0x17F3`).  The mode's low 7 bits index the
 * CS word table at `0x35F4` = `0x3600, 0x3640, 0x3659, 0x3678, 0x3688,
 * 0x36AB`; bit 7 first runs the entity scan whose result the dispatch
 * discards, so it changes nothing and the port masks it off.
 *
 * Returns 1 = the goal is met (the original's -1).
 *
 * @param mode the mode
 * @return 1 when the active test fires, 0 otherwise
 */
int16_t object_core_test_dispatch(uint16_t mode);


/**
 * @brief Accessor: the placed human count ([0x3E20]).
 *
 * @return the count
 */
uint16_t object_core_placed_humans(void);


/** @brief The map cell under a static record (0xF761 with AX = 8, BX = 0xC).
 *
 * The score's object bonus reads it: 0xA8 -> +1000, 0xA7 -> +2000
 * (1000:55FA).  0xFFFF for a non-static slot or when no level is loaded.
 *
 * @param slot the slot
 * @return the map cell, or 0xFFFF when there is no record
 */
uint32_t object_core_record_map_cell(uint32_t slot);


/** @brief 0x6EC3 + type != 0: the overlap/carry gate over the image's full DS
 * window - the static run, then the live register pool, then the spans.
 *
 * @param type the record type
 * @return 1 when the gate blocks the type, 0 otherwise
 */
uint8_t object_core_type_blocked(uint32_t type);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief object_core_tick: one level frame for the object machine (the frame
 * counter + the claim).
 *
 * @param levelPal the level palette
 */
void object_core_tick(const uint8_t *levelPal);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief One world tick.
 *
 * update_world's animator/bg tails (1000:80CC/0x80CF) and the claim's own
 * frame (frame counter + claim), separately for level_tick's [0xC] gate.
 */
void object_core_world_tick(void);


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: the world tick counter.
 *
 * @return the counter
 */
uint32_t object_core_world_ticks_probe(void);
#endif /* HUMANSRE_TEST_PROBES */


/** @brief The claim's own frame: [0x4103] advances on every page flip, and the claim
 * sequence runs while update_world is frozen (D-163).
 *
 * @param levelPal the level palette
 */
void object_core_claim_frame(const uint8_t *levelPal);


/** @brief pred_type_low_phase_match (1000:3600): the type <= 2 goal probe.
 *
 * The acting record's type must be <= 2 and the test runs once per frame
 * (`[0x4103]`, the frame counter, against its stored copy `[0x1972]`).  It
 * reads the map cell under the record (`1000:E361` with AX = 8, BX = 0xC) and
 * then **the cell one map row below it** (`[DI + 0x20]` - `DI` is still the map
 * cell pointer `1000:E361` left behind, and a map row is 32 bytes).  Either one
 * being the goal tile `0xA8` counts.  A miss clears the counter `[0x1976]`; a
 * hit counts it up and at `0x8C` (140) the goal is met (-1).
 *
 * SI comes from the caller (the dispatch's `0x8DF4`), i.e. the acting entity's
 * record - see `object_core_current`.
 *
 * @return -1 on the once-per-frame match, 0 otherwise
 */
int16_t object_core_low_phase_match(void);


/**
 * @brief Accessor: the last sound effect a handler asked for (1000:83B7).
 *
 * @return the sfx number
 */
uint8_t object_core_last_sfx(void);


/**
 * @brief Test seam: call the sound driver (1000:83B7 from another module).
 *
 * @param n the count
 */
void object_core_driver_call(uint32_t n);


/**
 * @brief Accessor: the ACTION press's latch ([0x13A4]) as the handlers see it.
 *
 * @return the latch byte
 */
uint8_t object_core_action_latched(void);


/**
 * @brief Set the ACTION latch (7 = op 7 was dispatched).
 *
 * @param on the latch byte
 */
void object_core_set_action_latched(uint8_t on);

#endif /* HUMANSRE_OBJECT_CORE_H */
