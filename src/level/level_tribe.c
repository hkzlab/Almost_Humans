#include "frontend.h"
#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "statusbar.h"

#include <stdint.h>

static uint16_t _time_limit; // [0x5D31]: the difficulty's BCD pick
static uint16_t _tribe_members; // [0x11FC]: 12/10/8 by the difficulty

static uint16_t _level_required; // [0x17FB]: the map's 0xAA count
static uint16_t _members_at_start; // [0x1970]: the members at level start
static uint8_t _end_requested; // the play loop's exit (35D6's -1)
static uint8_t _end_sound_sent; // 1000:34E6's 0x83B7 call, once
static uint8_t _play_exit_hold; // 1000:34E6: the 15 x 1000:8B56 waits
static uint8_t _goal_hit; // the discovery/claim win hook
static uint8_t _end_won; // [0x138E]: 1 won, 0 extinct

uint16_t level_required(void) { return _level_required; }

uint32_t level_goal_count(void)
{ return _level_is_loaded() ? _level_record()->goalCount : 0u; }

uint16_t level_members(void) { return _tribe_members; }

void level_add_member(void)
{
    if (_tribe_members < 0xFFu) _tribe_members++;
}

void level_set_members(uint32_t n) { _tribe_members = (uint16_t)(n); }

void level_restore_members(void)
{
    _tribe_members = _members_at_start;
    statusbar_start(_time_limit, _tribe_members, _level_required);
}

void level_seed_tribe(void)
{
    uint8_t skill = frontend_option(FRONTEND_OPT_LEVEL); // [0x5E65]

    _tribe_members = (skill == 0u) ? 12u : (skill == 1u) ? 10u : 8u; // 00B1
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void level_goal_reached(void) { _goal_hit = 1; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void level_member_lost(void)
{
    if (_tribe_members > 0u) _tribe_members--;
    statusbar_arm_tribe_count(); // the tribe counter rolls down (1000:F682)
    if (_tribe_members == 0u) {
        _end_requested = 1;
        _end_won = 0;
    }
}
#endif /* HUMANSRE_TEST_PROBES */

uint8_t level_end_test(void)
{
    if (_end_requested) return 1;
    if (_goal_hit) {
        level_request_end(1);
        return 1;
    }
    /* object_core_test_dispatch (1000:35D6): the level record's +0x04 word is the
     * goal test.  It is dispatched unconditionally (1000:34B8), and mode 0 is
     * `pred_type_low_phase_match` (1000:3600), not "no test": 128/160 shipped
     * levels use it (level 2's 0xA8 goal included). */
    if (_level_is_loaded() &&
        object_core_test_dispatch(_level_record()->objectTest)) {
        level_request_end(1);
        return 1;
    }
    if (_tribe_members == 0u) {
        level_request_end(0);
        return 1;
    }
    return 0;
}

void level_request_end(uint8_t won)
{
    _end_requested = 1;
    _end_won = won ? 1 : 0;
}

uint8_t level_ended(void) { return _end_requested && _play_exit_hold == 0u; }

uint8_t level_end_holding(void) { return _play_exit_hold > 0u; }

void level_set_end(uint8_t won)
{
    _end_requested = 1;
    _end_won = won ? 1 : 0;
}

uint8_t level_outcome(void) { return _end_won; }

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint16_t level_time_limit(void) { return _time_limit; }
#endif /* HUMANSRE_TEST_PROBES */

uint16_t level_tribe_members(void) { return _tribe_members; }

void _level_tribe_configure(uint16_t time, uint16_t members, uint16_t required)
{
    _time_limit = time;
    _tribe_members = members;
    _level_required = required;
    _members_at_start = members; // [0x1970] (306D)
}

uint8_t _level_end_requested(void) { return _end_requested; }

uint8_t _level_exit_hold_step(void)
{
    if (_play_exit_hold == 0u) return 0;
    _play_exit_hold--;
    return 1;
}

uint8_t _level_end_sound_pending(void)
{ return (uint8_t)(_end_requested && !_end_sound_sent); }

void _level_end_hold_start(void)
{
    _end_sound_sent = 1;
    _play_exit_hold = 0x0Fu; // MOV CX,0xF / LOOP 8B56
}

void _level_tribe_reset(void)
{
    _end_requested = 0; // [0x138E] = 0xFF in play
    _end_sound_sent = 0;
    _play_exit_hold = 0;
    _end_won = 1;
    _goal_hit = 0;
}
