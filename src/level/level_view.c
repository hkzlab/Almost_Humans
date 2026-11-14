#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "byteorder.h"

#include <stdint.h>

static uint16_t _view_x, _view_y;

/* [0x2D8E]: the follower's +-1 px offset walk enabler.  Its two writers are
 * 1000:DAC7 (the entity/collision path) and 0xF8CE (the fall machine), both
 * ported; the walk itself over 0x2D92..0x2DA2 is _level_offset_walk below. */
static uint16_t _level_offset_word = 0xFFFFu; // [0x2D8E]: bit 15 idles the walk
/* DS:0x2D92..0x2DA4 - the follower's five offset pairs, read through the
 * pointer at [0x2D90] (0x2D92 at rest).  [0x2DA2]'s pair is (1, 1). */
static const int16_t _level_offset_pairs[10] = {
    0, 0, 1, -1, -1, 1, -1, -1, 1, 1,
};
static uint16_t _level_offset_ptr = 0x2D92u; // [0x2D90]

/* [0x2D8C]: the follower's record override while menu_flag_tick's goal pan
 * runs (a pointer in the original); -1 = 0, the acting record. */
static int8_t _goal_follow_record = -1;
/* menu_flag_tick's own state (1000:3540): the original blocks in it, running
 * update_world until the view stops, then 0x23 frames, then raising [0x6531]
 * and 0x1E more.  Every one of those is a presented frame, so the port runs
 * one per level tick. */
enum {
    GOALPAN_NONE = 0,
    GOALPAN_TRACK,
    GOALPAN_HOLD,
    GOALPAN_TAIL,
    GOALPAN_DONE
};
static uint8_t _goal_pan_state;
static uint8_t _goal_pan_left;
static uint16_t _goal_pan_view_x, _goal_pan_view_y;

void level_set_view(uint16_t x, uint16_t y)
{
    _view_x = x;
    _view_y = y;
}

/** @brief The pair the follower adds to the record's position ([0x2D90] indexes
 * DS:0x2D92..0x2DA4).  Outside that range the original would read whatever
 * follows in DS; the pointer cannot leave it (see the walk below), so the port
 * returns zero there (deviation D-25).
 *
 * @param dx the x delta
 * @param dy the y delta
 */
static void _level_offset_pair(int32_t *dx, int32_t *dy)
{
    uint16_t i = (uint16_t)(_level_offset_ptr - 0x2D92u) / 2u;

    if (i + 1u < sizeof _level_offset_pairs / sizeof _level_offset_pairs[0]) {
        *dx = _level_offset_pairs[i];
        *dy = _level_offset_pairs[i + 1u];
    } else {
        *dx = 0;
        *dy = 0;
    }
}

/** @brief 1000:5D37: the walk at the end of clip_object_x.  While [0x2D8E]'s bit 15 is
 * clear the pointer advances one pair per call (4 bytes); on the last pair
 * (0x2DA2) the original stores `0x2D92` into [0x2D8E] and then decrements that
 * word - i.e. it writes 0x2D91 into the GATE and leaves the pointer on the last
 * pair, so the camera keeps the (1, 1) offset and the gate stays 0x2D91 from
 * then on.  Transcribed as it stands.
 */
static void _level_offset_walk(void)
{
    if (_level_offset_word & 0x8000u) return;
    if (_level_offset_ptr != 0x2DA2u) {
        _level_offset_ptr = (uint16_t)(_level_offset_ptr + 4u);
        return;
    }
    _level_offset_word = 0x2D92u;
    _level_offset_word = (uint16_t)(_level_offset_word - 1u); // 0x2D91
}

void level_follow_view(void)
{
    uint8_t *rec = object_core_slot(_goal_follow_record >= 0
                                        ? (uint32_t)_goal_follow_record
                                        : object_sequence_action_entity());
    int32_t x, y, tx, ty, dx, dy;

    if (!rec) return;
    x = rd_le_i16(rec);
    y = rd_le_i16(rec + 2u);
    {
        int32_t ox, oy;

        _level_offset_pair(&ox, &oy); // 1000:5CCB/5CCD
        x += ox;
        y += oy;
    }
    tx = x - 0x88;
    ty = y - 0x44;
    tx = tx < 0 ? 0 : (tx > 0xC0 ? 0xC0 : tx);
    ty = ty < 0 ? 0 : (ty > 0xA8 ? 0xA8 : ty);
    dx = tx - (int32_t)_view_x;
    dy = ty - (int32_t)_view_y;
    if (dx > 4) dx = 4;
    else if (dx < -4) dx = -4;
    if (dy > 4) dy = 4;
    else if (dy < -4) dy = -4;
    _view_x = (uint16_t)((int32_t)_view_x + dx);
    _view_y = (uint16_t)((int32_t)_view_y + dy);
    _level_offset_walk(); // 1000:5D37
}

uint16_t level_view_x(void) { return _view_x; }

uint16_t level_view_y(void) { return _view_y; }

/** @brief menu_flag_tick (1000:3540): the goal pan.
 *
 * input_dispatch_loop calls it before scroll_map/update_world.  When the goal
 * scan has raised [0x6530] and the pan has not run, the original finds the
 * first of the 20 live records whose class byte is 9, stores its pointer in
 * [0x2D8C] (the follower's override), then blocks: update_world until the view
 * stops moving, then 0x23 more frames, then [0x6531] = 0xFF, then 0x1E more,
 * then [0x2D8C] = 0.  When [0x6530] falls it only clears [0x6531].
 *
 * The port advances one of those update_world frames per level tick (the tick's
 * own _world update is that frame), so the pan is presented for the same number
 * of frames as the original's blocking loop.  Each nested `update_world` calls
 * `1000:E3F6`, which calls `1000:8526 poll_input`; the original therefore
 * re-reads the live key state every pan frame and a released direction stops
 * the human.  The port passes each tick's latched input, never a frozen copy.
 *
 * @return 1 while the goal pan runs, 0 otherwise
 */
static int32_t _level_goal_pan_running(void)
{
    return _goal_pan_state == GOALPAN_TRACK ||
           _goal_pan_state == GOALPAN_HOLD || _goal_pan_state == GOALPAN_TAIL;
}

uint8_t level_goal_pan_running(void)
{ return (uint8_t)(_level_goal_pan_running()); }

/**
 * @brief The goal-text camera pan: one step per tick.
 */
void _level_goal_pan_tick(void)
{
    /* 1000:3568 is the track loop's head, so the view is captured every frame
     * the pan tracks - including after [0x6530] has fallen, which the original
     * cannot see inside its blocking loop. */
    if (_level_goal_pan_running()) {
        if (_goal_pan_state == GOALPAN_TRACK) {
            _goal_pan_view_x = _view_x; // 1000:3568
            _goal_pan_view_y = _view_y;
        }
        return;
    }
    if (!object_core_goal_flag()) { // 1000:35A2
        /* 1000:35A2 tests [0x6531] alone: whenever the goal flag is down and
         * the marker mode is up it is cleared (35A9), with no pan state
         * involved.  This is what a RESTART THIS LEVEL relies on: level_load
         * resets the pan state without touching the mode, so the re-spawned
         * behaviour-9 records would otherwise erase their reloaded cells. */
        object_core_set_marker_mode(0); // 1000:35A9
        if (_goal_pan_state != GOALPAN_NONE) {
            _goal_follow_record = -1;
            _goal_pan_state = GOALPAN_NONE;
        }
        return;
    }
    if (_goal_pan_state == GOALPAN_NONE) { // 1000:354E
        for (uint16_t i = 0; i < 20u; i++) {
            if (object_core_slot_class(1u + i) == 9u) {
                _goal_follow_record = (int8_t)(9u + i);
                break;
            }
        }
        if (_goal_follow_record < 0) { // 1000:3563: no goal record, no pan
            _goal_pan_state = GOALPAN_DONE;
            return;
        }
        _goal_pan_state = GOALPAN_TRACK;
        _goal_pan_view_x = _view_x; // 1000:3568, the loop's first capture
        _goal_pan_view_y = _view_y;
    }
}

/** @brief the pan's accounting, after the tick's update_world (= one of the original's
 * nested frames).
 */
void _level_goal_pan_after(void)
{
    switch (_goal_pan_state) {
    case GOALPAN_TRACK: // 1000:3576: the view stopped moving
        if (_view_x == _goal_pan_view_x && _view_y == _goal_pan_view_y) {
            _goal_pan_state = GOALPAN_HOLD;
            _goal_pan_left = 0x23u;
        }
        break;
    case GOALPAN_HOLD: // 1000:3582's LOOP CX = 0x23
        if (--_goal_pan_left == 0u) {
            object_core_set_marker_mode(1); // 1000:358C
            _goal_pan_state = GOALPAN_TAIL;
            _goal_pan_left = 0x1Eu;
        }
        break;
    case GOALPAN_TAIL: // 1000:3591's LOOP CX = 0x1E
        if (--_goal_pan_left == 0u) {
            _goal_follow_record = -1; // 1000:359B
            _goal_pan_state = GOALPAN_DONE;
        }
        break;
    default: break;
    }
}

void level_set_offset_word(uint16_t v) { _level_offset_word = v; }

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint16_t level_offset_word(void) { return _level_offset_word; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void level_reset_offset_walk(void) { _level_offset_ptr = 0x2D92u; }
#endif /* HUMANSRE_TEST_PROBES */

void _level_view_reset(void)
{
    _goal_follow_record = -1;
    _goal_pan_state = GOALPAN_NONE;
    _goal_pan_left = 0;
}
