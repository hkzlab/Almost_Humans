/* The tribe and the level end: members, the goal count, the end test
 * and the outcome.
 */

#ifndef HUMANSRE_LEVEL_TRIBE_H
#define HUMANSRE_LEVEL_TRIBE_H

#include <stdint.h>

/**
 * @brief level_run 3060/3062: the level's required humans.
 *
 * [0x17FB] is the map's 0xAA count, compared against the tribe ([0x11FC]).
 * A tribe that is too small plays the briefing's GAME OVER variant and the
 * level is SKIPPED; [0x1809] then takes entry 1000:00DC -> 002B (the
 * restart-game path), not the next level.
 *
 * @return the members the level needs
 */
uint16_t level_required(void);

/**
 * @brief [0x17F0]: the level header's +0x02 low byte, the goal scan's threshold.
 *
 * @return the level's goal count
 */
uint32_t level_goal_count(void);

/**
 * @brief Accessor: the tribe's current member count.
 *
 * @return the count
 */
uint16_t level_members(void);

/**
 * @brief 1000:DE1F: the join tail increments the tribe count ([0x11FC]).
 */
void level_add_member(void);

/**
 * @brief 1000:41E6 (check_highscore): store the tribe counter + live-record
 * recount into [0x11FC].
 *
 * The only per-level write besides the difficulty pick/restore/join.
 *
 * @param n the recounted member count
 */
void level_set_members(uint32_t n);

/** @brief entry 1000:009C..0x00B1: [0x11FC] := 12/10/8 by the LEVEL board's
 * counter ([0x5E65]).
 *
 * Runs once when the front-end room hands over to the level walk (after the
 * RESTORE GAME slot is read and before the first load).  A level transition
 * inside the walk and a RESTART THIS LEVEL do not re-seed: the count carries,
 * and level_restore (1000:35AF) puts back the level-start snapshot.
 */
void level_seed_tribe(void);

/** @brief level_restore (1000:35AF): [0x11FC] := [0x1970], the members the
 * level started with, before its own joins.
 *
 * Runs before the reload, as 1000:35CD does (level_start 1000:3032 and
 * level_run 1000:3054 re-read and re-snapshot the same count at 306D).
 */
void level_restore_members(void);

/**
 * @brief The level's end request with the win/lose outcome.
 *
 * level_run's end: the play loop exits when object_core_test_dispatch (1000:35D6)
 * returns negative, and [0x138E] (0xFF won / 0 extinct) then selects
 * draw_level_text's page.
 * The six test handlers need the placed- object array (0x3E75), which object.c
 * holds; level_end_test evaluates the six predicates through
 * object_core_test_dispatch.
 * level_request_end(1) = the level was completed.
 *
 * @param won the won flag
 */
void level_request_end(uint8_t won);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief The win hook the discovery/claim path sets.
 */
void level_goal_reached(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief One tribe member was lost: decrement the counter.
 */
void level_member_lost(void);
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief The level-end test (object_core_test_dispatch's negative return).
 *
 * Runs from level_tick's play phase, like the original's call in
 * input_dispatch_loop.  [0x138E] stays 0xFF for a win and is cleared for the
 * extinct/time-out paths.
 *
 * @return 1 when the level-end test fires, 0 otherwise
 */
uint8_t level_end_test(void);

/**
 * @brief Accessor: the play loop's end reached and its hold finished.
 *
 * @return 1 when the level is over, 0 otherwise
 */
uint8_t level_ended(void);

/** @brief 1000:34E6: the play loop's exit - after the end test's negative return the
 * loop plays sound 0xC (1000:83B7) and waits 15 vsyncs (1000:8B56) on the
 * frozen _world before returning to draw_level_text.
 *
 * @return 1 while the exit hold runs, 0 otherwise
 */
uint8_t level_end_holding(void);

/** @brief 1000:F66D: the goal-record tail's "no survivor" exit sets [0x138E] = 0 and
 * ends the play loop (the same state object_core_test_dispatch's negative return
 * produces, reached from the object machine instead).
 *
 * @param won the won flag
 */
void level_set_end(uint8_t won);

/**
 * @brief Accessor: 1 when the level was won ([0x138E]: 1 won, 0 extinct).
 *
 * @return 1 won, 0 extinct
 */
uint8_t level_outcome(void);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief [0x5D31] after level_load's LEVEL-board pick: the record's timeA/B/C
 * word (the bar's seed).
 *
 * The pick is per level (render_level_frame 1000:9589); the 12/10/8 starting
 * tribe is the entry's seed (level_seed_tribe, 1000:009C..0x00B1) and is not
 * re-picked here.
 *
 * @return the time limit word
 */
uint16_t level_time_limit(void);
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief Accessor: the tribe's member count.
 *
 * @return the count
 */
uint16_t level_tribe_members(void);

#endif /* HUMANSRE_LEVEL_TRIBE_H */
