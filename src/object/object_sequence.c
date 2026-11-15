/* The claim animation and the ACTION program machine.
 */

#include "object_internal.h"
#include "object_core.h"
#include "object_sequence.h"
#include "object_units.h"
#include "level/level.h"
#include "osystem.h"
#include "palette.h"
#include "statusbar.h"
#include "byteorder.h"
#include <string.h>

/* --- the claim: the entity bytecode's adjacent-object op (1000:3A6A) ---
 *
 * Runs with SI = the acting record (0x8DF4); ops 10/11/12 of CS:0x399C pass
 * recipe 3/0/1.  The gate and neighbour scan are 1000:3A6D..3AD9, the writes
 * 1000:3ADC..3B1E.  One tick per original frame:
 *
 *   red    1..0x7F -> (0x3F,0,0) div 5, then -> level div 0x1E
 *   white  1..0x7F -> white div 5, the 3D04 two-frame save, -> level div 0x19
 *   frame  one 0x9F56
 *   blinks 10x: 0x80 bank -> white div n, -> level div n (110 frames)
 *   anim   6 passes of 9 frames (0x3BE9's frame++); 0x103 -> 0x109
 *   append 3BF9..3C64: x = neighbour, y = +9, the stashed recipe, [0x3E73]++
 *   reveal 3E73/3F4A/3FB3 dirty cells until [0x1B02] (level_spark_begin)
 *   hold   41 frames, then white div 5, the remove, two page frames, -> level
 */
enum {
    CLAIM_OFF,
    CLAIM_TO_RED, // 1000:3B56  1..0x7F -> red, div 5
    CLAIM_RED_BACK, // 1000:3B68  -> the level palette, div 0x1E
    CLAIM_TO_WHITE, // 1000:3B82  1..0x7F -> white, div 5
    CLAIM_PAN, // 1000:3B91  1000:3D04: the two-frame present
    CLAIM_WHITE_BACK, // 1000:3B97  -> the level palette, div 0x19
    CLAIM_FRAME, // 1000:3BA9  one 0x9F56 frame, then the blinks
    CLAIM_BLINK_WHITE, // 1000:3BB3  the 0x80 bank -> white, div n
    CLAIM_BLINK_LEVEL, // 1000:3BC3  the 0x80 bank -> the level, div n
    CLAIM_ANIM, // 1000:3BD6  6 x 9 frames, `frame++` per pass
    CLAIM_REVEAL, // 1000:3C73  the blocking dirty-cell flush
    CLAIM_HOLD, // 1000:3C7D  41 frames
    CLAIM_WHITE_OUT, // 1000:3C88  1..0xBF -> white, div 5
    CLAIM_FRAME2, // 1000:3C9F  one frame, then the remove
    CLAIM_FLIP, // 1000:3CB5  the two page frames
    CLAIM_SETTLE // 1000:3CDE  1..0xBF -> the level, div 0x28
};

/* --- the ACTION program machine (script_op_dispatch, 1000:3979) --------
 *
 * pause_toggle (1000:3523) sets [0x13A4] and jumps here when ACTION is held
 * with no direction and [0x6F1D] == 0.  op = prog[prog[1] + 1] through the
 * 12-entry table at CS:0x399C; prog comes from [0x5B56] via 0x5B2B:
 *
 *   1  0x3A26 register in 0x6F22 (frame 0x20, type 8)
 *   2  0x9C1B collision pass (also 6)
 *   3  0x3A1A -> 0xECFE class gate, frame 0x8C/0x91
 *   4  0x3A05 -> 0xEFA1/EFB9 type <= 2 -> frame 0xBB/0x17
 *   5  0x39B5 type += 0x1F, frame = 0x10E + 4*type
 *   7  0x39CF -> 0xEA0F/EA4D the two rides (cmd_ride)
 *   8  0x39CC -> 0xEA71 span reset
 *   9  0x39E4 player claims 2, else walk/mark (EF0C/EF39)
 *  10/11/12 0x3A64/0x3A5F/0x3A5A claim with AX = 3/0/1
 */
enum {
    OP_REGISTER = 1,
    OP_COLLIDE_A = 2,
    OP_SELECT = 3,
    OP_TYPE_A = 4,
    OP_SETFRAME = 5,
    OP_COLLIDE_B = 6,
    OP_TYPE_B = 7,
    OP_RESET = 8,
    OP_CLAIM_OR_TYPE = 9,
    OP_CLAIM3 = 10,
    OP_CLAIM0 = 11,
    OP_CLAIM1 = 12
};

/* forward declarations */
static uint32_t _obj_sequence_program_id(void);
static void _claim_fill_ramp(uint8_t *ramp, uint32_t r, uint32_t g, uint32_t b);
static void _obj_sequence_marker_arm(void);
static void _obj_sequence_op_register(uint8_t *actor);
static void _obj_sequence_op_set_frame(uint8_t *actor);
static int32_t _obj_sequence_take_found(uint8_t *actor, uint8_t *found,
                                        uint8_t *slot, uint32_t ordinal);
static int32_t _obj_sequence_give_found(uint8_t *actor, uint8_t *found,
                                        uint8_t *slot, uint32_t ordinal);
static void _obj_sequence_class_drop(uint8_t *actor, uint8_t *found,
                                     uint8_t *slot);
static void _obj_sequence_class_throw(uint8_t *actor, uint8_t *found,
                                      uint8_t *slot);
static void _obj_sequence_op_class_path(uint8_t *actor);
static void _obj_sequence_collide_tail(void);
static void _obj_sequence_op_collide(uint8_t *actor);
static int32_t _obj_sequence_record_walk(uint8_t *actor, uint32_t step);
static void _obj_sequence_op_set_state(uint8_t *actor, uint32_t type);
static void _obj_sequence_find_overlap(uint8_t *actor);
static void _obj_sequence_op_mark_walk(uint8_t *actor, uint32_t step);
static void _obj_sequence_op_reset_span(void);
static void _obj_sequence_charge_draw(void);


static uint8_t _claim_phase = CLAIM_OFF;

static int8_t _claim_neighbour = -1;

static uint8_t _claim_behav;

static uint16_t _claim_sprite;

static uint16_t _claim_params[4];

static uint8_t _claim_blinks, _claim_timer;

static uint8_t _claim_red[768], _claim_white[768];


/* DS:0x1C76 (behaviour -> program - 1) and DS:0x5B81 (behaviour -> the
 * direction the collision pass dispatches on).  DS:0x5B8A is the collision
 * pass's own program id: on success it becomes `[0x5B56]` at 1000:9CEB
 * (`CL = [0x423C]`), which is both the ACTION program index and the status
 * bar's action-row group header.
 *
 * All four maps below are **8 bytes in the image** (0x1C76/0x5B81/0x5B8A/
 * 0x5B97; 0x1C76 is followed by the "\humans\" string), indexed with the
 * 0..0x3F behaviour.  The spawns write 0/1/2/3/5/8/9 (and the 0xB7 tile's
 * empty record 0x85, whose `&0x3F` is 5), so 8/9 are the only values past
 * the table that occur; the image's bytes there are 0x5C (program), 0xFF/
 * 0xFF (direction), 0xFF/0xF0 (collide program) and 0xFF/0x94 (class
 * index), i.e. an out-of-range program id (ignored), skip, return, return -
 * exactly what the bound `beh < 8 ? table[beh] : 0xFF` reproduces.  The
 * 10..0x3F values read whatever follows those tables in the image
 * (e.g. 0x5B81[13..15] = 6/2/4) and no writer produces them.
 * 0x5B81 marks 0..3 with bit 7 and 1000:9C9C skips them. */
static const uint8_t _obj_beh_program[8] = { 0, 0, 0, 0, 5, 1, 3, 2 };

static const uint8_t _obj_beh_dir[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 1, 1 };

static const uint8_t _obj_beh_collide_prog[8] = { 0xFF, 0xFF, 0xFF, 0xFF,
                                                  6,    2,    4,    3 };

/* DS:0x5B97 (behaviour -> the class path's handler index) and its target
 * table at 0x5BA0: {0x9D94, 0x9DD7}.  DS:0x5BA4 is the throw's x offset by
 * the actor's type (0 -> +8, 1 -> -8, everything else 0) and DS:0x5BAA the
 * frame by behaviour (5 -> 0x60, 6 -> 0x9C, 7 -> 0x96). */
static const uint8_t _obj_beh_class_idx[8] = { 0xFF, 0xFF, 0xFF, 0xFF,
                                               0,    1,    1,    1 };

/* DS:0x5BA4, the throw's x offset by the actor's type (0 -> +8, 1 -> -8,
 * 2 -> 0).  The original's `word [0x5BA4 + 2*type]` is unbounded, but the
 * class path is gated to type 0xF or <= 2 (`1000:9C21..9C2B`) and the only
 * type-0xF writer (the give, `1000:9D19`) pairs `+7` with a behaviour-4
 * record, whose class-path handler is the drop (`1000:9D94`), not the throw -
 * so the throw only ever reads these three words.  A type past 2 returns 0
 * (D-179). */
static const uint16_t _obj_throw_dx_word[3] = {
    0x0008,
    0xFFF8,
    0x0000,
};

static const uint8_t _obj_drop_frame[8] = { 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x60, 0x9C, 0x96 };


/* DS:0x6EB6 - the per-behaviour *state* byte `test_class_against_6eb6`
 * (0xF2CF) gates on: `MOV BL,[BX+0x3E5E] / AND BL,0x3F / CMP AL,[BX+0x6EB6]`
 * with BX = 0..0x3F.  The table is 21 bytes - behaviours 5/6/7 -> 1/3/2,
 * 8 -> 0x0F, 9 -> 0xFF, 10..12 -> 0xC2/0x1D/0xA0, 16..20 -> 1 - and the
 * original reads on into the next tables for behaviours 21..0x3F (0x6ECB is
 * `_obj_overlap_gate[8]`).  No writer stores a behaviour above 9 (the
 * spawns write 0/1/2/3/5/8/9, the recipes 4/5/6/7; D-101), so the port bounds
 * the index and returns 0 past the table (D-266). */
static const uint8_t _obj_beh_state[21] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x02, 0x0F, 0xFF, 0xC2,
    0x1D, 0xA0, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
};


/* 0x5B37..0x5B5E: the six overlapping program runs.  The six words at
 * 202F:0x5B2B point at offsets 0, 4, 0xA, 0x10, 0x16 and 0x1C; the raw id at
 * 0x5B56 is byte 0x1F, i.e. program 5's +9 and program 6's +3, exactly as in
 * the image.  The save carries only 0x5B37..0x5B56 (0x20 bytes), so the tail
 * of program 6 stays static across a load. */
uint8_t _obj_program_region[0x28] = {
    0x02, 0x01, 0x02, 0x01, 0x04, 0x01, 0x03, 0x04, 0x05, 0x06,
    0x04, 0x01, 0x03, 0x07, 0x08, 0x06, 0x04, 0x01, 0x03, 0x05,
    0x09, 0x06, 0x04, 0x01, 0x09, 0x0A, 0x0B, 0x0C, 0x01, 0x01,
    0x06, 0x01, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
};

const uint8_t _obj_program_start[6] = {
    0x00u, 0x04u, 0x0Au, 0x10u, 0x16u, 0x1Cu
};


/** @brief [0x5B56] is shared with the status bar (its action row's group), so every
 * write goes through here.  It is a raw byte: the shipped writers produce 1..6,
 * but the collision tail stores `[0x423C]` straight (`1000:9CEB`, any value).
 *
 * @param index the index
 */
void _obj_sequence_set_program_index(uint32_t index)
{ _obj_program_region[0x1Fu] = (uint8_t)index; }


/** @brief The raw [0x5B56] byte.
 *
 * @return the id
 */
static uint32_t _obj_sequence_program_id(void)
{ return (uint32_t)_obj_program_region[0x1Fu]; }


#if defined(HUMANSRE_TEST_PROBES)
int32_t object_sequence_throw_dx_for_probe(uint32_t type)
{ return type < 3u ? (int16_t)_obj_throw_dx_word[type] : 0; }
#endif /* HUMANSRE_TEST_PROBES */


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t object_sequence_beh_state_for_probe(uint32_t beh)
{ return beh < sizeof _obj_beh_state ? _obj_beh_state[beh & 0x3Fu] : 0u; }
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 0x54F4: the whole 768-byte ramp := one 6-bit colour.  The fill's register
 * order is (BL, AH, AL) = (R, G, B), hence the red call's AX = 0, BL = 0x3F.
 *
 * @param ramp the ramp table
 * @param r the red component
 * @param g the green component
 * @param b the blue component
 */
static void _claim_fill_ramp(uint8_t *ramp, uint32_t r, uint32_t g, uint32_t b)
{
    for (uint16_t i = 0; i < 256u; i++) {
        ramp[i * 3u] = (uint8_t)r;
        ramp[i * 3u + 1u] = (uint8_t)g;
        ramp[i * 3u + 2u] = (uint8_t)b;
    }
}


int32_t object_sequence_find_neighbour(int32_t actor)
{
    const uint8_t *cur;
    int32_t cx, cy;

    if (actor < 0 || (uint32_t)actor >= OBJ_STATIC) return -1;
    cur = _obj_array + actor * OBJ_STRIDE;
    cx = (int16_t)rd_le_u16(cur + 0u);
    cy = (int16_t)rd_le_u16(cur + 2u);

    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        const uint8_t *r = _obj_array + i * OBJ_STRIDE;
        int32_t dx, dy;

        if ((int32_t)i == actor) continue; // 1000:3A9C
        if (rd_le_u16(r + 8u) > 2u) continue; // type <= 2
        dx = (int32_t)(int16_t)rd_le_u16(r + 0u) - cx;
        dy = (int32_t)(int16_t)rd_le_u16(r + 2u) - cy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 0x20 || dy > 8) continue;
        if (r[6] & 0x80u) continue; // active
        return (int32_t)i;
    }
    return -1;
}


int32_t object_sequence_claim_begin(uint32_t recipe, int32_t actor)
{
    int32_t n;
    uint8_t *rec, *live;
    uint8_t cls;

    if (actor < 0 || (uint32_t)actor >= OBJ_STATIC) return -1;
    if (recipe >= 4u) return -1;
    if (rd_le_u16(_obj_array + actor * OBJ_STRIDE + 8u) > 2u) return -1;
    if (!_obj_uses[recipe]) return -1; // 1000:3A89: the use counter

    n = object_sequence_find_neighbour(actor);
    if (n < 0) return -1;

    rec = _obj_array + n * OBJ_STRIDE;
    cls = rec[7u];
    if (cls) { // 1000:3AE2..1000:3B00
        _obj_slot_class[cls] = 0u; // 0x3E5F + cls - 1
        live = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE;
        wr_le_u16(live + 6u, 0xFFFFu); // 0x3F17 + 0x12*(cls-1)
    }
    object_core_set_current((int16_t)(n)); // 1000:3B03
    wr_le_u16(rec + 4u, 0x0103u); // 1000:3B07
    wr_le_u16(rec + 6u, 0u); // 1000:3B0D
    wr_le_u16(_obj_array + actor * OBJ_STRIDE + 4u, 0x0020u); // 1000:3B13
    statusbar_set_active(0); // 1000:3B23: [0x5B12] = 0
    /* 1000:3B19/3B28: the world freezes for the sequence, and the display
     * window is snapped to a 4-pixel boundary (`AND [0x0], 0xFFFC`) because the
     * strip save below works in whole page bytes.  Neither moves the camera. */
    level_set_updates(0);
    level_set_view(level_view_x() & ~3u, level_view_y());

    /* 1000:3B34..1000:3B46: spend the use and stash the recipe's fields (the recipe
     * itself is no longer readable once its use is gone) */
    _obj_uses[recipe]--;
    _claim_behav = _obj_recipes[recipe][1];
    _claim_sprite = rd_be_u16(_obj_recipes[recipe] + 2u);
    for (uint16_t k = 0; k < 4u; k++)
        _claim_params[k] = rd_le_u16(_obj_recipes[recipe] + 4u + 2u * k);

    _claim_neighbour = (int8_t)(n);
    _claim_blinks = 10u; // 1000:3BAC
    _claim_timer = 0u;
    _claim_fill_ramp(_claim_red, 0x3Fu, 0u, 0u); // 1000:3B4F: AX=0, BL=0x3F
    _claim_fill_ramp(_claim_white, 0x3Fu, 0x3Fu,
                     0x3Fu); // 1000:3B7A: AX=0x3F3F
    _claim_phase = CLAIM_TO_RED;
    palette_build_fade(1u, 0x7Fu, 5u, _claim_red + 3u); // 1000:3B56
    return n;
}


uint8_t object_sequence_claim_active(void) { return _claim_phase != CLAIM_OFF; }


uint8_t object_sequence_claim_pan_active(void)
{ return _claim_phase == CLAIM_PAN; }


uint8_t object_sequence_claim_anim_active(void)
{ return _claim_phase == CLAIM_ANIM; }


uint8_t object_sequence_claim_flip_active(void)
{ return _claim_phase == CLAIM_FLIP; }


uint8_t object_sequence_claim_page_frozen(void)
{ return _claim_phase >= CLAIM_WHITE_BACK; }


uint8_t object_sequence_claim_settle_active(void)
{ return _claim_phase == CLAIM_SETTLE; }


void object_sequence_claim_tick(const uint8_t *levelPal)
{
    uint8_t *nrec = _claim_neighbour >= 0
                        ? _obj_array + _claim_neighbour * OBJ_STRIDE
                        : NULL;

    switch (_claim_phase) {
    case CLAIM_OFF: return;
    case CLAIM_TO_RED: // 1000:3B56
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_RED_BACK;
        palette_build_fade(1u, 0x7Fu, 0x1Eu, levelPal + 3u); // 1000:3B68
        break;
    case CLAIM_RED_BACK: // 1000:3B68
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_TO_WHITE;
        palette_build_fade(1u, 0x7Fu, 5u, _claim_white + 3u); // 1000:3B82
        break;
    case CLAIM_TO_WHITE: // 1000:3B91: 1000:3D04
        if (palette_fade_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_PAN; // two frames
        break;
    case CLAIM_PAN: // 1000:3D04, two frames
        /* 1000:3D04 presents the world twice (5810, 5d56, 3d5e); on the first
         * pass 1000:3D41 hands min(player, claimed) to 1000:3DC0, which SAVES a
         * 64x18-byte page strip at that point to A000:0xF000 - a scratch save
         * for the sequence, not a scroll: no [0x0]/[0x2] write exists anywhere
         * in it.  The camera stays where the follower left it (frozen above). */
        if (++_claim_timer < 2u) break;
        _claim_timer = 0u;
        _claim_phase = CLAIM_WHITE_BACK;
        palette_build_fade(1u, 0x7Fu, 0x19u, levelPal + 3u); // 1000:3B97
        break;
    case CLAIM_WHITE_BACK: // 1000:3BA9
        if (palette_fade_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_FRAME;
        break;
    case CLAIM_FRAME:
        if (++_claim_timer <= 1u) break; // 1000:3BA9's 0x9F56
        _claim_timer = 0u;
        _claim_phase = CLAIM_BLINK_WHITE;
        palette_build_fade(0x80u, 0x40u, _claim_blinks,
                           _claim_white); // 1000:3BB3
        break;
    case CLAIM_BLINK_WHITE: // 1000:3BB3
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_BLINK_LEVEL;
        palette_build_fade(0x80u, 0x40u, _claim_blinks,
                           levelPal + 0x80u * 3u); // 1000:3BC3
        break;
    case CLAIM_BLINK_LEVEL: // 1000:3BC3
        if (palette_fade_active()) return;
        if (--_claim_blinks == 0u) {
            _claim_timer = 0u;
            _claim_phase = CLAIM_ANIM;
            break;
        }
        _claim_phase = CLAIM_BLINK_WHITE;
        palette_build_fade(0x80u, 0x40u, _claim_blinks, _claim_white);
        break;
    case CLAIM_ANIM: // 1000:3BD6..1000:3BF6
        if (++_claim_timer % 9u == 0u) // the 1000:3BE9 frame++
            wr_le_u16(nrec + 4u, (uint16_t)(rd_le_u16(nrec + 4u) + 1u));
        if (_claim_timer < 56u) break;
        _claim_timer = 0u;
        /* 1000:3BF9..1000:3C64: the append from the stashed fields.  The live record is
         * `0x3F17 + 0x12*([0x3E73]-8)` (port index count+1) and its class
         * `[0x3E5F + count-8]` = `[0x3E5E + count-7]` (count-7 here), i.e.
         * class c <-> live index 8+c. */
        if (_obj_live_count + 1u < OBJ_SLOTS) {
            uint8_t *nr = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
            wr_le_u16(nr + 0u, rd_le_u16(nrec + 0u));
            wr_le_u16(nr + 2u, (uint16_t)(rd_le_u16(nrec + 2u) + 9u));
            wr_le_u16(nr + 4u, _claim_sprite);
            wr_le_u16(nr + 6u, 0u);
            wr_le_u16(nr + 8u, _claim_params[0]);
            wr_le_u16(nr + 0xAu, _claim_params[1]);
            wr_le_u16(nr + 0xCu, _claim_params[2]);
            wr_le_u16(nr + 0xEu, _claim_params[3]);
            _obj_slot_class[_obj_live_count - 7u] = _claim_behav;
            _obj_live_count++;
            /* 1000:3C70 calls init_dirty_cell_list (1000:3E73), whose tail
             * hands (x - 0x20, y - 0x20) of the appended record to 1000:3DC0:
             * the same strip save, at the new record.  It does not move the
             * window either; the 0x48BC cell list it builds is the redraw list
             * this flat compose replaces. */
            {
                /* 1000:3E73's other half: the shade table from the recipe's
                 * sprite, and the list starts around the appended record. */
                uint8_t shade[16 * 16];

                _obj_render_build_shade(_claim_sprite, shade);
                level_spark_begin(rd_le_u16(nr + 0u), rd_le_u16(nr + 2u),
                                  shade);
            }
        }
        _claim_phase = CLAIM_REVEAL;
        break;
    case CLAIM_REVEAL: // 1000:3C73: the blocking dirty-cell flush
        /* 1000:3C73 CALL 3F4A / 1000:3C76 TEST [0x1B02] / JZ 3C73: the reveal
         * finishes before the 41-frame hold.  level_claim_frame advances the
         * spark once per presented frame (3F4A's restore/step/draw/flip). */
        if (level_spark_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_HOLD;
        break;
    case CLAIM_HOLD: // 1000:3C7D: 0x28+1
        if (++_claim_timer < 41u) break;
        _claim_timer = 0u;
        _claim_phase = CLAIM_WHITE_OUT;
        palette_build_fade(1u, 0xBFu, 5u, _claim_white + 3u); // 1000:3C90
        break;
    case CLAIM_WHITE_OUT: // 1000:3C88
        if (palette_fade_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_FRAME2;
        break;
    case CLAIM_FRAME2: // 1000:3C9F
        if (++_claim_timer <= 1u) break; // one 0x9F56
        _claim_timer = 0u;
        wr_le_u16(nrec + 6u, 0xFFFFu); // 50A6: removed
        statusbar_set_active(1); // 50B0: [0x5B12]
        _claim_phase = CLAIM_FLIP;
        break;
    case CLAIM_FLIP: // 1000:3CB5: LOOP CX=2
        if (_claim_timer == 0u) level_set_updates(1); // 1000:3CAB: [0xC] = 0xFF
        if (++_claim_timer < 2u) break;
        _claim_timer = 0u;
        _claim_phase = CLAIM_SETTLE;
        palette_build_fade(1u, 0xBFu, 0x28u, levelPal + 3u); // 50DE
        break;
    case CLAIM_SETTLE: // 1000:3CDE
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_OFF;
        _claim_neighbour = -1;
        break;
    default: _claim_phase = CLAIM_OFF; break;
    }
}


uint16_t object_sequence_action_entity(void) { return _obj_entity; }


void object_sequence_level_start_record(void)
{
    uint8_t *r = object_core_slot(8u);

    if (!r) return;
    wr_le_u16(r + 4u, 0x0017u);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, 0x001Au);
    wr_le_u16(r + 0xAu, 0x1006u);
    /* The next instruction of level_fade_in, `MOV byte ptr [0xc], 0xff` at
     * 1000:336C, is the GLOBAL update_world gate (level.c's worldUpdates) -
     * not this record's +0xC, which stays 0 as the asm leaves it. */
}


/** @brief 1000:4188 (the selector's tail): the selection arms the active-human marker,
 * record 8, on the new acting record.  The original also writes
 * [0x6DEA] = 0x3E8 (the one-shot command timer).
 */
static void _obj_sequence_marker_arm(void)
{
    uint8_t *m = object_core_slot(8u);
    uint8_t *r = _obj_array + _obj_entity * OBJ_STRIDE;

    if (!m) return;
    wr_le_u16(m + 0u, rd_le_u16(r + 0u));
    wr_le_u16(m + 2u, (uint16_t)(rd_le_u16(r + 2u) - 0x1Au));
    wr_le_u16(m + 6u, 0u);
    wr_le_u16(m + 8u, 0x001Au);
    wr_le_u16(m + 0xAu, 0x0605u);
    _obj_cmd_timer = 0x03E8u;
}


void object_sequence_marker_tick(void)
{
    uint8_t *m = object_core_slot(8u);
    uint8_t *r = _obj_array + _obj_entity * OBJ_STRIDE;
    uint32_t type, frame, mode, ax;

    if (!m) return;
    if (m[6] & 0x80u) return; // 1000:D655
    wr_le_u16(m + 0u, rd_le_u16(r + 0u)); // 1000:D65B
    type = rd_le_u16(m + 8u);
    wr_le_u16(m + 2u, (uint16_t)(rd_le_u16(r + 2u) - type)); // 1000:D65F
    frame = rd_le_u16(r + 4u);
    mode = object_sprite_at(frame).mode;
    wr_le_u16(m + 0u, (uint16_t)(rd_le_u16(m + 0u) +
                                 ((int32_t)mode - 1) * 8)); // 1000:D66B
    m[0xAu] = (uint8_t)(m[0xAu] - 1u); // 1000:D68F
    if (!(m[0xAu] & 0x80u)) { // 1000:D693 JNS
        wr_le_u16(m + 8u, (uint16_t)(type));
        return;
    }
    m[0xAu] = 2u; // 1000:D695
    ax = type - 2u; // 1000:D699
    if (ax >= 0x12u) { // 1000:D69F JNC
        wr_le_u16(m + 8u, (uint16_t)(ax));
        return;
    }
    ax = 0x001Au; // 1000:D6A1
    m[0xBu] = (uint8_t)(m[0xBu] - 1u); // 1000:D6A4
    if (m[0xBu] & 0x80u) m[6] = 0x80u; // 1000:D6A8
    wr_le_u16(m + 8u, (uint16_t)(ax)); // 1000:D6AE
}


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_sequence_set_action_entity(uint16_t i)
{ _obj_entity = i < OBJ_STATIC ? i : _obj_entity; }
#endif /* HUMANSRE_TEST_PROBES */


uint8_t object_sequence_action_program(void)
{ return (uint8_t)(_obj_sequence_program_id()); }


const uint8_t *object_sequence_program_header(uint32_t group)
{ return _obj_core_program_slot(group); }


const uint8_t *object_sequence_action_header(void)
{ return object_sequence_program_header(_obj_sequence_program_id()); }


uint8_t object_sequence_action_byte(uint32_t k)
{
    uint8_t *h = _obj_core_program_slot(_obj_sequence_program_id());
    uint16_t off;

    if (!h) return 0u;
    off = (uint16_t)((uint32_t)(h - _obj_program_region) + k);
    return off < sizeof _obj_program_region ? _obj_program_region[off] : 0u;
}


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint32_t object_sequence_action_program_si(void)
{
    uint8_t *h = _obj_core_program_slot(_obj_sequence_program_id());

    return h ? 0x5B37u + (uint32_t)(h - _obj_program_region) : 0u;
}
#endif /* HUMANSRE_TEST_PROBES */


void object_sequence_step_action_cell(void)
{
    uint8_t *p = _obj_core_program_slot(_obj_sequence_program_id());
    uint8_t cursor;

    if (!p) return; // D-186
    cursor = (uint8_t)(p[1] + 1u);
    /* 1000:3941..0x394D: INC the cursor, wrap to 1 when it passes the count
     * (a count of 0 leaves the wrap's `CMP 0,cursor / JNC` to set 1). */
    if (cursor > p[0]) cursor = 1u;
    p[1] = cursor;
}


void object_sequence_action_set_program(uint32_t index)
{ _obj_sequence_set_program_index(index); }


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_sequence_action_set_cursor(uint8_t cursor)
{
    uint8_t *p = _obj_core_program_slot(_obj_sequence_program_id());

    _obj_cursor = cursor;
    if (p) p[1] = cursor; // D-186
}
#endif /* HUMANSRE_TEST_PROBES */


uint8_t object_sequence_action_pending(void) { return _obj_pending; }


/**
 * @brief 0x3A26 (op 1): register the object in the 0x6F22 table and turn it into type 8.
 *
 * @param actor the object record
 */
static void _obj_sequence_op_register(uint8_t *actor)
{
    if (rd_le_u16(actor + 8u) > 2u) return;
    for (uint16_t i = 0; i < 8u; i++) {
        uint16_t *s = _obj_register[i];
        if (!(s[0] & 0x8000u)) continue; // testw $0x8000 -> free
        s[0] = rd_le_u16(actor + 0u);
        s[1] = rd_le_u16(actor + 2u);
        ((uint8_t *)s)[5] = (uint8_t)_obj_entity;
        wr_le_u16(actor + 4u, 0x0020u);
        wr_le_u16(actor + 8u, 0x0008u);
        return;
    }
}


/**
 * @brief 0x39B5 (op 5): the "pick up / change state" frame step.
 *
 * @param actor the object record
 */
static void _obj_sequence_op_set_frame(uint8_t *actor)
{
    uint16_t t = rd_le_u16(actor + 8u);
    if (t >= 2u) return;
    wr_le_u16(actor + 8u, (uint16_t)(t + 0x1Fu));
    wr_le_u16(actor + 4u, (uint16_t)(0x010Eu + 4u * t));
}


/**
 * @brief 0x9D38 (direction 1): the found live record is taken by the actor.
 *
 * @param actor the object record
 * @param found the found flag
 * @param slot the slot
 * @param ordinal the ordinal
 * @return 1 when the found record was taken, 0 otherwise
 */
static int32_t _obj_sequence_take_found(uint8_t *actor, uint8_t *found,
                                        uint8_t *slot, uint32_t ordinal)
{
    if (rd_le_u16(found + 8u) != 0xFFFFu) return 0;
    if (rd_le_u16(found + 0xAu) != 0u) return 0;
    actor[7] = (uint8_t)ordinal; // [SI+3] = DL
    *slot |= 0x80u; // ORB $0x80, [BP]
    wr_le_u16(found + 8u, _obj_entity); // [DI+4] = [0x3E22]
    return 1;
}


/**
 * @brief 0x9CF0 (direction 0): the "give / hold" variant.
 *
 * @param actor the object record
 * @param found the found flag
 * @param slot the slot
 * @param ordinal the ordinal
 * @return 1 when the found record can be given, 0 otherwise
 */
static int32_t _obj_sequence_give_found(uint8_t *actor, uint8_t *found,
                                        uint8_t *slot, uint32_t ordinal)
{
    if (found[6] & 0x40u) return 0;
    if (!(rd_le_u16(found + 0xEu) & 0x8000u)) return 0;
    wr_le_u16(found + 0xEu, _obj_entity);
    found[7] = 0u;
    *slot |= 0x80u;
    wr_le_u16(actor + 4u, 0u); // the actor's own record:
    actor[7] = (uint8_t)ordinal;
    wr_le_u16(actor + 8u, 0x000Fu);
    wr_le_u16(actor + 0xAu, 0u);
    wr_le_u16(actor + 0xEu,
              (uint16_t)((uintptr_t)found - (uintptr_t)_obj_array + 4u));
    return 1;
}


/** @brief 1000:9D94 (the class path's behaviour-4 handler): the carried record is put
 * down and the acting record becomes a normal record again at its position.
 *
 * @param actor the object record
 * @param found the found flag
 * @param slot the slot
 */
static void _obj_sequence_class_drop(uint8_t *actor, uint8_t *found,
                                     uint8_t *slot)
{
    uint8_t ax = 0, bx = 0;

    actor[7] = 0u; // 1000:9D94
    *slot &= 0x7Fu;
    wr_le_u16(found + 0xEu, 0xFFFFu);
    found[6] &= 0x7Fu;
    if (rd_le_u16(found + 8u) & 0x8000u) { // 1000:9DAA
        ax = 1u;
        bx = 8u;
    }
    wr_le_u16(actor + 0u, rd_le_u16(found + 0u)); // 1000:9DB7
    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(found + 2u) - 8u));
    wr_le_u16(actor + 4u, bx);
    wr_le_u16(actor + 6u, 0u);
    wr_le_u16(actor + 8u, ax);
}


/** @brief 1000:9DD7 (behaviours 5..7): the carried record is thrown / placed at the
 * acting record's feet and snapped to the terrain.  The original's two scratch
 * stores ([0x4238] = -6 / 0x16, [0x423C] = 0x10) lead the body: [0x4238] is
 * re-written by every reader (1000:650B, 1000:A783), but [0x423C] is the
 * collision tail's program scratch (1000:9CD9), which can read it on a later
 * empty-map press, so it is modelled here.
 *
 * @param actor the object record
 * @param found the found flag
 * @param slot the slot
 */
static void _obj_sequence_class_throw(uint8_t *actor, uint8_t *found,
                                      uint8_t *slot)
{
    uint16_t type = rd_le_u16(actor + 8u);
    uint8_t beh = (uint8_t)(*slot & 0x3Fu);
    int32_t landed;
    uint32_t cell;

    _obj_sel_type = 0x10u; // 1000:9DDD: [0x423C]
    actor[7] = 0u; // 1000:9DF1
    *slot &= 0x7Fu;
    wr_le_u16(found + 0u,
              (uint16_t)(rd_le_u16(actor + 0u) +
                         (type < 3u ? (int16_t)_obj_throw_dx_word[type] : 0)));
    wr_le_u16(found + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 8u));
    wr_le_u16(found + 4u, beh < 8u ? _obj_drop_frame[beh] : 0u);
    wr_le_u16(found + 6u, 0u);
    wr_le_u16(found + 8u, 0xFFFFu);
    wr_le_u16(found + 0xAu, 0u);
    wr_le_u16(found + 0xCu, 0u);
    wr_le_u16(found + 0xEu, 0u);
    landed =
        _obj_core_step_probe_at(found, 8, 0x10, &cell); // 1000:9E43 -> E389
    if (landed >= 0) wr_le_u16(found + 2u, (uint16_t)(landed - 0xF));
}


/** @brief 1000:9D5D (the class path): the acting record's `+7` is set, so it carries
 * the record whose ordinal that byte holds - `DI = 0x3F17 + 0x12*(class-1)`
 * (the first live record is 0x3F17) and `BP = 0x3E5F + class - 1`.  The
 * carried record's class slot picks the handler through 0x5B97/0x5BA0; the
 * tail then always writes [0x1979] = 5, [0x1988] = 1 and [0x5B56] = 1, i.e.
 * the bar returns to the walk group.
 *
 * @param actor the object record
 */
static void _obj_sequence_op_class_path(uint8_t *actor)
{
    uint8_t cls = actor[7];
    uint32_t live =
        _obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0 : 0u;
    uint8_t *found, *slot;
    uint32_t beh, idx;

    /* the original indexes with the byte unguarded; past the live records it
     * would read the image, which cannot happen - only the take / give paths
     * write it, always with a valid ordinal. */
    if (cls < 1u || cls > live) return;
    found = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE; // 1000:9D5D
    slot = _obj_slot_class + cls; // 1000:9D60
    beh = (*slot & 0x3Fu);
    idx = beh < 8u ? _obj_beh_class_idx[beh] : 0xFFu; // 1000:9D76
    if (idx & 0x80u) return; // 1000:9D7C
    if (idx == 0u) _obj_sequence_class_drop(actor, found, slot);
    else _obj_sequence_class_throw(actor, found, slot);
    _obj_pending = 5u; // 1000:9D84
    _obj_cursor = 1u; // 1000:9D89
    _obj_sequence_set_program_index(1u); // 1000:9D8E
}


/**
 * @brief 0x9CD9: the collision pass's tail.  The program id is the shared
 * scratch word [0x423C] (`_obj_sel_type`): on the scan's success path the loop
 * stamped it from 0x5B8A[behaviour] (1000:9C8D), and the count < 9 branch
 * reaches it without stamping anything in this call (D-115's stale case).
 * A negative low byte leaves all three writes undone (1000:9CDF).
 */
static void _obj_sequence_collide_tail(void)
{
    uint8_t prog = (uint8_t)(_obj_sel_type & 0xFFu);

    if (prog & 0x80u) return; // 1000:9CDF
    _obj_pending = 5u; // [0x1979] = 5
    _obj_cursor = 1u; // [0x1988] = 1
    _obj_sequence_set_program_index(prog); // [0x5B56] = CL
}


/**
 * @brief 0x9C1B (ops 2/6): the collision pass over the live records.
 *
 * The actor's type must be 0xF or <= 2.  Per live record: its slot bit 7 clear,
 * its +6 bit 7 clear, direction 0x5B81[beh] >= 0, |dx| < 0x10 and
 * 0 <= dy < 0x18, then 0x5B93[dir] (0x9CF0 give / 0x9D38 take).  Success:
 * [0x1979] = 5, [0x1988] = 1, [0x5B56] = 1.
 *
 * @param actor the object record
 */
static void _obj_sequence_op_collide(uint8_t *actor)
{
    uint16_t type = rd_le_u16(actor + 8u);
    int32_t ax, bx;
    uint8_t live;

    if (!(type == 0x0Fu || type <= 2u)) return;
    if (actor[7]) { // 1000:9C37 -> the class path
        _obj_sequence_op_class_path(actor);
        return;
    }
    ax = (int16_t)rd_le_u16(actor + 0u);
    bx = (int16_t)rd_le_u16(actor + 2u);
    live = _obj_live_count > OBJ_LIVE0 ? (uint8_t)(_obj_live_count - OBJ_LIVE0)
                                       : 0u;
    /* 1000:9C4E: count < 9 (no live records) jumps straight to the tail. */
    if (live == 0u) {
        _obj_sequence_collide_tail();
        return;
    }

    for (uint16_t k = 0; k < live; k++) {
        uint8_t ordinal = (uint8_t)(k + 1u); // DX
        /* DI starts at 0x3F17 = index 9 = OBJ_LIVE0 + 1 (OBJ_LIVE0 is the
         * count's base, 8). */
        uint8_t *found = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        uint8_t *slot = _obj_slot_class + ordinal; // BP = 0x3E5F + k
        int32_t dx, dy;
        uint32_t beh, dir, prog;

        if (*slot & 0x80u) continue;
        if (found[6] & 0x80u) continue;
        beh = (*slot & 0x3Fu);
        /* 1000:9C89: the tail's program id (0x5B8A[behaviour]) is stamped on
         * every candidate the slot/flag tests let through, before the
         * direction test, so a later empty-map press can read it.
         * 0x5B81 is 8 bytes; behaviours 8/9 (the 0xB7 stack record and the
         * 0xBB marker) read 0xFF/0xFF there, i.e. "skip" - see the table
         * comment.  10..63 read the tables that follow in the image and
         * never occur. */
        prog = beh < 8u ? _obj_beh_collide_prog[beh] : 0xFFu;
        _obj_sel_type = (uint16_t)prog; // [0x423C]
        dir = beh < 8u ? _obj_beh_dir[beh] : 0xFFu;
        if (dir & 0x80u) continue;
        dx = (int16_t)rd_le_u16(found + 0u) - ax;
        if (dx < 0) dx = -dx; // SUB/NEG, then < 0x10
        if (dx >= 0x10) continue;
        dy = (int16_t)rd_le_u16(found + 2u) - bx; // 0 <= dy < 0x18
        if (dy < 0 || dy >= 0x18) continue;
        if (dir == 0u) {
            if (!_obj_sequence_give_found(actor, found, slot, ordinal))
                continue;
        } else if (!_obj_sequence_take_found(actor, found, slot, ordinal))
            continue;
        _obj_sequence_collide_tail();
        return;
    }
}


/**
 * @brief 0x4147 (resolve_object_sprite): [0x3E22] := index, program from the
 * class.
 *
 * 0x1C76[behaviour] maps 4/5/6/7 to programs 6/2/4/3; the player's own record
 * is forced to 5.
 *
 * @param index the index
 */
void _obj_sequence_resolve_sprite(uint32_t index)
{
    uint8_t *cur = _obj_array + _obj_entity * OBJ_STRIDE;
    uint8_t *r = _obj_array + index * OBJ_STRIDE;
    uint8_t prog = 0u;

    if (rd_le_u16(cur + 8u) == 2u)
        wr_le_u16(cur + 4u, 0x0010u); // the previous record
    _obj_entity = (uint16_t)(index);
    if (r[7]) {
        /* 0x1C76 is 8 bytes (the "\humans\" path string follows it); its
         * [8] = 0x5C gives program 0x5D, out of 1..6 like the bound's 0xFF. */
        uint8_t b = _obj_core_class_byte_at(r[7]) & 0x3Fu;

        prog = b < 8u ? _obj_beh_program[b] : 0xFFu;
    }
    if (index == (uint32_t)_obj_player) prog = 4u;
    _obj_sequence_set_program_index(prog + 1u);
    _obj_pending = 1u;
    _obj_sequence_marker_arm(); // 1000:4188
}


uint8_t object_sequence_select_by_index(uint32_t index)
{
    uint8_t *r;

    if (index >= OBJ_STATIC) return 0;
    r = _obj_array + index * OBJ_STRIDE;
    if ((r[6] & 0x80u) || rd_le_u16(r + 8u) == 0x1Du) return 0; // 1000:38CE
    _obj_sequence_resolve_sprite(index); // 1000:38DA
    return 1;
}


uint8_t object_sequence_space_action(void)
{
    if (_obj_command_home_state(_obj_core_actor()))
        return 1; // 1000:392D -> 0xEDB7
    _obj_core_driver_call(0); // 1000:393A
    statusbar_cycle_action(); // 1000:3941
    return 1;
}


int32_t object_sequence_cycle_entity(int32_t step)
{
    int32_t at = (int32_t)_obj_entity;

    /* 1000:4113 BX = 6 and the DEC BX / JNS leave: seven candidates, the
     * records other than the current one (the eighth would be itself). */
    for (uint16_t n = 0; n + 1u < OBJ_STATIC; n++) {
        int16_t idx = (int16_t)(at + (step < 0 ? -1 : 1));
        uint8_t *r;

        if (idx < 0) idx = (int32_t)OBJ_STATIC - 1;
        if (idx >= (int32_t)OBJ_STATIC) idx = 0;
        r = _obj_array + idx * OBJ_STRIDE;
        if (!(r[6] & 0x80u) && rd_le_u16(r + 8u) != 0x1Du) {
            _obj_sequence_resolve_sprite((uint32_t)idx);
            return idx;
        }
        at = idx;
    }
    return -1;
}


/** @brief test_class_against_6eb6 (0xF2CF): ZF set when the acting record's behaviour
 * has this state.  The callers are `CALL`/`JNZ`.
 *
 * @param actor the object record
 * @param state the flow state
 * @return 1 when the behaviour state matches, 0 otherwise
 */
int32_t _obj_sequence_state_is(uint8_t *actor, uint32_t state)
{
    uint8_t b = actor[7u];
    uint32_t beh;

    if (!b) return 0;
    beh = _obj_core_class_byte_at(b) & 0x3Fu;
    return beh < sizeof _obj_beh_state && _obj_beh_state[beh] == state;
}


/** @brief 0xF989 (object_record_walk): the live record with slot 3 (behaviour 3) that
 * sits left of `x + step` and above `y + 0xC`, i.e. -0x10 <= dx < 0 and
 * -0x18 <= dy < 0.  Returns the slot index or -1.
 *
 * @param actor the object record
 * @param step the step
 * @return the record index, or -1 when none
 */
static int32_t _obj_sequence_record_walk(uint8_t *actor, uint32_t step)
{
    int32_t cx = (int16_t)rd_le_u16(actor + 2u) + 0x0C;
    int32_t bx = (int16_t)rd_le_u16(actor + 0u) + (int32_t)step;
    uint32_t live =
        _obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0 : 0u;

    for (uint16_t k = 0; k < live; k++) {
        uint8_t *r = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        int32_t dx, dy;

        if (_obj_slot_class[1u + k] != 3u) continue; // 1000:F99F, whole byte
        if (r[6] & 0x80u) continue;
        if (rd_le_u16(r + 8u) == 2u) continue;
        dx = (int16_t)rd_le_u16(r + 0u) - bx;
        if (!(dx < 0 && dx + 0x10 >= 0)) continue;
        dy = (int16_t)rd_le_u16(r + 2u) - cx;
        if (!(dy < 0 && dy + 0x18 >= 0)) continue;
        return (int32_t)(OBJ_LIVE0 + 1u + k); // BP = the record
    }
    return -1;
}


/** @brief 0xEFA1 / 0xEFB9 (op 4): set the record's fixed state, gated on the
 * behaviour's state byte being 1 (behaviour 5, the spear).
 *
 * @param actor the object record
 * @param type the type
 */
static void _obj_sequence_op_set_state(uint8_t *actor, uint32_t type)
{
    if (!_obj_sequence_state_is(actor, 1u)) return;
    if (type == 0u) {
        wr_le_u16(actor + 4u, 0x00BBu); // 1000:EFA9
        wr_le_u16(actor + 8u, 0x0017u);
    } else {
        wr_le_u16(actor + 4u, 0x00BEu); // 1000:EFC1
        wr_le_u16(actor + 8u, 0x0018u);
    }
    _obj_sequence_active = 0; // 1000:EFB3
}


/**
 * @brief Find the record's overlap target (1000:F2E0).
 *
 * Each candidate is dissolved inside the walk (`0xF453` with `DX = 5`), and
 * the `0x6EC3` gate is re-read afterwards: a dissolve can leave the record's
 * `+8` in the non-overlappable set, and the original then skips it.
 *
 * @param actor the object record
 */
static void _obj_sequence_find_overlap(uint8_t *actor)
{
    int32_t ax = (int16_t)rd_le_u16(actor + 0u);
    int32_t bx = (int16_t)rd_le_u16(actor + 2u);

    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint8_t *found;
        uint16_t type;
        int32_t dx, dy;

        if (r == actor) continue;
        if (r[6] & 0x80u) continue;
        dx = (int16_t)rd_le_u16(r + 0u) - ax;
        if (dx < 0) dx = -dx;
        if (dx > 0x10) continue;
        dy = (int16_t)rd_le_u16(r + 2u) - bx;
        if (dy < 0) dy = -dy;
        if (dy > 0x10) continue;
        found = (i == (uint32_t)_obj_player) ? actor : r;
        type = rd_le_u16(found + 8u);
        if (object_core_type_blocked(type)) continue;
        /* 1000:F34F: dissolve the group of [0x4254], the record the
         * conversion is about to hit (the actor when the candidate is the
         * player).  Then 1000:F35C re-tests the gate. */
        (void)object_units_stack_scan_dissolve((uint32_t)(found - _obj_array) /
                                               OBJ_STRIDE);
        type = rd_le_u16(found + 8u);
        if (object_core_type_blocked(type)) continue;
        wr_le_u16(found + 4u, 0x0103u);
        wr_le_u16(found + 8u, 0x001Eu);
        wr_le_u16(found + 0xAu, 0x0006u);
        wr_le_u16(found + 0xCu, 0x0000u);
        wr_le_u16(found + 0xEu, 0x000Au);
    }
}


/** @brief 0xEF0C / 0xEF39 (op 9, non-player): mark the walked record as state 1,
 * gated on the behaviour's state byte being 3 (behaviour 6), then run
 * object_find_overlap (0xF2E0).
 *
 * @param actor the object record
 * @param step the step
 */
static void _obj_sequence_op_mark_walk(uint8_t *actor, uint32_t step)
{
    int32_t slot;

    if (!_obj_sequence_state_is(actor, 3u)) return;
    slot = _obj_sequence_record_walk(actor, step);
    /* 1000:EF1A/0xEF47: the found record (a behaviour-3 bush) is marked type
     * 1, but **both** the found and the not-found paths fall into 0xEF33 /
     * 0xEF60, i.e. object_find_overlap always runs.  The burn is that
     * overlap: any placed record within 0x10 of the actor becomes 0x103 /
     * type 0x1E, bush or human.  Returning here when no bush was found was a
     * port bug (the torch could never burn a human with no bush in front). */
    if (slot >= 0) {
        uint8_t *r = _obj_array + (uint32_t)slot * OBJ_STRIDE;

        if (rd_le_u16(r + 8u) != 1u) {
            wr_le_u16(r + 8u, 0x0001u);
            wr_le_u16(r + 0xAu, 0x0003u);
            wr_le_u16(r + 0xCu, 0x0000u);
        }
    }
    _obj_sequence_find_overlap(actor); // 1000:EF33/0xEF60
}


/** @brief 1000:E3F6's input normalisation: swap bits 2 and 3, drop bits 4..6, then kill
 * each contradictory pair (0x3 / 0xC) the way the original does.
 *
 * @param in the input state
 * @return the normalised input byte
 */
uint8_t _obj_sequence_normalise_input(uint8_t in)
{
    uint8_t v =
        (uint8_t)((in & (JOYBITS_FIRE | JOYBITS_UP | JOYBITS_DOWN)) |
                  ((in & JOYBITS_RIGHT) >> 1) | ((in & JOYBITS_LEFT) << 1));

    if ((v & (JOYBITS_UP | JOYBITS_DOWN)) == (JOYBITS_UP | JOYBITS_DOWN))
        v &= (uint8_t)~JOYBITS_DOWN;
    if ((v & (JOYBITS_LEFT | JOYBITS_RIGHT)) == (JOYBITS_LEFT | JOYBITS_RIGHT))
        v &= (uint8_t)~JOYBITS_LEFT; // the swapped RIGHT bit is in bit 2
    return v;
}


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_sequence_find_overlap_for_probe(uint8_t *actor)
{ _obj_sequence_find_overlap(actor); }
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 0xEA71 (op 8): on odd frames reset the span entry of the acting record.
 * On even frames the original also clears [0x13A4]; the port's equivalent is
 * game_main's one-action-per-press latch (poll_input masks the ACTION bit
 * while [0x13A4] is set and clears it on release, 1000:9998).
 */
static void _obj_sequence_op_reset_span(void)
{
    if (!(_obj_frame & 1u)) return;
    for (uint16_t i = 0; i < 2u; i++) {
        uint16_t *e = _obj_span[i];
        if (e[0] != _obj_entity) continue;
        /* 1000:EA94..0xEAA1: +8 != 0 clears outright, and with +8 == 0 the
         * `CMP [+6],[+8] / JBE` only leaves the entry alone when +6 is also 0. */
        if (e[4] != 0u || e[3] != 0u) {
            e[4] = 0u;
            e[9] = 0u;
        }
        return;
    }
}


/** @brief 1000:3747 with the acting record's +0xE (the bar drawing lives in
 * statusbar.c, which owns the band and panel4).
 */
static void _obj_sequence_charge_draw(void)
{
    uint8_t *actor = object_core_slot(object_sequence_action_entity());
    uint16_t v;

    if (!actor) return;
    /* 1000:36DB's entry: 0x99D8/0x9B7D/0x9A18 restore the band over the
     * action row and draw the trough before the strokes.  The port recomposes
     * the page each tick, so replay it on every charging frame. */
    statusbar_charge_begin();
    /* The original's charge loop accumulates one stroke per frame (1000:3702
     * -> 0x3747, +0xE stepping by 4); its page is never rebuilt, so the
     * strokes stay.  The port recomposes the page every tick, so it redraws
     * the whole run - the same pixels the original accumulated (D-81). */
    v = (uint16_t)((rd_le_u16(actor + 0xEu) & 0x1FFu));
    statusbar_charge_bar(0u); // 1000:36F7
    statusbar_charge_bar(2u); // 1000:36FF
    for (uint16_t x = 4u; x <= v; x += 4u) statusbar_charge_bar(x);
}


/** @brief The pending-action dispatch ([0x1979] -> the table at 0x197B).
 *
 * Runs once per play frame, after scan_id_list (which skips its whole walk
 * while a code is armed, 1000:386E), and debounces a repeat of the previous
 * code ([0x197A], 1000:34D1..0x34D5).  The handlers end a debounce: the
 * code-1 lock (0x36CD), the code-5 rewind (0x3839) and the code-4 loop's exit
 * (0x373B) store a *word* 0 at 0x1979, which also clears 0x197A.  Code 5
 * rewinds the program cursor to [0x1988] (1000:382D); the bar redraw that
 * follows is statusbar.c's per-tick draw_actions().
 */
void object_sequence_pending_run(void)
{
    uint8_t code = _obj_pending;

    _obj_pending = 0u;
    if (code == 0u) return;
    /* 1000:3705 -> 0x3735/0x373B: a code queued by the running charge loop's
     * own update_world frame is consumed by the loop, not dispatched, and the
     * `MOV word [0x1979],0` at 0x373B also resets the debounce byte 0x197A. */
    if (_obj_charging) {
        _obj_charging = 0;
        _obj_last_result = 0u;
        /* 1000:3738: 0x98F8/0x990A redraw the action row on the loop's exit */
        statusbar_charge_end();
        return;
    }
    /* 1000:34D1: the code the previous dispatch ran is not run again. */
    if (code == (uint32_t)_obj_last_result) return;
    _obj_last_result = code; // 1000:34D5
    switch (code) {
    case 1u: // 1000:36C7: the lock arm clears the dispatch word (0x36CD)
        _obj_last_result = 0u;
        break;
    case 4u: _obj_charging = 1; break; // 1000:36DB
    case 5u: { // 1000:382D
        uint8_t *p = _obj_core_program_slot(_obj_sequence_program_id());

        if (p) p[1] = _obj_cursor; // D-186
        _obj_last_result = 0u; // 1000:3839: MOV word [0x1979],0
        break;
    }
    default: break; // 1000:36C7/36D7/36DA/3843
    }
}


uint8_t object_sequence_charge_active(void) { return _obj_charging; }


uint8_t object_sequence_charge_tick(void)
{
    if (!_obj_charging) return 0;
    if (!_obj_charge_arm) { // 1000:370C
        _obj_charging = 0; // the loop leaves; 0x373B
        /* 0x373B is `MOV word [0x1979],0`: it clears the debounce byte
         * 0x197A as well, so the next turn re-arms event 4. */
        _obj_last_result = 0u;
        statusbar_charge_end(); // 1000:3738: 0x98F8/0x990A
        return 0;
    }
    _obj_charge_arm = 0u; // 1000:3713
    _obj_sequence_charge_draw(); // 1000:3718-0x3730
    return 1;
}


void object_sequence_action_run(void)
{
    uint8_t *p = _obj_core_program_slot(_obj_sequence_program_id());

    /* 1000:3523/0x3530: pause_toggle returns while a sequence owns the
     * record ([0x6F1D] != 0), so the held frames keep reaching the type row's
     * fire half (the turn) and the charge keeps advancing.  During a charge
     * [0x6F1D] is exactly what blocks this call, so there is no separate
     * "press ends the charge" rule: the release ends it, when the fire
     * column stops arming [0x1987]. */
    if (_obj_sequence_active) return;
    _obj_core_driver_call(0); // 1000:3979 script_op_dispatch's entry
    object_core_set_action_latched(1);
    if (!p) return; // D-186: nothing to dispatch

    uint8_t *actor = _obj_array + _obj_entity * OBJ_STRIDE;
    /* 1000:3987: cursor = [SI+1] (after the two INCs), op = [SI+1+cursor]. */
    uint8_t op =
        object_sequence_action_byte(1u + object_sequence_action_byte(1u));

    switch (op) {
    case OP_REGISTER: _obj_sequence_op_register(actor); break;
    case OP_COLLIDE_A:
    case OP_COLLIDE_B: _obj_sequence_op_collide(actor); break;
    case OP_SETFRAME: _obj_sequence_op_set_frame(actor); break;
    /* 0x39E4: the player claims recipe 2; the others (type 0/1, i.e. walking)
     * run the ignite. */
    case OP_CLAIM_OR_TYPE:
        if (_obj_player >= 0 && actor == _obj_array + _obj_player * OBJ_STRIDE)
            (void)object_sequence_claim_begin(2u, (int32_t)_obj_entity);
        else if (rd_le_u16(actor + 8u) < 2u) // 1000:39F3 CMP AX,2 / JNC
            _obj_sequence_op_mark_walk(actor,
                                       rd_le_u16(actor + 8u) ? 0u : 0x10u);
        break;
    case OP_CLAIM3:
        (void)object_sequence_claim_begin(3u, (int32_t)_obj_entity);
        break;
    case OP_CLAIM0:
        (void)object_sequence_claim_begin(0u, (int32_t)_obj_entity);
        break;
    case OP_CLAIM1:
        (void)object_sequence_claim_begin(1u, (int32_t)_obj_entity);
        break;
    case OP_TYPE_A: // 0x3A05 -> 0xEFA1 / 0xEFB9
        if (rd_le_u16(actor + 8u) < 2u) // 1000:3A08 CMP AX,2 / JNC
            _obj_sequence_op_set_state(actor, rd_le_u16(actor + 8u) ? 1u : 0u);
        break;
    case OP_RESET: // 0x39CC -> 0xEA71
        _obj_sequence_op_reset_span();
        break;
    case OP_SELECT: // 0x3A1A -> 0xECFE
        /* 0xECFE: needs a class, then page_offset_to_ptr (0xF123) on the map
         * cell under the record and on the cell one to the right (type 0) or
         * left; only when both are free does it set type 0x12/frame 0x8C
         * (type 0) or type 0x14/frame 0x91.  `1000:ED1D..0xED3E` forms
         * `DI = [0x1FA0] + 2*(y & 0xFFF0) + (x >> 4)` and takes the
         * neighbour as `DI +/- 1`, with no range test - the map buffer's gap
         * and second copy are legitimate.  The original's single exit
         * (1000:ED49) clears the sequence latch [0x6F1D] on every path and
         * nothing in the body reads it, so the clear is done here. */
        _obj_sequence_active = 0;
        {
            uint16_t t = rd_le_u16(actor + 8u);
            int32_t step = (t == 0u) ? 1 : -1;
            uint16_t frame = (t == 0u) ? 0x8Cu : 0x91u;
            const uint8_t *map = level_map();
            int32_t cell =
                (int32_t)(uint16_t)(2u * ((uint32_t)rd_le_u16(actor + 2u) &
                                          0xFFF0u)) +
                (int32_t)((uint32_t)rd_le_u16(actor + 0u) >> 4);
            int32_t next = cell + step;

            if (!actor[7u] || t >= 2u || !map) break; // 1000:3A1D CMP/JNC
            if (!level_page_free(_obj_core_map_buffer_byte(cell))) break;
            if (!level_page_free(_obj_core_map_buffer_byte(next))) break;
            wr_le_u16(actor + 8u, (uint16_t)((t == 0u) ? 0x12u : 0x14u));
            wr_le_u16(actor + 4u, frame);
        }
        break;
    case OP_TYPE_B: // 0x39CF -> 0xEA0F / 0xEA4D
        /* 1000:39CF: `CMP AX,2 / JNC` then BP = 0xEA0F for type 0 and
         * 0xEA4D otherwise - the two ride bodies, `cmd_ride_ab` /
         * `cmd_ride_ad`, already ported as `_obj_command_ride` (the same bodies
         * the type rows reach as command 8). */
        {
            uint16_t t = rd_le_u16(actor + 8u);

            if (t < 2u) { // 1000:39D2 CMP AX,2 / JNC
                if (t == 0u)
                    _obj_command_ride(actor, 0xABu, 0xCu, 9u); // 1000:EA0F
                else _obj_command_ride(actor, 0xADu, 2u, 0xAu); // 1000:EA4D
            }
        }
        break;
    default: break;
    }
}
