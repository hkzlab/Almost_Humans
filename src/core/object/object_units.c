#include "object_internal.h"
#include "object_core.h"
#include "object_sequence.h"
#include "object_units.h"
#include "entity.h"
#include "level/level.h"
#include "statusbar.h"
#include "byteorder.h"
#include <string.h>

/* forward declarations */
static void _obj_units_stack_mark(uint8_t *actor, const uint8_t *slot);
static void _obj_units_frame_step(uint8_t *actor, uint32_t lo, uint32_t hi);
static int32_t _obj_units_ter_rect(uint8_t *e, int32_t bx, int32_t cx,
                                   uint8_t bit);
static void _obj_units_frame_step_down(uint8_t *actor, uint32_t lo,
                                       uint32_t hi);
static uint32_t _obj_units_anim_list_word(uint32_t off);
static void _obj_units_level_end_goal(uint8_t *t, uint32_t idx);
static void _obj_units_level_end_probe(uint8_t *actor, int32_t bx);
static uint32_t _obj_units_tab_byte(const uint8_t *t, uint32_t n, uint32_t off);
static void _obj_units_tail_frame_to_type(uint8_t *actor);
static void _obj_units_tail_crash(uint8_t *actor);
static void _obj_units_tail_type3_coast(uint8_t *actor, uint32_t dx);
static void _obj_units_tail_type3_land(uint8_t *actor, uint32_t bx);
static uint8_t *_obj_units_span_owner(uint8_t *actor, int32_t *yes);
static void _obj_units_tail_walk_to_frame(uint8_t *actor, uint32_t frame,
                                          const uint8_t *span);
static uint8_t *_obj_units_span_entry(uint32_t off);
static uint32_t _obj_units_type_tile_byte(uint32_t type);
static void _obj_units_fall_hard_landing(uint8_t *actor);
static void _obj_units_fall_land_at_y(uint8_t *actor, int32_t bx);
static void _obj_units_fall_land_pair(uint8_t *actor);
static void _obj_units_fall_place(uint8_t *actor, int32_t ax, int32_t bx);
static void _obj_units_fall_item_word(uint8_t *actor, int32_t ax, int32_t bx);
static void _obj_units_group_restore_member(uint8_t *rec);
static void _obj_units_group_clear_list(uint8_t *e);
static void _obj_units_group_dissolve(uint8_t *e);
static void _obj_units_class_transfer(uint32_t recIndex);


/* The settle's static data: the per-type gate at 0x6EBE (32 bytes) and the two
 * 32-byte footing offset tables at 0x689E / 0x68BE, copied from the image. */
static const uint8_t _obj_type_gate[32] = {
    0x0F, 0xFF, 0xC2, 0x1D, 0xA0, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
};


/* DS:0x6E5C - the frame list 0xECB7 walks: twelve ramp words and then 3-word
 * vectors from 0x6E74. */
static const uint16_t _obj_anim_list[0x1A] = {
    0x0010, 0x007F, 0x0080, 0x0081, 0x0080, 0x0081, 0x0080, 0x0081, 0x0080,
    0x0081, 0x007F, 0x0010, 0xFFFF, 0x0003, 0xFFFB, 0x0000, 0x0000, 0x0002,
    0xFFF9, 0x0000, 0x0000, 0x0001, 0xFFF7, 0x0000, 0x0000, 0x0000,
};


/* DS:0x6868 - the 20-byte frame table 0xDBCD walks (the frame is a table byte,
 * `+0xC` its cursor), and DS:0x687C - the 4-byte {dx, dy, flag, hold} table
 * 0xDE7E walks. */
static const uint8_t _obj_type28_frame_tab[0x14] = {
    0xCD, 0xCE, 0xCD, 0xCE, 0xCD, 0xCE, 0xCF, 0xD0, 0xCF, 0xD0,
    0xCF, 0xD0, 0xD1, 0xD2, 0xD1, 0xD2, 0xD1, 0xD2, 0xD3, 0x00,
};

static const uint8_t _obj_type30_step_tab[0x20] = {
    0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01,
    0x06, 0xFF, 0x32, 0x01, 0x06, 0xFF, 0x32, 0x01, 0x06, 0x01, 0x06,
    0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0xFF, 0x07, 0x01, 0x07,
};


/* --- 1000:F6F6: the landing / bounce machine (handlers 0xF067 / 0xF078) ---
 *
 * F3ED gates it.  Per tick, +0xA counts down and advances +4; +0xC indexes the
 * 0x6EF3 curve and accumulates in +0xE (vy); +0x10 is vx; the probe at
 * (x + vx + 8, y + vy + 0x18) decides:
 *
 *   F76C  0xF0..0xF2 above is a platform: snap x/y, frame 0xCD, type 0x1C
 *   F7BB  feet == the type's 0x6F0B byte and the cell below's page = 0: land
 *   F7F3  page < 0: retry one column right (+0x10), else F8EC
 *   F834  page = 0: the row above (its type match lands, else its page)
 *   F87F  page > 0: snap within 3 px of the surface, else the item word
 *   F8A1  vy < 0: land (F8D2); vy >= 0x30: hard landing (F8AD); else arm
 *         [0x2D8E] = vx >> 3 and land with effect 5
 *   F8EC  the item word's fine-x bit: nonzero -> F975 (y only), else F966
 *
 * [0x4230]/[0x4234] are the caller's frame/type, [0x4238] the item probe's x
 * offset and [0x423C] the scratch page byte / item word.
 */
static const int8_t _obj_fall_step[0x19] = {
    /* 202F:6EF3: -2 -2 -1 x5, ten 0 bytes (0x6EFA..0x6F03), +1 x5, +2 +2;
     * index 0x18 is the clamp entry the f71b `CMP AX,0x18` bypasses. */
    -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  1,  1,  1,  1, 1, 2, 2, 0,
};

/* 0x6EF2..0x6F14: the feet-cell comparison byte, indexed by the *type* (the
 * code reads 0x6F0B + (type - 0x19), which for types 0..0x18 wraps back into
 * 0x6EF3's tail; the flat table below is the same bytes).  Entries 0x19/0x1A
 * are the image's pre-load bytes only: `level_setup_screen` writes the level's
 * field-K second word over 0x6F0B/0x6F0C (1000:4E99), so those two are read
 * through `_obj_units_type_tile_byte` instead. */
static const uint8_t _obj_type_tile[35] = {
    0x96, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x02, 0x00, 0x00, 0x49, 0xFA, 0x64, 0xFA, 0x78, 0xFA, 0x78, 0xFA,
};


/** @brief The original's `1 << (7 - n)` on a 16-bit register: for n > 7 the count is
 * negative, and an 8086 SHL by a count >= 16 shifts every bit out, so the bit is
 * 0.  Records 0..7 therefore own 0x80..0x01 and 8..14 own nothing.
 *
 * @param idx the index
 * @return the bit mask, or 0 past bit 7
 */
uint32_t _obj_units_index_bit(uint32_t idx)
{ return idx <= 7u ? (1u << (7u - idx)) : 0u; }


/**
 * @brief 1000:F3CA: clear the record's bit from both masks.
 *
 * @param actor the object record
 */
void _obj_units_stack_unmark(uint8_t *actor)
{
    uint8_t keep = (uint8_t)~_obj_units_index_bit(
        (uint32_t)(actor - _obj_array) / OBJ_STRIDE);

    /* 0x64AD / 0x64C1 are the TER slots' +0x12 high bytes. */
    _obj_ter_slot[0][0x13] &= keep;
    _obj_ter_slot[1][0x13] &= keep;
}


/** @brief 1000:F38F test_bits_64ad_64c1: mark the record as being in a stack, in the
 * mask that the list byte below it (`slot[-1]`) is marked in; when that byte is
 * a terminator or unmarked, clear both instead.
 *
 * @param actor the object record
 * @param slot the slot
 */
static void _obj_units_stack_mark(uint8_t *actor, const uint8_t *slot)
{
    uint8_t below = slot[-1];

    if (!(below & 0x80u)) {
        uint8_t bit = (uint8_t)(_obj_units_index_bit(below));

        for (uint16_t m = 0; m < 2u; m++) {
            if (_obj_ter_slot[m][0x13] & bit) {
                _obj_ter_slot[m][0x13] |= (uint8_t)_obj_units_index_bit(
                    (uint32_t)(actor - _obj_array) / OBJ_STRIDE);
                return;
            }
        }
    }
    _obj_units_stack_unmark(actor);
}


/** @brief 1000:FD91: the walk frame step - `y--`, every 5th frame advance the frame by
 * +/-1 (`+0xC` is the direction) and reverse at the `lo`/`hi` bounds.  Both
 * leaves take this on a `y` mismatch.
 *
 * @param actor the object record
 * @param lo the low word
 * @param hi the high word
 */
static void _obj_units_frame_step(uint8_t *actor, uint32_t lo, uint32_t hi)
{
    uint16_t f;

    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) - 1u));
    wr_le_u16(actor + 0xAu, (uint16_t)(rd_le_u16(actor + 0xAu) - 1u));
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 0xAu, 0x0004u);
    f = rd_le_u16(actor + 4u);
    if (actor[0xCu] != 0u) {
        if (--f == lo) actor[0xCu] = 0u;
    } else {
        if (++f == hi) actor[0xCu] = 0xFFu;
    }
    wr_le_u16(actor + 4u, f);
}


int32_t object_units_stack_push(uint8_t *actor)
{
    uint16_t ax = rd_le_u16(actor + 0u);
    uint16_t bx = (uint16_t)(rd_le_u16(actor + 2u) + 0x18u);

    for (uint16_t i = 0; i < 8u; i++) { // `CX = 7` + `JNS`: 8
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint16_t ex, ey, dx;
        uint8_t *di;
        int32_t de;

        ex = rd_le_u16(e + 0u);
        if (ex & 0x8000u) continue; // free
        de = (int32_t)(int16_t)ex - (int32_t)(int16_t)ax;
        if (de < 0) de = -de;
        if (de >= 8) continue; // |entry.x - actor.x| < 8
        ey = rd_le_u16(e + 2u);
        if (bx >= ey) return 0; // `JNC 0xE6FA`: not under it
        dx = ey;
        di = e + 5; // the owner byte
        while (1) {
            dx -= 0x10u; // 16 px per member
            if (di[0] & 0x80u) {
                di++;
                break;
            } // the terminator
            di++;
        }
        dx += 0x18u;
        if (bx < dx) return 0; // `JC 0xE6FA`
        di--; // the terminator's slot
        *di = (uint8_t)_obj_entity; // `[0x3E22]` into the list
        /* the original stores the slot's pointer; the port stores its offset in
         * the register, the same relation the 0x3E24 paths use for records */
        wr_le_u16(actor + 0xEu,
                  (uint16_t)((uintptr_t)di - (uintptr_t)_obj_register));
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 1u));
        wr_le_u16(actor + 0u, ex);
        wr_le_u16(actor + 4u, 0x0025u);
        wr_le_u16(actor + 8u, 0x0007u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        _obj_units_stack_mark(actor, di); // 1000:F38F
        return 1;
    }
    return 0;
}


int32_t object_units_stack_pop(uint16_t ax, uint8_t **below, uint8_t **entry)
{
    *below = NULL;
    if (entry) *entry = NULL;
    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di = e;
        int8_t bl;

        if (entry) *entry = e; // 1000:FDF7: DX = the entry

        if (rd_le_u16(di + 0u) & 0x8000u) continue; // free
        di += 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)_obj_entity) di++;
        if (di[0] & 0x80u) continue; // not in this entry
        di++; // past the match
        if (!(di[0] & 0x80u)) return -1; // a member above it
        bl = (int8_t)di[-2]; // the list byte below
        if (bl >= 0) {
            uint8_t *nb = object_core_slot((uint32_t)bl);
            int32_t dx =
                nb ? (int32_t)(int16_t)rd_le_u16(nb + 0u) - (int32_t)(int16_t)ax
                   : 8;

            if (dx < 0) dx = -dx;
            if (nb && dx < 8) return 0; // already aligned: no removal
        }
        di[-1] = 0xFFu; // terminate at the match
        bl = (int8_t)di[-2];
        if (bl < 0) { // nothing below: free the entry
            wr_le_u16(di - 6 + 0u, 0xFFFFu);
            wr_le_u16(di - 6 + 2u, 0xFFFFu);
            return 1;
        }
        *below = object_core_slot((uint32_t)bl);
        return 1;
    }
    return -1;
}


int32_t object_units_stack_grab(uint8_t *actor)
{
    uint16_t ax = rd_le_u16(actor + 0u);
    uint16_t bx = rd_le_u16(actor + 2u);
    uint8_t *lastFree = NULL;

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint16_t ex = rd_le_u16(e + 0u), ey;
        uint8_t *di;
        int32_t d;

        if (ex & 0x8000u) {
            lastFree = e + 2;
            continue;
        } // `[0x423C] = DI`
        d = (int32_t)(int16_t)ex - (int32_t)(int16_t)ax;
        if (d < 0) d = -d;
        if (d >= 8) continue; // |entry.x - actor.x| < 8
        ey = rd_le_u16(e + 2u);
        d = (int32_t)(int16_t)ey - (int32_t)(int16_t)bx;
        if (d < 0) d = -d;
        if (d >= 4) continue; // |entry.y - actor.y| < 4
        di = e + 5;
        while (!(di[0] & 0x80u)) di++; // to the terminator
        *di = (uint8_t)_obj_entity;
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) - 1u));
        wr_le_u16(actor + 0u, ex);
        wr_le_u16(actor + 4u, 0x0023u);
        wr_le_u16(actor + 8u, 0x0006u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        _obj_units_stack_mark(actor, di); // 1000:F38F
        return 1;
    }
    if (!lastFree) return -1;
    wr_le_u16(lastFree + 0u, bx); // entry + 2: y
    lastFree[3] = (uint8_t)_obj_entity; // entry + 5: the owner
    wr_le_u16(lastFree - 2 + 0u, ax); // entry + 0: x
    wr_le_u16(actor + 4u, 0x0020u);
    wr_le_u16(actor + 8u, 0x0008u);
    return 0;
}


int32_t object_units_stack_step_off(uint8_t *actor)
{
    uint8_t idx = (uint8_t)((actor - _obj_array) / OBJ_STRIDE);
    uint16_t ax = rd_le_u16(actor + 0u);

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;
        uint16_t ey;

        if (rd_le_u16(e + 0u) != ax) continue;
        ey = rd_le_u16(e + 2u);
        di = e + 6; // the first member
        while (!(di[0] & 0x80u) && di[0] != idx) {
            ey -= 0x10u; // 1000:E896: one level up
            di++;
        }
        if (di[0] & 0x80u) continue;
        ey -= 0x18u; // 1000:E89B
        if (ey != rd_le_u16(actor + 2u)) {
            _obj_units_frame_step(actor, 0x23u, 0x25u); // 1000:E8D4
            return -1;
        }
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 8u));
        wr_le_u16(actor + 4u, 0x0020u);
        wr_le_u16(actor + 8u, 0x0008u);
        if (!(di[-1] & 0x80u)) {
            uint8_t *nb = object_core_slot(di[-1]);

            if (nb) {
                wr_le_u16(nb + 0u, ax);
                wr_le_u16(nb + 2u, (uint16_t)(rd_le_u16(nb + 2u) + 8u));
                wr_le_u16(nb + 4u, 0x0021u);
            }
        }
        return 1;
    }
    return 0;
}


int32_t object_units_stack_leave(uint8_t *actor)
{
    uint8_t idx = (uint8_t)((actor - _obj_array) / OBJ_STRIDE);
    uint16_t ax = rd_le_u16(actor + 0u);

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;

        if (rd_le_u16(e + 0u) != ax) continue;
        di = e + 6;
        while (!(di[0] & 0x80u) && di[0] != idx) di++;
        if (di[0] & 0x80u) continue;
        if (rd_le_u16(e + 2u) != rd_le_u16(actor + 2u)) {
            /* 1000:E934: the *down* stepper (y += 1, frames 0x23..0x25) - the
             * member climbs down the column 1 px a step.  (The step-off's
             * 1000:E8D4 is the one that uses the up stepper.) */
            _obj_units_frame_step_down(actor, 0x23u, 0x25u);
            return -1;
        }
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
        {
            uint16_t off = rd_le_u16(actor + 0xEu);

            if (off < sizeof _obj_register) // 1000:E922
                ((uint8_t *)_obj_register)[off] = 0xFFu;
        }
        return 1;
    }
    return 0;
}


int32_t object_units_stack_stand(uint8_t *actor)
{
    uint8_t idx = (uint8_t)((actor - _obj_array) / OBJ_STRIDE);

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;
        int8_t bl;

        if (rd_le_u16(e + 0u) & 0x8000u) continue;
        di = e + 5;
        while (!(di[0] & 0x80u) && di[0] != idx) di++;
        if (di[0] & 0x80u) continue;
        di++;
        if (!(di[0] & 0x80u)) return 0; // a member above it
        di--; // the match
        wr_le_u16(actor + 0xEu,
                  (uint16_t)((uintptr_t)di - (uintptr_t)_obj_register));
        di--;
        bl = (int8_t)di[0];
        if (bl < 0) return 0;
        {
            uint8_t *nb = object_core_slot((uint32_t)bl);

            if (nb) {
                wr_le_u16(nb + 2u, (uint16_t)(rd_le_u16(nb + 2u) - 8u));
                wr_le_u16(nb + 4u, 0x0020u);
            }
        }
        wr_le_u16(actor + 0u, rd_le_u16(e + 0u));
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) - 7u));
        wr_le_u16(actor + 4u, 0x0025u);
        wr_le_u16(actor + 8u, 0x0007u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        return 1;
    }
    return 0;
}


int32_t object_units_stack_snap(uint8_t *actor)
{
    uint8_t idx = (uint8_t)((actor - _obj_array) / OBJ_STRIDE);

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;

        if (rd_le_u16(e + 0u) & 0x8000u) continue;
        di = e + 5;
        while (!(di[0] & 0x80u) && di[0] != idx) di++;
        if (di[0] & 0x80u) continue;
        di++;
        if (!(di[0] & 0x80u)) return 0; // a member above it
        wr_le_u16(actor + 0u, rd_le_u16(e + 0u));
        wr_le_u16(actor + 4u, 0x0020u);
        return 1;
    }
    return 0;
}


/** @brief 1000:F423: is (BX, CX) inside a TER slot's attach box?
 *
 * A match ORs `bit` into the slot's +0x13 carry byte and returns 1 (the
 * caller's JS: the settle is refused, so the fall machine takes the platform
 * landing).  The box is x in (slot.x - 0x10, slot.x + 0xA] and y in
 * [slot.y - 9, slot.y - 5]; bit 15 of +0x10 is the disabled sentinel.  F3ED
 * calls this for the two 0x649A/0x64AE slots.
 *
 * The original stores the type-derived bit here; 1000:C8A4 applies the mask
 * to record indices, and every 0xAA human has type 2, so the original always
 * carries record 2.  The port stores the acting record's own bit (D-227), so
 * the human on the platform is the one that rides.
 *
 * @param e the TER slot
 * @param bx the x value
 * @param cx the y value
 * @param bit the acting record's mask bit
 * @return 1 when the point is inside the window, 0 otherwise
 */
static int32_t _obj_units_ter_rect(uint8_t *e, int32_t bx, int32_t cx,
                                   uint8_t bit)
{
    int32_t d;

    if (rd_le_u16(e + 0x10u) & 0x8000u) return 0; // 1000:F425
    d = (int16_t)rd_le_u16(e + 0u) - 0x10 - bx; // 1000:F42C
    if (d >= 0) return 0;
    d += 0x1A; // 1000:F433
    if (d < 0) return 0;
    d = (int16_t)rd_le_u16(e + 2u) - cx; // 1000:F438
    if (d < 0) return 0;
    d -= 5;
    if (d < 0 || d > 4) return 0;
    if (bit) e[0x13] |= bit; // 1000:F449
    return 1;
}


/** @brief 1000:F3ED: clear the stack bits, then - unless the actor's type is marked at
 * 0x6EBE - let the two TER slots claim it.  Returns 1 when the settle may run
 * (the original's ZF); a slot match sets the carry bit and returns 0.
 *
 * @param actor the object record
 * @return 1 when the settle may run, 0 otherwise
 */
int32_t _obj_units_settle_gate(uint8_t *actor)
{
    uint16_t t = rd_le_u16(actor + 8u);
    uint8_t bit = (uint8_t)(1u << (7u - (t & 7u)));
    uint8_t rec_bit = (uint8_t)_obj_units_index_bit(
        (uint32_t)(actor - _obj_array) / OBJ_STRIDE);
    int32_t bx = (int16_t)rd_le_u16(actor + 0u),
            cx = (int16_t)rd_le_u16(actor + 2u);

    _obj_units_stack_unmark(actor); // 1000:F3CA
    if (_obj_type_gate[(t >> 3) & 0x1Fu] & bit) return 1; // 1000:F450
    /* 1000:F417/F41F store the type bit; D-227 stores the actor's index bit. */
    if (_obj_units_ter_rect(_obj_ter_slot[0], bx, cx, rec_bit)) return 0;
    if (_obj_units_ter_rect(_obj_ter_slot[1], bx, cx, rec_bit)) return 0;
    return 1;
}


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t object_units_settle_gate_for_probe(uint8_t *actor)
{ return (uint8_t)(_obj_units_settle_gate(actor)); }
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 1000:EA33: the acting entity's bit in the two stack masks, 0x64AD and
 * 0x64C1, the same `1 << (7 - entity)` the stack verbs use.
 *
 * @return 1 when the acting entity is in a stack, 0 otherwise
 */
int32_t _obj_units_stack_masked(void)
{
    return ((_obj_ter_slot[0][0x13] | _obj_ter_slot[1][0x13]) &
            _obj_units_index_bit(_obj_entity)) != 0;
}


/* Step the frame down within [lo,hi] and count +0xA down. */
static void _obj_units_frame_step_down(uint8_t *actor, uint32_t lo, uint32_t hi)
{
    uint16_t f;

    wr_le_u16(actor + 2u,
              (uint16_t)(rd_le_u16(actor + 2u) + 1u)); // 1000:FDBF
    wr_le_u16(actor + 0xAu, (uint16_t)(rd_le_u16(actor + 0xAu) - 1u));
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 0xAu, 4u);
    f = rd_le_u16(actor + 4u);
    if (actor[0xCu] != 0u) {
        if (++f == hi) actor[0xCu] = 0u;
    } else {
        if (--f == lo) actor[0xCu] = 0xFFu;
    }
    wr_le_u16(actor + 4u, f);
}


/**
 * @brief Accessor: one word of the animation list.
 *
 * @param off the offset
 * @return the word, or 0 past the list
 */
static uint32_t _obj_units_anim_list_word(uint32_t off)
{ return off + 1u < sizeof _obj_anim_list ? _obj_anim_list[off / 2u] : 0u; }


/* --- 1000:F5BC: the level-end probe the movers' tails call --------------
 *
 * `BX` names the record (>= 0) or is -1 for the caller; `[0x6F1D]` is cleared
 * first.  The gate is the tribe counter: while `[0x5B25] | [0x5B26]` is zero
 * the record is **taken** - freed, its class slot cleared, and, when it was the
 * acting record, the first placed record whose type is neither 0x16, 0x1B..0x1E
 * nor 0x21 is resolved through `0x4147`; when none is, the level is lost
 * (`[0x138E] = [0x1809] = 0`, `[0x1979] = 2`, `[0x6916] = 2`).  With the
 * tribe counter showing a count it is the **goal form** instead: a human is
 * lost, so `[0x5B22] = 0xFF`, the position from `[0x4220]`/`[0x4222]` (the
 * settle cache `0x6847` when x is negative), frame 0x2D / type 0x21 /
 * `+0xA` = 5, the acting-record flags, and the 0xE389 probe on the landing.
 */
/**
 * @brief 1000:F682: the level-end probe's goal form.
 *
 * @param t the target record
 * @param idx the index
 */
static void _obj_units_level_end_goal(uint8_t *t, uint32_t idx)
{
    statusbar_arm_tribe_count(); // [0x5B22] = 0xFF
    wr_le_u16(t + 2u, _obj_sel_xoff); // [0x4222]
    wr_le_u16(t + 0u, _obj_side); // [0x4220]
    if (_obj_side < 0) { // 1000:F692
        wr_le_u16(t + 0u, (uint16_t)((_obj_settled[idx][0] + 8u) & 0xFFF0u));
        wr_le_u16(t + 2u, _obj_settled[idx][1]);
    }
    wr_le_u16(t + 0u, (uint16_t)(rd_le_u16(t + 0u) - 8u)); // 1000:F6B1
    wr_le_u16(t + 4u, 0x002Du);
    t[6] = 0u;
    wr_le_u16(t + 8u, 0x0021u);
    wr_le_u16(t + 0xAu, 5u);
    if (idx == _obj_entity && (int32_t)idx != _obj_player && !t[7]) {
        _obj_sequence_set_program_index(1u); // [0x5B56] = 1
        _obj_result_pending = 5; // [0x6916] = 0x105
        _obj_result_cursor = 1;
    }
    {
        uint32_t cell;
        int32_t land =
            _obj_core_land_y((int32_t)(int16_t)rd_le_u16(t + 0u) + 0x10,
                             (int32_t)(int16_t)rd_le_u16(t + 2u) + 0x18, &cell);

        if (land >= 0) wr_le_u16(t + 2u, (uint16_t)(land - 0x18)); // 1000:F6F2
    }
}


/**
 * @brief The level-end probe the movers' tails call.
 *
 * @param actor the object record
 * @param bx the record index, or -1 for the actor
 */
static void _obj_units_level_end_probe(uint8_t *actor, int32_t bx)
{
    uint8_t *t;
    uint16_t idx;

    _obj_sequence_active = 0; // 1000:F5BC
    if (bx >= 0) {
        t = object_core_slot((uint32_t)bx);
        idx = (uint16_t)((uint32_t)bx);
    } else {
        t = actor;
        idx = (uint16_t)((uint32_t)((actor - _obj_array) / OBJ_STRIDE));
    }
    if (!t) return;
    if (statusbar_tribe_count_nonzero()) { // 1000:F5E3
        _obj_units_level_end_goal(t, idx);
        return;
    }
    wr_le_u16(t + 0u, 0xFFFFu); // 1000:F5EF: taken
    wr_le_u16(t + 2u, 0xFFFFu);
    t[6] = 0xFFu;
    wr_le_u16(t + 8u, 0x001Bu);
    if (t[7]) { // 1000:F603
        uint8_t cls = t[7];

        _obj_core_class_byte_set(cls, 0u); // 0x3E5F + cls - 1
        if (8u + cls < OBJ_SLOTS)
            _obj_array[(8u + cls) * OBJ_STRIDE + 6] = 0xFFu;
        t[7] = 0u;
    }
    if (idx != _obj_entity) return; // 1000:F627
    for (uint16_t i = 0; i < 8u; i++) { // 1000:F630
        uint8_t *r = object_core_slot(i);
        uint16_t ty;

        if (!r || (r[6] & 0x80u)) continue;
        ty = rd_le_u16(r + 8u);
        if (ty == 0x16u) continue;
        if (ty >= 0x1Bu && ty <= 0x1Eu) continue;
        if (ty == 0x21u) continue;
        _obj_sequence_resolve_sprite(i); // 1000:F664 -> 0x4147
        return;
    }
    level_set_end(0); // 1000:F66D
}


/**
 * @brief Accessor: one byte of a length-checked table.
 *
 * @param t the table
 * @param n the table length
 * @param off the byte offset
 * @return the byte, or 0 past n
 */
static uint32_t _obj_units_tab_byte(const uint8_t *t, uint32_t n, uint32_t off)
{ return off < n ? t[off] : 0u; }


/** @brief 1000:DBCD (type 28): a 9-frame stepper over 0x6868 (the frame is a table
 * byte, +0xC the cursor); at frame 0xD3 the record climbs one pixel per tick
 * while its y's low nibble is non-zero, and at a row boundary the level-end
 * probe runs with the scratch pair at 0xFFFF.
 *
 * @param actor the object record
 */
void _obj_units_tail_type28(uint8_t *actor)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); // 1000:DBCD
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 9u);
    if (rd_le_u16(actor + 4u) != 0x00D3u) { // 1000:DBD7
        uint16_t off = rd_le_u16(actor + 0xCu);

        actor[4] = (uint8_t)_obj_units_tab_byte(
            _obj_type28_frame_tab, sizeof _obj_type28_frame_tab, off);
        wr_le_u16(actor + 0xCu, (uint16_t)(off + 1u));
        return;
    }
    if (rd_le_u16(actor + 2u) & 0xFu) { // 1000:DBEC
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 1u));
        return;
    }
    _obj_side = (int16_t)0xFFFF; // [0x4220]
    _obj_sel_xoff = 0xFFFFu; // [0x4222]
    _obj_units_level_end_probe(actor, -1); // 1000:DC11
}


/**
 * @brief 1000:DE7E (type 30): the 0x687C table walk.
 *
 * @param actor the object record
 */
void _obj_units_tail_type30(uint8_t *actor)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); // 1000:DE7E
    if ((int16_t)a >= 0) return;
    {
        uint16_t off = rd_le_u16(actor + 0xCu);
        int32_t dx = (int32_t)(int8_t)(uint8_t)_obj_units_tab_byte(
            _obj_type30_step_tab, sizeof _obj_type30_step_tab, off);
        int32_t dy = (int32_t)(int8_t)(uint8_t)_obj_units_tab_byte(
            _obj_type30_step_tab, sizeof _obj_type30_step_tab, off + 1u);
        uint16_t nxt = off + 2u;

        if (_obj_units_tab_byte(_obj_type30_step_tab,
                                sizeof _obj_type30_step_tab,
                                off + 2u) == 0u) { // 1000:DE9F
            uint16_t e;

            nxt = (uint16_t)(nxt - 4u); // 1000:DEA5
            e = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u);
            wr_le_u16(actor + 0xEu, e); // 1000:DEA8
            if ((int16_t)e < 0 &&
                statusbar_tribe_count_nonzero()) { // 1000:DEB4
                _obj_side = rd_le_u16(actor + 0u); // [0x4220]
                _obj_sel_xoff = rd_le_u16(actor + 2u); // [0x4222]
                _obj_units_level_end_probe(actor, -1); // 1000:DEC4
                return;
            }
        }
        wr_le_u16(actor + 0xCu, nxt); // 1000:DEC7
        wr_le_u16(actor + 0xAu, (uint16_t)(int16_t)dy); // [SI+0xA] = CX
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) + (uint16_t)(int16_t)dx));
    }
}


/** @brief 1000:DED1 (type 34): a two-state tail on the record `+0xA` points at.  While
 * the +6 bit 7 is set: a target `+4` of 2 or 3 holds, anything else runs the
 * level-end probe at ([SI+0x10], [target+8] + 0x28).  With the bit clear: a
 * target `+4` of 2 or 3 sets the bit when `[target+0xC] == +0xE`, and anything
 * else takes the home form (frame 0x10 / type 2, `+0xA`/`+0xC` cleared).
 *
 * @param actor the object record
 */
void _obj_units_tail_type34(uint8_t *actor)
{
    /* 1000:DED1: +0xA is the ENTITY record pointer, not an object link.  The
     * entity interaction tags the index (0x8000|n); an untagged link is not
     * a carry and leaves the record alone. */
    uint16_t link = rd_le_u16(actor + 0xAu);
    const Entity *e = (link & 0x8000u) ? entity_at(link & 0x7FFFu) : NULL;
    uint16_t v;

    if (!e) return;
    v = e->handler;
    if (v == 2u || v == 3u) { // 1000:DEDA / 0xDF0A
        if (!(actor[6] & 0x80u)) {
            if (e->waypoint == rd_le_u16(actor + 0xEu)) actor[6] |= 0x80u;
        }
        return;
    }
    if (actor[6] & 0x80u) { // 1000:DEE8
        _obj_side = rd_le_u16(actor + 0x10u); // [0x4220]
        _obj_sel_xoff = (uint16_t)(e->y + 0x28u); // [0x4222]
        _obj_units_level_end_probe(actor, -1);
        return;
    }
    wr_le_u16(actor + 4u, 0x0010u); // 1000:DF17
    wr_le_u16(actor + 8u, 0x0002u);
    wr_le_u16(actor + 0xAu, 0u);
    wr_le_u16(actor + 0xCu, 0u);
}


/** @brief 1000:EF66 (type 22): a 5-frame stepper to frame 0x2E, then the level-end
 * probe with the scratch pair at 0xFFFF.
 *
 * @param actor the object record
 */
void _obj_units_tail_type22(uint8_t *actor)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); // 1000:EF66
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 5u);
    wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
    if (rd_le_u16(actor + 4u) != 0x2Eu) return; // 1000:EF73
    _obj_side = (int16_t)0xFFFF; // [0x4220]
    _obj_sel_xoff = 0xFFFFu; // [0x4222]
    _obj_units_level_end_probe(actor, -1); // 1000:EF93
}


/* --- the rows' tails (the +0xC word of each DS:0x698A row) --------------
 *
 * `1000:E4D1` reads the tail word from the row and `1000:E4E0` calls it after
 * the handler and the `+0xA` hook.  These are the rows' own tails, each cited
 * to its entry.
 */

/** @brief 1000:D9D0 (shared body of 0xD9C8 / 0xD9CD, types 31 and 32): a 7-frame hold
 * that steps the frame up and wraps four back at the bound (0x112 / 0x116).
 *
 * @param actor the object record
 * @param bound the bound value
 */
void _obj_units_tail_frame_bound(uint8_t *actor, uint32_t bound)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); // 1000:D9D0
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 7u); // 1000:D9D6
    wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
    if (rd_le_u16(actor + 4u) == bound)
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) - 4u)); // 1000:D9E3
}


/**
 * @brief 1000:DB30: the frame -> type map the landing tails share.
 *
 * @param actor the object record
 */
static void _obj_units_tail_frame_to_type(uint8_t *actor)
{
    uint8_t t = 2;

    if (rd_le_u16(actor + 4u) < 0x10u) { // 1000:DB33
        t = 1;
        if (rd_le_u16(actor + 4u) < 8u) t = 0;
    }
    wr_le_u16(actor + 8u, t); // 1000:DB44
}


/**
 * @brief 1000:DB15: the hard landing.
 *
 * @param actor the object record
 */
static void _obj_units_tail_crash(uint8_t *actor)
{
    wr_le_u16(actor + 0u,
              (uint16_t)(rd_le_u16(actor + 0u) - 7u)); // 1000:DB15
    wr_le_u16(actor + 4u, 0x0028u);
    wr_le_u16(actor + 8u, 0x0016u);
    wr_le_u16(actor + 0xAu, 5u);
    _obj_core_driver_call(4); // 1000:DB28
}


/**
 * @brief 1000:DB05: the type-3 tail's coast (no landing).
 *
 * @param actor the object record
 * @param dx the x delta
 */
static void _obj_units_tail_type3_coast(uint8_t *actor, uint32_t dx)
{
    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + dx));
    wr_le_u16(actor + 0xEu, (uint16_t)(rd_le_u16(actor + 0xEu) + dx));
    wr_le_u16(actor + 0xAu,
              (uint16_t)(dx == rd_le_u16(actor + 0xCu) ? dx : dx + 1u));
}


/**
 * @brief 1000:DAB2: the type-3 tail's landing.
 *
 * @param actor the object record
 * @param bx the surface row
 */
static void _obj_units_tail_type3_land(uint8_t *actor, uint32_t bx)
{
    uint16_t sp;

    wr_le_u16(actor + 2u, (uint16_t)(bx - 0x18u));
    sp = rd_le_u16(actor + 0xEu);
    if (sp >= 0x30u) {
        _obj_units_tail_crash(actor);
        return;
    } // 1000:DABF
    level_set_offset_word((uint16_t)(sp >> 3)); // 1000:DAC7
    _obj_units_tail_frame_to_type(actor); // 1000:DACA
    _obj_core_driver_call(5);
}


/** @brief 1000:D9E8 (type 3): the gravity tail.  The gate is the same 0x6EBE/row one
 * the settle uses; then a probe at (x + 8, y + 0x18) with the row above it, the
 * 0xF0..0xF2 platform snap (frame 0xCD / type 0x1C / effect 6), and otherwise
 * the page byte of the feet / one row above / one row below picking a landing:
 * a descent of 0x30 or more crashes (frame 0x28 / type 0x16 / effect 4), a
 * slower one arms [0x2D8E] = speed >> 3, takes the frame->type map and plays
 * effect 5.  `[0x4258]` is the map buffer's end (map + 0x280 = 640 here).
 *
 * @param actor the object record
 */
void _obj_units_tail_type3(uint8_t *actor)
{
    const uint8_t *map = level_map();
    uint32_t ax, bx, cx, dx, di, b;
    int32_t sc;

    if (!_obj_units_settle_gate(actor)) { // 1000:D9EB
        _obj_units_tail_frame_to_type(actor);
        return;
    }
    ax = (rd_le_u16(actor + 0u) + 8u);
    bx = (rd_le_u16(actor + 2u) + 0x18u);
    dx = rd_le_u16(actor + 0xAu);
    if (bx >= 0x158u) {
        _obj_units_tail_crash(actor);
        return;
    } // 1000:DA02
    cx = bx;
    bx &= 0xFFF0u;
    di = 2u * bx + (ax >> 4); // 1000:DA0C..DA23
    ax &= 0xFu; // 1000:DA1C: AX = the FINE x
    if (map) {
        b = _obj_core_map_buffer_byte(di - 0x20u); // 1000:DA30

        if (b >= 0xF0u && b - 0xF0u < 3u) { // 1000:DA37/DA39
            /* 1000:DA3E..0xDA61 writes only y/frame/type/+C/+A - no x store
             * (the x snap belongs to 0xF6F6's branch, not this one).  Writing
             * it teleported a drowning actor to x -4 (D-78). */
            wr_le_u16(actor + 2u, (uint16_t)(bx - 0x1Cu));
            wr_le_u16(actor + 4u, 0x00CDu);
            wr_le_u16(actor + 8u, 0x001Cu);
            wr_le_u16(actor + 0xCu, 0u);
            wr_le_u16(actor + 0xAu, 9u);
            _obj_core_driver_call(6);
            return;
        }
    }
    if (di >= 0x280u || !map) { // 1000:DA63
        _obj_units_tail_type3_coast(actor, dx);
        return;
    }
    b = level_page_byte(_obj_core_map_buffer_byte(di), ax); // 1000:DA71
    if (b & 0x80u) { // 1000:DA85
        di += 0x20u;
        if (di >= 0x280u) { // 1000:DA8C
            _obj_units_tail_type3_coast(actor, dx);
            return;
        }
        b = level_page_byte(_obj_core_map_buffer_byte(di), ax);
        if (b & 0x80u) { // 1000:DAA6
            _obj_units_tail_type3_coast(actor, dx);
            return;
        }
        bx += 0x10u; // 1000:DAA8
        sc = (int32_t)(int8_t)(uint8_t)b;
        bx = (uint32_t)((int32_t)bx + sc);
        cx = (uint32_t)((int32_t)cx - (int32_t)bx);
        if ((int32_t)cx < 0) {
            cx = (uint32_t)-(int32_t)cx;
            if (cx > dx) { // 1000:DB01
                _obj_units_tail_type3_coast(actor, dx);
                return;
            }
        }
        _obj_units_tail_type3_land(actor, bx); // 1000:DAB2
        return;
    }
    if (b != 0u) { // 1000:DAD6
        sc = (int32_t)(int8_t)(uint8_t)b;
        bx = (uint32_t)((int32_t)bx + sc);
        cx = (uint32_t)((int32_t)cx - (int32_t)bx);
        if ((int32_t)cx < 0) {
            cx = (uint32_t)-(int32_t)cx;
            if (cx > dx) {
                _obj_units_tail_type3_coast(actor, dx);
                return;
            }
        }
        _obj_units_tail_type3_land(actor, bx);
        return;
    }
    /* b == 0: look two rows above (1000:DAD8) */
    if (di >= 0x40u) {
        b = level_page_byte(_obj_core_map_buffer_byte(di - 0x40u), ax);
        _obj_side = (uint16_t)(int16_t)(int8_t)(uint8_t)b; // [0x4230]
        if (!(b & 0x80u)) {
            bx -= 0x10u;
            sc = (int32_t)_obj_side;
            bx = (uint32_t)((int32_t)bx + sc);
            cx = (uint32_t)((int32_t)cx - (int32_t)bx);
            if ((int32_t)cx < 0) {
                cx = (uint32_t)-(int32_t)cx;
                if (cx > dx) {
                    _obj_units_tail_type3_coast(actor, dx);
                    return;
                }
            }
            _obj_units_tail_type3_land(actor, bx);
            return;
        }
    }
    sc = 0; // 1000:DA87: AX = 0
    bx = (uint32_t)((int32_t)bx + sc);
    cx = (uint32_t)((int32_t)cx - (int32_t)bx);
    if ((int32_t)cx < 0) {
        cx = (uint32_t)-(int32_t)cx;
        if (cx > dx) {
            _obj_units_tail_type3_coast(actor, dx);
            return;
        }
    }
    _obj_units_tail_type3_land(actor, bx);
}


/** @brief 1000:DB7A: find the actor's span entry.  Returns the matched entry (even
 * when a gate fails, because the original leaves it in DI for the walk) and
 * sets `yes` when the gates at DB9D/DBA4 also pass.  No match returns NULL
 * (the original's DI is then past the two entries, i.e. the zero word).
 *
 * @param actor the object record
 * @param yes the gate result (out)
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_obj_units_span_owner(uint8_t *actor, int32_t *yes)
{
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);
    uint8_t *found = NULL;

    *yes = 0;
    for (uint16_t i = 0; i < 2u; i++) { // AX = 1: two entries
        uint8_t *e = (uint8_t *)_obj_span[i];

        if (rd_le_u16(e + 0u) != (uint16_t)(idx & 0x7FFFu))
            continue; // 1000:DB8D
        found = e;
        if (!(rd_le_u16(e + 0x12u) & 0x8000u)) return found; // 1000:DB9D
        if (rd_le_u16(e + 8u) != 0u) return found; // 1000:DBA4
        *yes = 1;
        return found;
    }
    return found;
}


/** @brief 1000:DBAE: walk the actor's frame toward AX on a 3-frame hold.  The span's
 * height (+6) equal to its fall distance (+8) holds the frame; a NULL span is
 * the original's past-the-table DI, which reads as two zero words.
 *
 * @param actor the object record
 * @param frame the frame index
 * @param span the span
 */
static void _obj_units_tail_walk_to_frame(uint8_t *actor, uint32_t frame,
                                          const uint8_t *span)
{
    uint16_t a;
    uint16_t height = span ? rd_le_u16(span + 6u) : 0u;
    uint16_t dist = span ? rd_le_u16(span + 8u) : 0u;

    if (height == dist) return; // 1000:DBAE..0xDBCC
    a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);
    wr_le_u16(actor + 0xAu, a); // 1000:DBB6
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 3u); // 1000:DBBB
    if (rd_le_u16(actor + 4u) == frame)
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) + 1u)); // 1000:DBC9
    else
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) - 1u)); // 1000:DBC5
}


/** @brief 1000:DB48 / 1000:DB61 (types 9 and 10): the span's owner test; on "yes" the
 * matched entry is freed and the acting record is reset (frame/type 0/0 or
 * 8/1), otherwise it walks toward 0xAB / 0xAD.
 *
 * @param actor the object record
 * @param frame the frame index
 * @param f8 the field-8 value
 * @param bad the frame to walk toward
 */
void _obj_units_tail_span_owner(uint8_t *actor, uint32_t frame, uint32_t f8,
                                uint32_t bad)
{
    int32_t yes;
    uint8_t *span = _obj_units_span_owner(actor, &yes);

    if (yes) { // 1000:DB4B
        wr_le_u16(span + 0u, 0xFFFFu); // 1000:DB4D: free the matched entry
        wr_le_u16(actor + 4u, (uint16_t)frame);
        wr_le_u16(actor + 8u, (uint16_t)f8);
        return;
    }
    _obj_units_tail_walk_to_frame(actor, bad, span); // 1000:DB5C -> 0xDBAE
}


/** @brief 1000:DD93 (type 16): a 10-frame stepper that walks the frame through
 * 0x55..0x57, flipping the direction byte at +0xC and playing effect 8 at 0x55;
 * a `+0xE` countdown then drops it back to frame 0x10 / type 2.
 *
 * @param actor the object record
 */
void _obj_units_tail_type16(uint8_t *actor)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a);
    if ((int16_t)a < 0) { // 1000:DD96
        uint16_t f = rd_le_u16(actor + 4u);

        wr_le_u16(actor + 0xAu, 0x0Au); // 1000:DD98
        if (actor[0xCu] != 0u) { // 1000:DDA0
            f = (uint16_t)(f - 1u);
            if (f == 0x55u) { // 1000:DDA7
                actor[0xCu] = 0u;
                _obj_core_driver_call(8);
                f = 0x55u;
            }
        } else {
            f++;
            if (f == 0x57u) actor[0xCu] = 0xFFu; // 1000:DDC3
        }
        wr_le_u16(actor + 4u, f); // 1000:DDC7
    }
    a = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u); // 1000:DDCA
    wr_le_u16(actor + 0xEu, a);
    if ((int16_t)a < 0) {
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
    }
}


/** @brief 1000:EB5C / 1000:EB6A (types 13 and 14): one walk step, then a `+0xE`
 * countdown that sets the type to 1 (left) or 0 (right).
 *
 * @param actor the object record
 * @param left the left value
 */
void _obj_units_tail_walk_countdown(uint8_t *actor, int32_t left)
{
    if (left) _obj_command_walk(actor, 1);
    else _obj_command_walk(actor, 0); // 1000:EB5C

    {
        uint16_t a = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u);

        wr_le_u16(actor + 0xEu, a); // 1000:EB5F
        if ((int16_t)a < 0)
            wr_le_u16(actor + 8u, left ? 1u : 0u); // 1000:EB64 / 0xEB72
    }
}


/** @brief 1000:E802 / 1000:E831 (types 4 and 5): on a type match, probe the cell under
 * the feet; a field-K special tile is the climb stepper (`1000:E828`/`E857`:
 * lo 0x23, hi 0x25 into 0xFD91/0xFDBF), anything else drops the record to
 * frame 0x10 / type 2.  (The 0x52/0x54 pair belongs to 1000:EAAC's type-11
 * carry stepper, not here.)
 *
 * @param actor the object record
 * @param type the type
 * @param yoff the y offset
 * @param down non-zero for the down stepper
 */
void _obj_units_tail_special_tile(uint8_t *actor, uint32_t type, int32_t yoff,
                                  int32_t down)
{
    uint32_t cell, sub;

    if (rd_le_u16(actor + 8u) != type) return; // 1000:E802
    cell = _obj_core_map_at_abs((int32_t)(int16_t)rd_le_u16(actor + 0u) + 8,
                                (int32_t)(int16_t)rd_le_u16(actor + 2u) + yoff,
                                &sub);
    if (cell == level_special_tile(0u) || cell == level_special_tile(1u)) {
        if (down) _obj_units_frame_step_down(actor, 0x23u, 0x25u); // 1000:E857
        else _obj_units_frame_step(actor, 0x23u, 0x25u); // 1000:E828
        return;
    }
    wr_le_u16(actor + 4u, 0x0010u); // 1000:E81D
    wr_le_u16(actor + 8u, 0x0002u);
}


/** @brief 1000:ECB7 (type 17): step the 0x6E5C frame list by one.  While the 9-beat
 * has not expired (`1000:ECBB JNS 0xECDC`) the routine returns at the bare
 * `RET`; when it has, the frame list advances and the shared "restore home"
 * block runs - the `ADD +0xC` at 0xECD8 falls straight into 0xECDD, so both
 * branches of the expiry reach it: the frame/type come from `+0xE`/`+0x10`,
 * `+0xA`/`+0xC` are cleared and `[0x1F1C]` gets 0x82 to its `+4` and 0 to its
 * `+8`.
 *
 * @param actor the object record
 */
void _obj_units_tail_type17(uint8_t *actor)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); // 1000:ECB7
    if ((int16_t)a >= 0) return; // 1000:ECBB JNS 0xECDC
    {
        uint16_t off = rd_le_u16(actor + 0xCu);

        wr_le_u16(actor + 0xAu, 9u);
        if (off < 0x1Au)
            wr_le_u16(actor + 4u, (uint16_t)_obj_units_anim_list_word(off));
        if (!(_obj_units_anim_list_word(off + 2u) & 0x8000u))
            wr_le_u16(actor + 0xCu, (uint16_t)(off + 2u));
    }
    wr_le_u16(actor + 4u, rd_le_u16(actor + 0xEu)); // 1000:ECDD
    wr_le_u16(actor + 8u, rd_le_u16(actor + 0x10u));
    wr_le_u16(actor + 0xAu, 0u);
    wr_le_u16(actor + 0xCu, 0u);
    /* 1000:ECF1 writes [0x1F1C]'s +4 = 0x82 and +8 = 0; that pointer has no
     * ported writer, so it is not stored. */
}


/** @brief 1000:DDDA (type 29): the "join the tribe" tail.  It compares the *acting*
 * record with this one (0x79F4 returns it in SI): when the actor is within the
 * 0x20 window, the actor is collected - type 2, its class record marked, the
 * two counters bumped ([0x3E20], [0x11FC]) - with effect 0xC; otherwise a slow
 * two-frame animation toward frame 0xFE and then 0x102 on its class record.
 *
 * @param actor the object record
 */
void _obj_units_tail_type29(uint8_t *actor)
{
    uint8_t *e = object_core_slot(object_sequence_action_entity());
    int32_t dx, dy;
    uint16_t a;

    if (e && rd_le_u16(e + 8u) <= 2u) { // 1000:DDE4
        dx = (int32_t)(int16_t)rd_le_u16(actor + 0u) -
             (int32_t)(int16_t)rd_le_u16(e + 0u);
        dy = (int32_t)(int16_t)rd_le_u16(actor + 2u) -
             (int32_t)(int16_t)rd_le_u16(e + 2u);
        if (dx < 0 && dx + 0x20 >= 0 && dy < 0 && dy + 0x20 >= 0) {
            uint8_t *cls = _obj_core_link_record(rd_le_u16(actor + 0xCu));

            wr_le_u16(actor + 8u, 2u); // 1000:DDFD
            wr_le_u16(actor + 0xAu, 0u);
            if (cls) cls[6] = 0x80u; // 1000:DE0A
            wr_le_u16(actor + 4u, 0x0010u);
            wr_le_u16(actor + 0u, (uint16_t)(rd_le_u16(actor + 0u) + 8u));
            wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 8u));
            _obj_placed_humans++; // 1000:DE1B: [0x3E20]
            level_add_member(); // 1000:DE1F: [0x11FC]
            _obj_core_driver_call(0xC);
            return;
        }
    }
    a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u); // 1000:DE2C
    wr_le_u16(actor + 0xAu, a);
    if ((int16_t)a >= 0) return;
    if (rd_le_u16(actor + 4u) == 0x00FEu) { // 1000:DE31
        uint8_t *cls;

        wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
        wr_le_u16(actor + 0xAu, 0x32u);
        cls = _obj_core_link_record(rd_le_u16(actor + 0xCu));
        if (cls) {
            wr_le_u16(cls + 0u, rd_le_u16(actor + 0u));
            wr_le_u16(cls + 2u, rd_le_u16(actor + 2u));
            wr_le_u16(cls + 0u,
                      (uint16_t)(rd_le_u16(cls + 0u) + 0x0Fu)); // 1000:DE60
            wr_le_u16(cls + 2u, (uint16_t)(rd_le_u16(cls + 2u) - 0x0Cu));
            wr_le_u16(cls + 4u, 0x0102u);
            cls[6] = 0u;
        }
        _obj_core_driver_call(8);
        return;
    }
    wr_le_u16(actor + 4u,
              (uint16_t)(rd_le_u16(actor + 4u) - 1u)); // 1000:DE38
    wr_le_u16(actor + 0xAu, 0xFAu);
    {
        uint8_t *cls = _obj_core_link_record(rd_le_u16(actor + 0xCu));

        if (cls) cls[6] = 0x80u;
    }
}


/**
 * @brief The 0x6F92 span entry at a raw offset, or NULL.
 *
 * @param off the byte offset into the two spans
 * @return the entry, or NULL past the spans
 */
static uint8_t *_obj_units_span_entry(uint32_t off)
{
    if (off >= sizeof _obj_span) return NULL;
    return (uint8_t *)_obj_span + off;
}


/**
 * @brief 1000:EAAC (type 11): the record's +0xE is the 0x6F92 span entry.
 *
 * Its word 0 is the owner record, resolved through the 0x3E24 pointer table.
 * Compare the owner's y with ours, then either the 0x52/0x54 stepper or the
 * "home" form (type = the owner's +8 + 4, frame 8 only for type 9, +0xE = 0xA).
 *
 * @param actor the object record
 */
void _obj_units_tail_type11(uint8_t *actor)
{
    uint8_t *span = _obj_units_span_entry(rd_le_u16(actor + 0xEu));
    uint8_t *t;

    if (!span) return;
    t = object_core_slot(rd_le_u16(span + 0u)); // the span's owner
    if (!t) return;
    if (rd_le_u16(actor + 2u) > rd_le_u16(t + 2u)) { // 1000:EAB8
        _obj_units_frame_step(actor, 0x52u, 0x54u);
        return;
    }
    {
        uint16_t f = rd_le_u16(t + 8u);

        wr_le_u16(actor + 8u, (uint16_t)(f + 4u)); // 1000:EAC9
        wr_le_u16(actor + 4u, f == 9u ? 8u : 0u);
        wr_le_u16(actor + 0xEu, 0x000Au); // 1000:EAE0
    }
}


/** @brief 1000:EAE6 (type 12): the same span entry; when the span's end
 * (`span+4` + `span+6`) is at or above the record's y it is home (frame 0x10,
 * type 2, [0x6FE6] = 0xFF), otherwise the map probe under its feet decides
 * between the down-stepper and home.
 *
 * @param actor the object record
 */
void _obj_units_tail_type12(uint8_t *actor)
{
    uint8_t *span = _obj_units_span_entry(rd_le_u16(actor + 0xEu));
    uint32_t sum, ry = rd_le_u16(actor + 2u);

    if (!span) return;
    sum = (uint32_t)(rd_le_u16(span + 4u) + rd_le_u16(span + 6u));
    if (sum > ry) { // 1000:EAF6: span end below the record
        uint32_t cell, sub;
        int32_t y = (int32_t)(int16_t)ry + 0x18;
        uint8_t b;

        cell = _obj_core_map_at_abs((int32_t)(int16_t)rd_le_u16(actor + 0u) + 8,
                                    y, &sub);
        b = (uint8_t)(level_page_byte(cell, sub));
        /* 1000:EB4B: a solid page byte takes the stepper; otherwise the
         * surface line 1000:EB4D is tested against the probe line `y`. */
        if ((b & 0x80u) ||
            (uint16_t)(((uint16_t)y & 0xFFF0u) + b) > (uint16_t)y) {
            _obj_units_frame_step_down(actor, 0x52u, 0x54u);
            return;
        }
    }
    wr_le_u16(actor + 4u, 0x0010u); // 1000:EAFB
    wr_le_u16(actor + 8u, 0x0002u);
    _obj_core_home_stance_set(); // 1000:EB05
}


/** @brief 1000:F0C9 (type 33): a 5-frame stepper down to frame 0x27, where the record
 * jumps 8 px right and returns to frame 0x10 / type 2; if it is the acting
 * record it also seeds slot 8 (0x3F05) with type 0x1A and [0x6DEA] = 0x3E8.
 *
 * @param actor the object record
 */
void _obj_units_tail_type33(uint8_t *actor)
{
    uint16_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); // 1000:F0C9
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 5u);
    wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) - 1u));
    if (rd_le_u16(actor + 4u) != 0x27u) return; // 1000:F0D6
    wr_le_u16(actor + 0u, (uint16_t)(rd_le_u16(actor + 0u) + 8u));
    wr_le_u16(actor + 4u, 0x0010u);
    wr_le_u16(actor + 8u, 0x0002u);
    wr_le_u16(actor + 0xAu, 0u);
    if ((uint32_t)((actor - _obj_array) / OBJ_STRIDE) != _obj_entity)
        return; // 1000:F0F7
    {
        uint8_t *slot8 = object_core_slot(8u); // 0x3F05

        if (!slot8) return;
        wr_le_u16(slot8 + 0u, rd_le_u16(actor + 0u));
        wr_le_u16(slot8 + 2u, (uint16_t)(rd_le_u16(actor + 2u) - 0x1Au));
        wr_le_u16(slot8 + 6u, 0u);
        wr_le_u16(slot8 + 8u, 0x001Au);
        wr_le_u16(slot8 + 0xAu, 0x0605u);
        _obj_cmd_timer = 0x03E8u; // 1000:F11C
    }
}


/** @brief 1000:F7C3/F83E: `byte[0x6F0B + (type - 0x19)]` = `byte[0x6EF2 + type]`.
 * Types 0x19/0x1A select bytes 0x6F0B/0x6F0C, which `level_setup_screen`
 * (1000:4E99) overwrites with the level's field-K second word; the rest of the
 * window is the image's static bytes.
 *
 * @param type the type
 * @return the tile byte for the type
 */
static uint32_t _obj_units_type_tile_byte(uint32_t type)
{
    if (type == 0x19u) return level_special_tile(2u); // [0x6F0B]
    if (type == 0x1Au) return level_special_tile(3u); // [0x6F0C]
    return _obj_type_tile[type < 35u ? type : 0u];
}


/**
 * @brief 1000:F8AD: the fall machine's hard landing.
 *
 * @param actor the object record
 */
static void _obj_units_fall_hard_landing(uint8_t *actor)
{
    wr_le_u16(actor + 0u, (uint16_t)(rd_le_u16(actor + 0u) - 8u));
    wr_le_u16(actor + 4u, 0x0028u);
    wr_le_u16(actor + 8u, 0x0016u);
    wr_le_u16(actor + 0xAu, 5u);
    _obj_core_driver_call(4);
}


/**
 * @brief 1000:F975: the fall machine's landing at y.
 *
 * @param actor the object record
 * @param bx the landing y
 */
static void _obj_units_fall_land_at_y(uint8_t *actor, int32_t bx)
{
    wr_le_u16(actor + 2u, (uint16_t)bx);
    wr_le_u16(actor + 0x10u, 0u);
    if (rd_le_u16(actor + 0xCu) < 0x10u) wr_le_u16(actor + 0xCu, 0x0010u);
}


/**
 * @brief 1000:F8D2: the fall machine's land pair.
 *
 * @param actor the object record
 */
static void _obj_units_fall_land_pair(uint8_t *actor)
{
    wr_le_u16(actor + 4u, (uint16_t)_obj_side); // frame <- [0x4230]
    wr_le_u16(actor + 8u, _obj_sel_xoff); // type  <- [0x4234]
    wr_le_u16(actor + 0xAu, 0u);
    _obj_core_driver_call(5);
}


/**
 * @brief 1000:F966: the fall machine's placement.
 *
 * @param actor the object record
 * @param ax the x
 * @param bx the y
 */
static void _obj_units_fall_place(uint8_t *actor, int32_t ax, int32_t bx)
{
    wr_le_u16(actor + 0u, (uint16_t)ax);
    wr_le_u16(actor + 2u, (uint16_t)bx);
    if (bx >= 0x140) _obj_units_fall_hard_landing(actor);
}


/**
 * @brief 1000:F8EC: the fall machine's item-word probe.
 *
 * @param actor the object record
 * @param ax the x
 * @param bx the y
 */
static void _obj_units_fall_item_word(uint8_t *actor, int32_t ax, int32_t bx)
{
    int32_t px = ax + (int32_t)(int16_t)_obj_sel_frame;
    int32_t py = bx + 0x18;
    uint16_t tile = (uint16_t)(_obj_core_map_at_abs(px, py, NULL));
    uint16_t item = (uint16_t)(level_item_word(tile, ((uint32_t)py & 0xFu)));
    uint32_t lo, hi, mask;

    _obj_sel_type = item;
    lo = 0xFFFFu;
    hi = 0u;
    if (!(rd_le_u16(actor + 0x10u) & 0x8000u)) {
        lo = 0u;
        hi = 0xFFFFu;
    }
    /* 1000:F90B/F950: the shift count is `(x + +0x10 + [0x4238]) & 0xF` -
     * the *item* probe's sub-column, not the actor probe's `fine`. */
    for (uint16_t i = 0; i < ((uint32_t)px & 0xFu); i++) { // 1000:F952
        uint8_t carry = lo & 1u;

        lo >>= 1;
        hi = (hi >> 1) | (carry << 15);
    }
    mask = hi;
    if ((_obj_sel_type & mask) != 0u) { // 1000:F964 -> F975
        _obj_units_fall_land_at_y(actor, bx);
        return;
    }
    _obj_units_fall_place(actor, ax, bx);
}


/**
 * @brief 1000:F6F6: the fall machine.
 *
 * @param actor the object record
 */
void _obj_units_fall_machine(uint8_t *actor)
{
    int32_t ax, bx, col, d;
    uint32_t row, fine, page;
    uint32_t probeY, above, below;
    uint16_t anim;
    int32_t vy;

    if (!_obj_units_settle_gate(actor)) { // 1000:F6F6
        /* 1000:F6FB/F701: frame <- [0x4230] (_obj_side), type <- [0x4234]
         * (_obj_sel_xoff).  The third word [0x4238] is the item probe's x
         * offset, not the type. */
        wr_le_u16(actor + 4u, (uint16_t)_obj_side);
        wr_le_u16(actor + 8u, _obj_sel_xoff);
        wr_le_u16(actor + 0xAu, 0u);
        return;
    }
    if (rd_le_u16(actor + 0xAu) == 1u) // 1000:F70D DEC / JNZ
        wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
    wr_le_u16(actor + 0xAu, (uint16_t)(rd_le_u16(actor + 0xAu) - 1u));

    anim = rd_le_u16(actor + 0xCu); // 1000:F715
    /* 1000:F71B is `CMP AX,0x18 / JZ`: only the exact 0x18 takes the flat 3.
     * The counter stops there (1000:F727 increments to 0x18 at most), so `>=`
     * is equivalent for every reachable value; a crafted save can store more,
     * where the original reads past the 0x6EF3 table (D-101). */
    if (anim >= 0x18u) {
        vy = 3; // 1000:F71B
    } else {
        /* 1000:F720-0xF727: `CBW` sign-extends the table byte, so the
         * first steps of a full charge are NEGATIVE (the leap rises). */
        vy = _obj_fall_step[anim];
        anim++;
    }
    wr_le_u16(actor + 0xCu, anim); // 1000:F728
    wr_le_u16(actor + 0xEu,
              (uint16_t)(rd_le_u16(actor + 0xEu) + (uint16_t)(int16_t)vy));

    ax = (int32_t)(int16_t)rd_le_u16(actor + 0u) +
         (int32_t)(int16_t)rd_le_u16(actor + 0x10u);
    bx = (int32_t)(int16_t)rd_le_u16(actor + 2u) + vy;
    col = ax + 8; // 1000:F736/F73C: the probe x (DX = y, CX keeps the row)
    probeY = (uint32_t)(bx + 0x18); // 1000:F739
    fine = (uint32_t)col & 0xFu; // 1000:F753
    row = probeY & ~0xFu; // 1000:F758: CX is the row word

    above = _obj_core_map_at_abs(col, (int32_t)row - 0x10,
                                 NULL); // 1000:F761: [DI-0x20]
    if (above >= 0xF0u) { // 1000:F766
        /* 1000:F772: AX is the record.x + [+0x10] captured at 1000:F736, not
         * the probe x in CX (`ADD AX,8` then `AND AX,0xFFF0`). */
        uint16_t gx = (uint16_t)(ax + 8) & ~0xFu;

        if (above != 0xF1u) {
            gx -= 4u; // 1000:F778
            if (above != 0xF2u) gx += 8u;
            wr_le_u16(actor + 0u, gx); // 1000:F784
        }
        wr_le_u16(actor + 2u, (uint16_t)(row - 0x1Cu)); // 1000:F786
        wr_le_u16(actor + 4u, 0x00CDu);
        wr_le_u16(actor + 8u, 0x001Cu);
        wr_le_u16(actor + 0xCu, 0u);
        wr_le_u16(actor + 0xEu, 0u);
        wr_le_u16(actor + 0xAu, 9u);
        _obj_core_driver_call(6); // 1000:F7A6
        return;
    }
    if (bx >= 0x128) { // 1000:F7AE
        _obj_units_fall_place(actor, ax, bx);
        return;
    }
    below =
        _obj_core_map_at_abs(col, (int32_t)probeY, NULL); // 1000:F7B9: the feet
    page = level_page_byte(below, fine); // 1000:F7F3
    if (rd_le_u16(actor + 0x10u) != 0u) { // 1000:F7BB
        uint8_t want =
            (uint8_t)(_obj_units_type_tile_byte(rd_le_u16(actor + 8u)));

        if (below == want) { // 1000:F7CD
            /* 1000:F7D7: `[DI+0x20]` is the cell one row below the feet, in
             * the same column, through the same `BP` (0x92A0 + fine). */
            uint8_t b2 =
                (uint8_t)(_obj_core_map_at_abs(col, (int32_t)row + 0x10, NULL));

            if (level_page_byte(b2, fine) == 0u) { // 1000:F7EE
                _obj_units_fall_land_at_y(actor, bx);
                return;
            }
        }
    }
    if (page == 0u) { // 1000:F80A: the row above decides
        uint8_t a2 =
            (uint8_t)(_obj_core_map_at_abs(col, (int32_t)row - 0x10, NULL));
        uint8_t want =
            (uint8_t)(_obj_units_type_tile_byte(rd_le_u16(actor + 8u)));
        /* 1000:F83A/F84C park these two in [0x422C]/[0x422E] only to compare
         * them; the port compares the cells directly. */
        if (want == a2) { // 1000:F856
            _obj_units_fall_land_at_y(actor, bx);
            return;
        }
        if (!(level_page_byte(a2, fine) & 0x80u)) {
            row -= 0x10; // 1000:F86F: the row word, for the snap's `d`
            page = level_page_byte(a2, fine);
        }
    } else if (page & 0x80u) { // 1000:F80C JNS
        /* 1000:F80E: `ADD CX,0x10` moves the ROW, not the probe x; the cell
         * read at 1000:F813 is `[DI+0x20]` - the same column, one row down. */
        row += 0x10;
        page = level_page_byte(_obj_core_map_at_abs(col, (int32_t)row, NULL),
                               fine);
        if (page & 0x80u) { // 1000:F82F -> F8EC
            _obj_units_fall_item_word(actor, ax, bx);
            return;
        }
    }
    /* 1000:F87F: the snap */
    _obj_sel_type = (uint16_t)(int16_t)(int8_t)(uint8_t)page;
    d = (int32_t)(row + (uint32_t)(int16_t)(int8_t)(uint8_t)page) - 0x18 - bx;
    if (d >= 0) { // 1000:F891
        _obj_units_fall_item_word(actor, ax, bx);
        return;
    }
    d = -d;
    if (d > 3) {
        _obj_units_fall_item_word(actor, ax, bx);
        return;
    }
    bx -= d; // 1000:F89A
    wr_le_u16(actor + 0u, (uint16_t)ax);
    wr_le_u16(actor + 2u, (uint16_t)bx);
    {
        int32_t speed = (int16_t)rd_le_u16(actor + 0xEu);

        if (speed < 0) { // 1000:F8A6
            _obj_units_fall_land_pair(actor);
            return;
        }
        if (speed >= 0x30) { // 1000:F8AB
            _obj_units_fall_hard_landing(actor);
            return;
        }
        level_set_offset_word((uint16_t)((uint32_t)speed >> 3)); // 1000:F8CE
    }
    _obj_units_fall_land_pair(actor);
}


/**
 * @brief 1000:FADA: the per-member restore inside a dissolve.
 *
 * @param rec the record
 */
static void _obj_units_group_restore_member(uint8_t *rec)
{
    wr_le_u16(rec + 4u, 0x0010u); // frame 0x10
    wr_le_u16(rec + 8u, 0x0003u); // type 3
    wr_le_u16(rec + 0xAu, 0x0001u);
    wr_le_u16(rec + 0xCu, 0x0004u);
    wr_le_u16(rec + 0xEu, 0x0000u);
}


void object_units_group_dissolve_at(uint32_t slot)
{
    if (slot < 8u) _obj_units_group_dissolve((uint8_t *)_obj_register[slot]);
}


/**
 * @brief 1000:FAC1/FAC4: clear the group entry's five list words.
 *
 * @param e the group entry
 */
static void _obj_units_group_clear_list(uint8_t *e)
{
    for (uint16_t i = 0; i < 5u; i++) // 1000:FAC1/FAC4: five words
        wr_le_u16(e + 4u + 2u * i, 0xFFFFu);
}


/**
 * @brief 1000:FA16: dissolve a group entry.
 *
 * The last member is released as a lone human (frame 0x10, type 2); the
 * entry's five list words are then cleared.
 *
 * @param e the group entry
 */
static void _obj_units_group_dissolve(uint8_t *e)
{
    uint8_t *bp;
    uint8_t n;
    uint8_t *owner;

    wr_le_u16(e + 0u, 0xFFFFu); // 1000:FA16
    wr_le_u16(e + 2u, 0xFFFFu); // 1000:FA1C
    bp = e + 5;
    while (!(bp[0] & 0x80u)) bp++; // past the terminator
    n = (uint8_t)((uint32_t)(bp - (e + 5)) - 1u); // list length - 1
    if (n == 0u) {
        owner = object_core_slot(bp[-1]); // 1000:FA49: the only member
        if (owner) {
            wr_le_u16(owner + 4u, 0x0010u); // frame 0x10
            wr_le_u16(owner + 8u, 0x0002u); // type 2, a human
        }
        _obj_units_group_clear_list(e); // 1000:FABF
        return;
    }
    /* 1000:FA64/0xFA78: reset the last n list bytes (top .. second). */
    for (uint16_t k = 0u; k < n; k++) {
        uint8_t *m = object_core_slot(bp[-1 - (int32_t)k]);

        if (m) _obj_units_group_restore_member(m);
    }
    /* 1000:FAA4: one more DEC BP -> the first list byte (the owner/base). */
    owner = object_core_slot(bp[-1 - (int32_t)n]);
    if (owner) {
        wr_le_u16(owner + 2u, (uint16_t)(rd_le_u16(owner + 2u) - 8u));
        wr_le_u16(owner + 4u, 0x0010u);
        wr_le_u16(owner + 8u, 0x0002u);
    }
    _obj_units_group_clear_list(e); // 1000:FABF
}


/* 0x6EEB: the placed item's frame by the found record's behaviour (the byte run
 * at 0x6EC3 + 0x28).  Behaviours 0..9 are the shipped set (the spawns write
 * 0/1/2/3/5/8/9, the recipes 4/5/6/7); the original reads on into the 0x6F0D
 * dispatch for higher values, which needs a save the game never writes
 * (D-272). */
static const uint8_t _obj_stack_item_frame[10] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x60u, 0x9Cu, 0x96u, 0xFEu, 0xFEu,
};


/** @brief 1000:F4D9..0xF5BB: the `DL & 1` class-transfer tail of `0xF453`.  A
 * dissolved record with a class hands the carried live record back (or, for a
 * behaviour-4 slot, lifts the live projectile into the record), clears the
 * class and, when the record is the acting one, arms event 5.
 *
 * @param recIndex the record index (static or live)
 */
static void _obj_units_class_transfer(uint32_t recIndex)
{
    uint8_t *rec = _obj_array + recIndex * OBJ_STRIDE;
    uint8_t cls = rec[7];
    uint8_t *live, *slot;
    uint8_t beh;

    if (!cls) return; // 1000:F4E6
    rec[7] = 0u; // 1000:F4EF
    live = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE; // 0x3F17+0x12*(cls-1)
    slot = _obj_slot_class + cls; // 0x3E5F + cls - 1
    *slot &= 0x7Fu; // 1000:F504
    beh = (uint8_t)((*slot & 0x3Fu));
    if (beh == 4u) { // 1000:F50B: the live record is the wheel
        uint32_t ax, bx;

        wr_le_u16(live + 0xEu, 0xFFFFu);
        live[6] &= 0x7Fu;
        ax = (rd_le_u16(live + 8u) & 0x8000u) ? 1u : 0u;
        bx = ax ? 8u : 0u;
        wr_le_u16(rec + 0u, rd_le_u16(live + 0u)); // 1000:F52D
        wr_le_u16(rec + 2u, (uint16_t)(rd_le_u16(live + 2u) - 8u));
        wr_le_u16(rec + 4u, (uint16_t)bx);
        wr_le_u16(rec + 8u, (uint16_t)ax);
        wr_le_u16(rec + 0xAu, 0u);
    } else { // 1000:F54B: the found record becomes the placed item
        /* the frame is `byte[0x6EEB + behaviour]`; behaviours 0..9 are the
         * shipped set and the later entries need a save the game never writes
         * (D-272). */
        wr_le_u16(live + 0u, rd_le_u16(rec + 0u));
        wr_le_u16(live + 2u, (uint16_t)(rd_le_u16(rec + 2u) + 8u));
        wr_le_u16(live + 4u, beh <= 9u ? _obj_stack_item_frame[beh] : 0u);
        wr_le_u16(live + 6u, 0u);
        wr_le_u16(live + 8u, 0xFFFFu);
        wr_le_u16(live + 0xAu, 0u);
        wr_le_u16(live + 0xCu, 0u);
        wr_le_u16(live + 0xEu, 0u);
    }
    if (recIndex != (uint32_t)_obj_player &&
        recIndex == (uint32_t)_obj_entity) { // 1000:F597
        _obj_sequence_set_program_index(1u); // [0x5B56] = 1
        _obj_result_pending = 5u; // [0x6916] = 0x105
        _obj_result_cursor = 1u;
        _obj_pending = 5u; // [0x1979] = 5
        _obj_cursor = 1u; // [0x1988] = 1
    }
}


int32_t object_units_stack_scan_dissolve(uint32_t recIndex)
{
    uint8_t found = 0;

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        if (e[1] & 0x80u) continue; // +0 bit 15 set = free
        for (uint8_t *p = e + 5; !(p[0] & 0x80u); p++)
            if (p[0] == (uint8_t)recIndex) {
                _obj_units_group_dissolve(e);
                found = 1;
                break;
            }
        if (found) break;
    }
    _obj_units_class_transfer(recIndex);
    return found;
}
