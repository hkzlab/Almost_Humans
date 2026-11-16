#include "object_internal.h"
#include "object_core.h"
#include "object_units.h"
#include "object_walk.h"
#include "entity.h"
#include "level/level.h"
#include "byteorder.h"

/* forward declarations */
static void _obj_walk_step1(uint8_t *r);
static void _obj_walk_seq(uint8_t *r);
static void _obj_walk_wander(uint8_t *r);
static void _obj_walk_torch(uint8_t *r);
static void _obj_walk_sum_step(const uint8_t *tab, const uint8_t *r,
                               int16_t *ax, int16_t *bx);
static int32_t _obj_walk_land_probe(int16_t ax, int16_t bx, int16_t *cxOut);
static void _obj_walk_move_kill(uint8_t *r, uint32_t idx);
static void _obj_walk_move_settle(uint8_t *r, int16_t cx);
static void _obj_walk_move_equal(uint8_t *r);
static void _obj_walk_move_impact(uint8_t *r, const uint8_t *tab, uint32_t idx);
static void _obj_walk_move_pair_b(uint8_t *r, const uint8_t *tab, int16_t selB,
                                  uint32_t idx);
static void _obj_walk_move_scan(uint8_t *r, const uint8_t *tab, int16_t selC,
                                int16_t selB);
static void _obj_walk_selectors(uint16_t oldFrame, uint16_t newFrame,
                                int16_t *selC, int16_t *selB);
static void _obj_walk_rope(uint8_t *r);
static void _obj_walk_mover_pass_records(uint8_t *r, int32_t ax, int32_t bx);
static void _obj_walk_mover_pass_entity(uint8_t *r, int32_t ax, int32_t bx);
static void _obj_walk_mover_pass_placed(uint8_t *r, int32_t ax, int32_t bx);
static void _obj_walk_mover_stepper(uint8_t *r, uint32_t mode);
static void _obj_walk_mover(uint8_t *r);
static void _obj_walk_stack8_fail(uint8_t *r, uint32_t frame);
static void _obj_walk_stack8_slots(uint8_t *r);
static void _obj_walk_stack8_move(uint8_t *r, int32_t vx, int32_t vy,
                                  uint32_t stopFrame, uint32_t endFrame);
static void _obj_walk_stack8_anim(uint8_t *r, uint32_t endFrame);
static void _obj_walk_stack8_land(uint8_t *r, uint32_t bx, int32_t ax,
                                  int32_t bx2, uint32_t idx);
static void _obj_walk_stack(uint8_t *r, uint32_t idx);
static int32_t _obj_walk_proj_cull(int32_t vx, int16_t *x);
static int32_t _obj_walk_proj_offpage(uint8_t *r);
static int32_t _obj_walk_proj_advance(uint8_t *r, int16_t bx, int16_t cx);
static void _obj_walk_proj_bounce(int16_t *vx, int16_t *vy);
static void _obj_walk_proj_burst(uint8_t *r, int16_t bx, int16_t cx,
                                 int16_t *vx, int16_t *vy);
static int32_t _obj_walk_hit_delay(uint8_t *r, uint16_t *s38, uint16_t *s3c);
static void _obj_walk_hit_dispatch(uint8_t *r, uint8_t *t, int16_t frame,
                                   int16_t vx, int16_t vy);
static void _obj_walk_proj_target(uint8_t *r, int32_t x, int32_t y,
                                  int32_t frame, int32_t vx, int32_t vy);
static void _obj_walk_projectile(uint8_t *r, uint32_t idx);
static void _obj_walk_marker(uint8_t *r);


/* --- 1000:C295 update_list: the placed records' behaviour walk ---------
 *
 * `update_world` (1000:80AD at 1000:80BD) walks the live records from 0x3F17
 * with the parallel behaviour bytes at 0x3E5F and dispatches through the
 * ten-word table at 202f:0x6535 (`byte & 0x3F`).  The handlers below are the
 * animation/movement ones; 5's/7's move scan (1000:E039) and 4's projectile
 * machinery are D-88/D-97/D-99.  The render pass draws every record.
 */
static const uint8_t _obj_beh3_frame_run[0x64] = {
    0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB1, 0xB2, 0xB3, 0xB1, 0xB2, 0xB3, 0xB1,
    0xB2, 0xB3, 0xB1, 0xB2, 0xB3, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0x00,
    0xC0, 0xCC, 0xE3, 0xCC, 0x06, 0xCD, 0x4A, 0xCD, 0x4A, 0xCD, 0x4A, 0xCD,
    0x08, 0x0F, 0xFF, 0xFF, 0x08, 0x0B, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x08, 0x0F, 0xFF, 0xFF, 0x08, 0x0F, 0xFF, 0xFF, 0x08, 0x0F, 0xFF, 0xFF,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x08, 0x0C, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x08, 0x0C, 0xFF, 0xFF,
};


/* 0x6612: 16 groups (frames 0x58..0x67), each **two** (dx,dy) pairs = 4 bytes.
 * 1000:CE88 reads pair A's `dy` at `+1` and pair B's `dy` at `+3`; 1000:E039
 * projects with pair A and, on its second pass, with pair B. */
static const uint8_t _obj_mover_tab[0x40] = {
    0x07, 0x00, 0x07, 0x0F, 0x0C, 0x00, 0x02, 0x0F, 0x0F, 0x00, 0x00,
    0x0D, 0x0F, 0x03, 0x00, 0x0A, 0x0F, 0x07, 0x00, 0x07, 0x0F, 0x0C,
    0x00, 0x02, 0x0F, 0x0F, 0x02, 0x00, 0x0A, 0x0F, 0x05, 0x00, 0x08,
    0x0F, 0x08, 0x00, 0x03, 0x0F, 0x0D, 0x00, 0x00, 0x0F, 0x0F, 0x02,
    0x00, 0x0B, 0x0F, 0x05, 0x00, 0x08, 0x0F, 0x08, 0x00, 0x03, 0x0F,
    0x0D, 0x00, 0x00, 0x0D, 0x0F, 0x04, 0x00, 0x0A, 0x0F,
};


/* 0x68EE: the settled frame for a mover's frame index 0..0xF (1000:E0BF). */
static const uint8_t _obj_settle_frame[0x10] = {
    0x5C, 0x5C, 0x5C, 0x5C, 0x5C, 0x5D, 0x5E, 0x5F,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x64, 0x64, 0x64,
};


/* 0x68DE, the equal-landing y step for frames 0x58..0x67, indexed by
 * `frame - 0x58` (1000:E163).  The original reads on past the 16 bytes for
 * frames above 0x67; the port defaults to no step (D-101).  Only behaviour 5's
 * pair-B groups (frames 0x58..0x5B / 0x64..0x67) and behaviour 7's frame 0x96
 * reach the arm, and 0x96's pair-B bit is clear, so the tail is unreachable on
 * shipped data. */
static const int8_t _obj_land_step[0x10] = {
    0x00, 0x02, 0x03, 0x03, -1, -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1, 0x05, 0x02, 0x00,
};


/* 0x667F: the hit record's frame -> class byte map 1000:D268/0xD273 uses. */
static const uint8_t _obj_hit_frame[0x16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01,
};


/* 0x658C: behaviour 7's own two-pair step table (frames 0x96..0xA5), used by
 * 1000:CD4A. */
static const uint8_t _obj_rope_tab[0x40] = {
    0x08, 0x0F, 0xFF, 0xFF, 0x08, 0x0B, 0xFF, 0xFF, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x08, 0x0F, 0xFF, 0xFF, 0x08, 0x0F, 0xFF, 0xFF, 0x08,
    0x0F, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x08, 0x0C, 0xFF, 0xFF, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x0C, 0xFF, 0xFF,
};


/* 0x65D2: the direction -> frame table (16 rows of 4?). */
static const uint8_t _obj_mover_dir[0x40] = {
    0x67, 0x66, 0x66, 0x67, 0x59, 0x5A, 0x5A, 0x59, 0x66, 0x65, 0x66,
    0x66, 0x5A, 0x5B, 0x5A, 0x5A, 0x65, 0x65, 0x65, 0x65, 0x5B, 0x5B,
    0x5B, 0x5B, 0x64, 0x64, 0x64, 0x64, 0x5C, 0x5C, 0x5C, 0x5C, 0x64,
    0x64, 0x64, 0x64, 0x5C, 0x5C, 0x5C, 0x5C, 0x63, 0x63, 0x63, 0x63,
    0x5D, 0x5D, 0x5D, 0x5D, 0x62, 0x63, 0x62, 0x62, 0x5E, 0x5D, 0x5E,
    0x5E, 0x61, 0x62, 0x62, 0x61, 0x5F, 0x5E, 0x5E, 0x5F,
};


/* --- 0xCEC3 (behaviour 4): record A's projectile ----------------------
 *
 * Two phases.  Phase 1 (vy == 0) flies horizontally: the tile under
 * (x+8, y+0x10) gives a signed slope through the field-A table ([0x1FD4]),
 * which is added to vx together with the +-1 step at 0x6695; the fuse at
 * +0xC is recomputed as 8 - (|vx|>>2) when it expires and the frame cycles
 * 0x38..0x3B.  Phase 2 (vy != 0) is the fall: the signed byte curve at
 * 0x6652 (indexed by vy) is added to y, and the landing test resolves the
 * target record through the +0xE index (0x3E24), writing its 0x28/0/0x16/5
 * state.  The draw dispatch (0x666B) and the terrain bounce (0xD34E, which
 * draws nothing despite its name) are D-97/D-99.
 */
static const int8_t _obj_proj_curve[25] = {
    0, -4, -3, -3, -2, -2, -2, -1, -1, -1, -1, 0, 0,
    0, 0,  0,  1,  1,  1,  1,  2,  2,  2,  3,  3,
};


/**
 * @brief 0xC77B (behaviour 1): an 8-tick frame stepper between +8 and +8 + [0x6532].
 *
 * @param r the record
 */
static void _obj_walk_step1(uint8_t *r)
{
    uint16_t a = rd_le_u16(r + 0xAu);

    if (a != 0u) {
        wr_le_u16(r + 0xAu, a - 1u);
        return;
    }
    wr_le_u16(r + 0xAu, 8u);
    {
        uint16_t base = rd_le_u16(r + 8u);
        uint16_t f = (uint16_t)(rd_le_u16(r + 4u) + 1u);

        if (f >= base + level_anim_frames()) f = base;
        wr_le_u16(r + 4u, f);
    }
}


/** @brief 0xCBFE (behaviour 3): a 4-tick sequence player over 0x6568, +0xC the cursor;
 * the end of a run sets +8 = 2 and clears the map cell at +0xE.
 *
 * @param r the record
 */
static void _obj_walk_seq(uint8_t *r)
{
    uint32_t a, cur, i;

    if (rd_le_u16(r + 8u) != 1u) return;
    a = rd_le_u16(r + 0xAu);
    if (a != 0u) {
        wr_le_u16(r + 0xAu, (uint16_t)(a - 1u));
        return;
    }
    wr_le_u16(r + 0xAu, 4u);
    cur = rd_le_u16(r + 0xCu);
    wr_le_u16(r + 0xCu, (uint16_t)(cur + 1u));
    i = cur;
    wr_le_u16(r + 4u,
              i < sizeof _obj_beh3_frame_run ? _obj_beh3_frame_run[i] : 0u);
    if (i + 1u < sizeof _obj_beh3_frame_run &&
        _obj_beh3_frame_run[i + 1u] == 0u) {
        uint8_t *map = (uint8_t *)level_map(); // the writable [0x1FA0] copy

        wr_le_u16(r + 8u, 2u);
        if (map && rd_le_u16(r + 0xEu) < OBJ_MAP_CELLS)
            map[rd_le_u16(r + 0xEu)] = 0u;
    }
}


/** @brief 0xCC39 (behaviour 2): the wanderer, on even frames.  The state lives in +8
 * (0 = walking right, 1 = left, 2 = arrived); +0x10 is a 4-tick divider.
 *
 * @param r the record
 */
static void _obj_walk_wander(uint8_t *r)
{
    uint32_t ax, bx, cx, dx;

    if (_obj_frame & 1u) return;
    ax = rd_le_u16(r + 0u);
    bx = rd_le_u16(r + 4u);
    cx = rd_le_u16(r + 8u);
    dx = rd_le_u16(r + 0x10u);
    switch (cx) {
    case 0: // 0xCCC0
        ax++;
        if (dx == 0u) {
            dx = 3u;
            bx++;
            if (bx > 0x85u) bx = 0x82u;
            if (ax >= rd_le_u16(r + 0xCu)) {
                bx = 0x86u;
                cx = 1u;
            }
        } else dx--;
        break;
    case 1: // 0xCCE3
        ax--;
        if (dx == 0u) {
            dx = 3u;
            bx++;
            if (bx > 0x89u) bx = 0x86u;
            if (ax > rd_le_u16(r + 0xAu)) {
                bx = 0x82u;
                cx = 0u;
            }
        } else dx--;
        break;
    default: bx = 0x8Au; break; // 0xCD06
    }
    wr_le_u16(r + 0u, (uint16_t)ax);
    wr_le_u16(r + 4u, (uint16_t)bx);
    wr_le_u16(r + 0x10u, (uint16_t)dx);
    wr_le_u16(r + 8u, (uint16_t)cx);
    if (cx != 0u) return;
    /* 1000:CC65: the (x-3, y-8) match against the eight fixed slots. */
    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *slot = _obj_array + i * OBJ_STRIDE;
        uint16_t type;

        if ((uint32_t)(int16_t)(ax - 3) != rd_le_u16(slot + 0u)) continue;
        if ((uint32_t)(int16_t)(bx - 8) != rd_le_u16(slot + 2u)) continue;
        type = rd_le_u16(slot + 8u);
        if (type > 2u) return;
        if (rd_le_u16(r + 0xEu) == 0u) {
            wr_le_u16(r + 0xEu, 5u);
            wr_le_u16(r + 8u, 2u);
            wr_le_u16(slot + 0xAu, 0u);
            wr_le_u16(slot + 0xCu, 0u);
            wr_le_u16(slot + 0xEu, rd_le_u16(slot + 4u));
            wr_le_u16(slot + 0x10u, type);
            wr_le_u16(slot + 8u, 0x11u);
        } else {
            wr_le_u16(r + 0xEu, (uint16_t)(rd_le_u16(r + 0xEu) - 1u));
        }
        return;
    }
}


/**
 * @brief The torch walk tail.
 *
 * @param r the record
 */
static void _obj_walk_torch(uint8_t *r)
{
    if ((_obj_frame & 3u) == 0u && rd_le_u16(r + 8u) == 0xFFFFu &&
        rd_le_u16(r + 0xAu) == 0u) {
        uint16_t f = (uint16_t)(rd_le_u16(r + 4u) + 1u);

        if (f >= 0x9Fu) f = 0x9Cu;
        wr_le_u16(r + 4u, f);
    }
    _obj_walk_rope(r); // 1000:CD2C -> 1000:CD2E
}


/** @brief 1000:E295 sum_three_fields: add the frame's pair A/B deltas.
 *
 * Behaviour 5/7's steppers call this from object_collision_scan (1000:E039)
 * after choosing a frame; it is a move, not a draw.  `tab` points at the
 * frame's 4-byte group: pair A (dx,dy) at +0/+1, pair B at +2/+3.
 *
 * @param tab the table
 * @param r the record
 * @param ax the output x
 * @param bx the output y
 */
static void _obj_walk_sum_step(const uint8_t *tab, const uint8_t *r,
                               int16_t *ax, int16_t *bx)
{
    *ax = (int16_t)(uint16_t)(tab[0] + rd_le_u16(r + 0u) + rd_le_u16(r + 0xCu));
    *bx = (int16_t)(uint16_t)(tab[1] + rd_le_u16(r + 2u) + rd_le_u16(r + 0xEu));
}


/** @brief 1000:E2B5 collision_land_probe: the landing row for the projected point
 * (AX, BX).  Returns 0 when the caller must take 1000:E194.
 *
 * @param ax the x coordinate
 * @param bx the y coordinate
 * @param cxOut receives the landing row
 * @return 1 when a landing row was found, 0 otherwise
 */
static int32_t _obj_walk_land_probe(int16_t ax, int16_t bx, int16_t *cxOut)
{
    uint16_t a = (uint16_t)ax, b = (uint16_t)bx;
    uint32_t si = (a >> 4) + 2u * (b & 0xFFF0u), sub = a & 0xFu;
    int32_t page;
    int16_t cx;

    if (bx < 0) return 0; // 1000:E2B8
    cx = (int16_t)(0x140 - b);
    if (cx < 0) return 0; // 1000:E2C4
    cx = (int16_t)(b & 0xFFF0u); // 1000:E2E3: CX = the row base
    page = (int32_t)(int8_t)(uint8_t)level_page_byte(
        _obj_core_map_buffer_byte((int32_t)si), sub);
    if (page < 0) { // 1000:E33A
        cx = (int16_t)(cx + 0x10);
        page = (int32_t)(int8_t)(uint8_t)level_page_byte(
            _obj_core_map_buffer_byte((int32_t)(si + 0x20u)), sub);
        if (page < 0) return 0;
    } else if (page == 0) { // 1000:E304
        uint8_t above =
            (uint8_t)(_obj_core_map_buffer_byte((int32_t)(si - 0x20u)));

        /* 1000:E30E/E314 compare against the field-K *second* word
         * ([0x6F0B]/[0x6F0C]), unlike the settle and the step states, which
         * read the first ([0x6F20]/[0x6F21]). */
        if (above == level_special_tile(2u) ||
            above == level_special_tile(3u)) {
            page = 0;
        } else {
            int32_t up = (int32_t)(int8_t)(uint8_t)level_page_byte(above, sub);

            if (up < 0) page = 0;
            else {
                page = up;
                cx = (int16_t)(cx - 0x10);
            }
        }
    }
    cx = (int16_t)(cx + page); // 1000:E354
    *cxOut = cx;
    return cx >= 0;
}


/**
 * @brief The move scan's kill tail: hide the record and clear its class.
 *
 * @param r the record
 * @param idx the index
 */
static void _obj_walk_move_kill(uint8_t *r, uint32_t idx)
{
    r[6] = 0xFFu;
    if (idx >= 9u && idx - 8u < sizeof _obj_slot_class)
        _obj_slot_class[idx - 8u] = 0u;
}


/**
 * @brief The move scan's settle tail (1000:E0D8): apply the step.
 *
 * @param r the record
 * @param cx the landing difference
 */
static void _obj_walk_move_settle(uint8_t *r, int16_t cx)
{
    /* 1000:E0D8: x by +0xC, y by +0xE less the landing difference */
    wr_le_u16(r + 2u, (uint16_t)((int16_t)rd_le_u16(r + 2u) - cx +
                                 (int16_t)rd_le_u16(r + 0xEu)));
    wr_le_u16(r + 0u, (uint16_t)((int16_t)rd_le_u16(r + 0u) +
                                 (int16_t)rd_le_u16(r + 0xCu)));
    wr_le_u16(r + 0xCu, 0u);
    wr_le_u16(r + 0xEu, 0u);
    wr_le_u16(r + 0xAu, 0u);
}


/**
 * @brief The move scan's equal-distance landing (1000:E159).
 *
 * @param r the record
 */
static void _obj_walk_move_equal(uint8_t *r)
{
    int16_t fi = (int16_t)(rd_le_u16(r + 4u) - 0x58u);
    int8_t step = -1;

    if ((uint16_t)fi < sizeof _obj_land_step)
        step = _obj_land_step[(uint16_t)fi];
    if (step >= 0) {
        wr_le_u16(r + 2u, (uint16_t)((int16_t)rd_le_u16(r + 2u) + step));
        wr_le_u16(r + 4u,
                  (uint16_t)(rd_le_u16(r + 4u) + (fi < 8 ? 1u : 0xFFFFu)));
    }
    wr_le_u16(r + 0xEu, 0u); // 1000:E17D
    wr_le_u16(r + 0xAu, 0u);
    wr_le_u16(r + 0xCu, 0u);
}


/**
 * @brief The move scan's impact tail (1000:E194): bounce or step.
 *
 * @param r the record
 * @param tab the table
 * @param idx the index
 */
static void _obj_walk_move_impact(uint8_t *r, const uint8_t *tab, uint32_t idx)
{
    int16_t ax, bx;

    /* 1000:E194: the projected point is inside the terrain (or the landing
     * probe failed).  Probe the tile above and the cell's item word. */
    if (rd_le_u16(r + 0xAu) == 0u) {
        uint32_t si, sub, word, hi, lo, k;

        _obj_walk_sum_step(tab, r, &ax, &bx);
        sub = (uint16_t)ax & 0xFu;
        si = ((uint16_t)ax >> 4) + 2u * ((uint16_t)bx & 0xFFF0u);
        if (_obj_core_map_buffer_byte((int32_t)(si - 0x20u)) >= 0xF0u) {
            _obj_walk_move_kill(r, idx);
            return;
        }
        word = level_item_word(_obj_core_map_buffer_byte((int32_t)si),
                               (uint16_t)bx & 0xFu);
        hi = (rd_le_u16(r + 0xCu) & 0x8000u) ? 0xFFFFu : 0u;
        lo = hi ? 0u : 0xFFFFu;
        for (k = 0; k < sub; k++) {
            uint8_t carry = hi & 1u;

            hi >>= 1;
            lo = (lo >> 1) | (carry << 15);
        }
        if ((word & lo) != 0u) {
            int16_t vx = (int16_t)rd_le_u16(r + 0xCu), nvx;

            wr_le_u16(r + 0xEu, 0u); // 1000:E213
            if (vx > 0) nvx = vx > 4 ? (int16_t)-(vx - 4) : 0;
            else if (vx < 0) nvx = vx < -4 ? (int16_t)(-vx - 4) : 0;
            else nvx = 0;
            wr_le_u16(r + 0xCu, (uint16_t)nvx); // 1000:E231
            wr_le_u16(r + 0xAu,
                      (uint16_t)((int16_t)rd_le_u16(r + 2u) < 0x60 ? 1 : 2));
            if ((int16_t)rd_le_u16(r + 0xEu) < 3)
                wr_le_u16(r + 0xEu, 3u); // 1000:E24C
        }
    }
    { // 1000:E252: the vertical step, incremented and clamped to 0xF
        int16_t step = (int16_t)rd_le_u16(r + 0xEu);
        int16_t next = (int16_t)(step + 1);

        if (next >= 0 && next > 0xF) next = 0xF;
        wr_le_u16(r + 0xEu, (uint16_t)next);
        wr_le_u16(r + 2u, (uint16_t)((int16_t)rd_le_u16(r + 2u) + step));
        wr_le_u16(r + 0u, (uint16_t)((int16_t)rd_le_u16(r + 0u) +
                                     (int16_t)rd_le_u16(r + 0xCu)));
    }
}


/**
 * @brief Pair B's projection (1000:E102), gated by the pair sign and frames.
 *
 * @param r the record
 * @param tab the table
 * @param selB pair B's selector
 * @param idx the index
 */
static void _obj_walk_move_pair_b(uint8_t *r, const uint8_t *tab, int16_t selB,
                                  uint32_t idx)
{
    int16_t ax, bx, cx, d;

    if ((tab[2] | (tab[3] << 8)) & 0x8000u) {
        _obj_walk_move_impact(r, tab, idx);
        return;
    }
    if (!((uint16_t)(1u << ((rd_le_u16(r + 4u) - 0x58u) & 0x1Fu)) &
          OBJ_PAIRB_FRAMES)) {
        _obj_walk_move_impact(r, tab, idx);
        return;
    }
    _obj_walk_sum_step(tab + 2, r, &ax, &bx);
    if (!_obj_walk_land_probe(ax, bx, &cx)) {
        _obj_walk_move_impact(r, tab, idx);
        return;
    }
    d = (int16_t)(cx - bx);
    if (d == 0) {
        _obj_walk_move_equal(r);
        return;
    }
    if (d > 0) {
        _obj_walk_move_impact(r, tab, idx);
        return;
    }
    cx = (int16_t)(-d);
    {
        int16_t t = (int16_t)(selB + cx);

        if (t >= 0 && t > (int16_t)rd_le_u16(r + 0xEu)) {
            _obj_walk_move_impact(r, tab, idx);
            return;
        }
    }
    _obj_walk_move_settle(r, cx);
}


/**
 * @brief The landing scan for one move step (1000:E045..E27A).
 *
 * @param r the record
 * @param tab the table
 * @param selC pair C's selector
 * @param selB pair B's selector
 */
static void _obj_walk_move_scan(uint8_t *r, const uint8_t *tab, int16_t selC,
                                int16_t selB)
{
    uint8_t idx = (uint8_t)((uint32_t)((r - _obj_array) / OBJ_STRIDE));
    int16_t ax, bx, cx, d;

    _obj_walk_sum_step(tab, r, &ax, &bx);
    if (ax < 0 || ax >= 0x200 || bx >= 0x150) { // 1000:E045..E05A
        _obj_walk_move_kill(r, idx);
        return;
    }
    if (rd_le_u16(r + 0xEu) & 0x8000u) { // 1000:E05D
        _obj_walk_move_impact(r, tab, idx);
        return;
    }
    if (!_obj_walk_land_probe(ax, bx, &cx)) { // 1000:E068
        _obj_walk_move_impact(r, tab, idx);
        return;
    }
    d = (int16_t)(cx - bx);
    if (d == 0) cx = 0;
    else if (d < 0) cx = (int16_t)(-d);
    else {
        _obj_walk_move_pair_b(r, tab, selB, idx);
        return;
    }
    { // 1000:E07B
        int16_t t = (int16_t)(selC + cx);

        if (t >= 0 && t > (int16_t)rd_le_u16(r + 0xEu)) {
            _obj_walk_move_pair_b(r, tab, selB, idx);
            return;
        }
    }
    if (rd_le_u16(r + 0xEu) != 0u) _obj_core_driver_call(5); // 1000:E097
    { // 1000:E0A7: the settled frame, and y by its pair-A dy change
        int16_t fi = (int16_t)(rd_le_u16(r + 4u) - 0x58u);

        if (fi >= 0 && fi < 0x10) {
            int32_t i = fi;
            int32_t dyA = (int8_t)_obj_mover_tab[4 * i + 1];
            int32_t nf = _obj_settle_frame[i];
            int32_t dyNew = (int8_t)_obj_mover_tab[4 * (nf - 0x58) + 1];

            wr_le_u16(r + 4u, (uint16_t)nf);
            wr_le_u16(r + 2u, (uint16_t)((int16_t)rd_le_u16(r + 2u) +
                                         (int16_t)(dyA - dyNew)));
        }
    }
    _obj_walk_move_settle(r, cx);
}


/** @brief 1000:CE88 compute_object_selectors: the pair-A/pair-B `dy` changes between
 * the old and the new frame, stored into [0x423C]/[0x4238].
 *
 * @param oldFrame the old frame
 * @param newFrame the new frame
 * @param selC pair C's selector
 * @param selB pair B's selector
 */
static void _obj_walk_selectors(uint16_t oldFrame, uint16_t newFrame,
                                int16_t *selC, int16_t *selB)
{
    int32_t o = (int16_t)(oldFrame - 0x58u) * 4;
    int32_t n = (int16_t)(newFrame - 0x58u) * 4;

    *selC = (int16_t)((int8_t)_obj_mover_tab[o + 1] -
                      (int8_t)_obj_mover_tab[n + 1]);
    *selB = (int16_t)((int8_t)_obj_mover_tab[o + 3] -
                      (int8_t)_obj_mover_tab[n + 3]);
}


/* Rope mover: advance the record along its 0x658C table. */
static void _obj_walk_rope(uint8_t *r)
{
    uint16_t f;
    const uint8_t *tab;

    if (!(rd_le_u16(r + 8u) & 0x8000u)) return; // 1000:CD33
    switch (rd_le_u16(r + 0xAu)) { // 0x6586 holds CD4A x3
    case 0u:
    case 1u:
    case 2u: break;
    default: return; // D-101: past the three-entry table
    }
    if (_obj_frame & 1u) return; // 1000:CD4A: even frames
    f = rd_le_u16(r + 4u);
    if (f < 0x96u || f > 0xA5u) return; // D-101: past the 0x658C table
    tab = _obj_rope_tab + 4u * (f - 0x96u);
    _obj_walk_move_scan(r, tab, 0, 0);
}


/** @brief 1000:CD71 (behaviour 5): the mover's collision passes ----------------
 *
 * The handler first runs the stepper (`[0x65CC + 2*[+0xA]]`, 0xCDC7/0xCE38/
 * 0xCE3D) - which moves the record through 0xE039 - then projects the record
 * again by the 0x6612 (dx,dy) pair and hands the projected AX/BX to three
 * passes.  The passes write only when they hit.  0x58D1 (record-vs-entity,
 * testing the entity's REX sprite pixels) is `_obj_walk_mover_pass_entity` below;
 * 0x5A2F and 0x5B3E are `_obj_walk_mover_pass_records` / `_obj_walk_mover_pass_placed`.
 *
 * @param r the record
 * @param ax the x position
 * @param bx the y position
 */
static void _obj_walk_mover_pass_records(uint8_t *r, int32_t ax, int32_t bx)
{
    for (uint16_t i = 0; i <= _obj_live_count; i++) {
        uint8_t *t = _obj_array + i * OBJ_STRIDE;
        uint8_t *beh = _obj_slot_class + (i - 8u);
        int32_t dx, dy;

        if (i < OBJ_LIVE0 + 1u) continue;
        if (*beh != 8u) continue; // 1000:5A41 compares the whole byte
        dx = (int16_t)rd_le_u16(t + 0u) - ax;
        if (!(dx < 0 && dx + 0x10 >= 0)) continue; // 1000:5A55/0x5A5D
        dy = (int16_t)rd_le_u16(t + 2u) - bx;
        if (!(dy < 0 && dy + 0x1A >= 0)) continue; // 1000:5A66/0x5A6E
        if (rd_le_u16(r + 0xCu) != 0u || (rd_le_u16(r + 0xEu) & 0x8000u) ||
            rd_le_u16(r + 0xEu) > 1u) {
            uint8_t cls;

            wr_le_u16(t + 0u, (uint16_t)(rd_le_u16(t + 0u) - 8u));
            wr_le_u16(t + 4u, 0x0028u);
            wr_le_u16(t + 8u, 0x0003u);
            wr_le_u16(t + 0xAu, 6u);
            cls = t[7];
            if (cls) {
                _obj_core_class_byte_set(cls, 0u); // 0x3E5F + cls - 1
                if (cls >= 1u && OBJ_LIVE0 + cls <= _obj_live_count)
                    _obj_array[(OBJ_LIVE0 + cls) * OBJ_STRIDE + 6] = 0xFFu;
            }
            /* 1000:5B13..0x5B24: the MOVER's behaviour byte and its +6 are
             * cleared (SI = the mover's +6), NOT the victim's: the victim
             * (behaviour 8) must keep its script so its state-6 exit can
             * animate frames 0x28..0x2E and free itself (D-229). */
            _obj_slot_class[(uint32_t)((r - _obj_array) / OBJ_STRIDE) - 8u] =
                0u;
            r[6] = 0xFFu;
            _obj_core_driver_call(0x0Cu);
            return;
        }
        if (t[7] == 0u) {
            /* 1000:5A9D..0x5AD6: clear the ACTING record's +6/+8/+0xA/+0xC,
             * OR 0x80 into the ACTING record's class byte and link the
             * candidate to it by that class ordinal; the scan then moves on
             * to the next record (0x5B31). */
            uint8_t ord = (uint8_t)((r - _obj_array) / OBJ_STRIDE) - 8u;

            wr_le_u16(r + 6u, 0u);
            wr_le_u16(r + 8u, 0u);
            wr_le_u16(r + 0xAu, 0u);
            wr_le_u16(r + 0xCu, 0u);
            _obj_slot_class[ord] |= 0x80u;
            t[7] = ord;
        }
    }
}


/** @brief 0x58D1: the record-vs-entity pass.  Walks the entity array, keeps the
 * entities whose handler is < 2, and tests the entity's REX sprite pixel at
 * the record's projected offset (the 0xE0/0x150/0x2+4a tables via
 * entity_frame_info).
 *
 * @param r the record
 * @param ax the x position
 * @param bx the y position
 */
static void _obj_walk_mover_pass_entity(uint8_t *r, int32_t ax, int32_t bx)
{
    uint16_t n = entity_count();

    if (rd_le_u16(r + 0xCu) == 0u && rd_le_u16(r + 0xEu) == 0u) return;
    for (uint16_t i = 0; i < n; i++) {
        Entity *e = entity_at_mut(i);
        int32_t ex, anim;

        if (!e || e->handler >= 2u) continue;
        ex = (int16_t)e->x - ax;
        if (ex >= 0 || -ex >= 0x40) continue;
        if (ex < 0) ex = -ex;
        anim = (int16_t)e->waypoint;
        if (anim < 0) continue;
        anim <<= 1;
        if (ex > 0x1F) {
            ex -= 0x20;
            anim++;
        }
        {
            uint32_t src = 0, rows = 0, yoff = 0;
            int32_t dy;

            if (!entity_frame_info((uint32_t)anim, &src, &rows, &yoff))
                continue;
            dy = (int16_t)e->y + yoff - bx;
            if (dy >= 0) continue;
            if (dy < 0) dy = -dy;
            if ((uint32_t)dy >= rows) continue;
            if (!entity_sprite_pixel(src, rows, (uint32_t)ex, (uint32_t)dy))
                continue;
            _obj_slot_class[(uint32_t)((r - _obj_array) / OBJ_STRIDE) - 8u] =
                0u;
            r[6] = 0xFFu; // 1000:59F2: SI = record+6
            e->hold = 5u;
            e->waypoint =
                (e->handler != 0u && e->handler != 3u) ? 0x1Au : 0x0Cu;
            /* 1000:5A14: the DEC is unconditional, so a zero life word wraps
             * to 0xFFFF and does not enter handler 4. */
            e->life--;
            if (e->life == 0u) e->handler = 4u;
            return;
        }
    }
}


/**
 * @brief The mover pass over the placed records.
 *
 * @param r the record
 * @param ax the x position
 * @param bx the y position
 */
static void _obj_walk_mover_pass_placed(uint8_t *r, int32_t ax, int32_t bx)
{
    uint8_t i;

    /* 1000:5B3E's gate is on the record's +0xA/+0xC/+0xE (arg_SI = r+6). */
    if (rd_le_u16(r + 0xAu) != 0u) return;
    if (!(rd_le_u16(r + 0xCu) != 0u || (rd_le_u16(r + 0xEu) & 0x8000u) ||
          rd_le_u16(r + 0xEu) > 1u))
        return;
    for (i = 0; i < OBJ_STATIC; i++) {
        uint8_t *slot = _obj_array + i * OBJ_STRIDE;
        int32_t dx, dy;
        uint32_t type, cls;

        if (slot[6] & 0x80u) continue;
        dx = (int16_t)rd_le_u16(slot + 0u) - ax;
        if (!(dx < 0 && dx + 0x10 >= 0)) continue; // 1000:5B77/0x5B7F
        dy = (int16_t)rd_le_u16(slot + 2u) - bx;
        if (!(dy - 2 < 0 && dy + 0x0C >= 0)) continue; // 1000:5B89/0x5B91
        type = rd_le_u16(slot + 8u);
        if (object_core_type_blocked(type)) continue;
        (void)object_units_stack_scan_dissolve(i); // 1000:F453, AX = 5
        _obj_slot_class[(uint32_t)((r - _obj_array) / OBJ_STRIDE) - 8u] = 0u;
        r[6] = 0xFFu;
        cls = slot[7];
        if (cls) {
            _obj_core_class_byte_set(cls, 0u); // 0x3E5F + cls - 1
            if (cls >= 1u && OBJ_LIVE0 + cls <= _obj_live_count)
                _obj_array[(OBJ_LIVE0 + cls) * OBJ_STRIDE + 6] = 0xFFu;
        }
        wr_le_u16(slot + 0u, (uint16_t)(rd_le_u16(slot + 0u) - 8u));
        wr_le_u16(slot + 4u, 0x0028u);
        wr_le_u16(slot + 8u, 0x0016u);
        wr_le_u16(slot + 0xAu, 5u);
        _obj_core_driver_call(4);
        return;
    }
}


/** @brief 0xCDC7 / 0xCE38 / 0xCE3D: the frame/direction stepper (odd frames only).
 * It picks the frame, derives the pair-A/pair-B `dy` deltas through 0xCE88 and
 * then runs the move scan 0xE039 with the frame's `0x6612` group.
 *
 * @param r the record
 * @param mode the mode
 */
static void _obj_walk_mover_stepper(uint8_t *r, uint32_t mode)
{
    uint16_t oldFrame, newFrame;
    int16_t selC, selB;
    const uint8_t *tab;

    if (!(_obj_frame & 1u)) return;
    oldFrame = rd_le_u16(r + 4u);
    newFrame = oldFrame;
    if (mode != 0u) { // 0xCE38 / 0xCE3D -> 0xCE40
        int16_t b = (int16_t)((mode == 1u ? 1 : -1) + oldFrame);

        if (b >= 0x68) b = 0x58;
        if (b <= 0x57) b = 0x67;
        newFrame = (uint16_t)b;
        wr_le_u16(r + 4u, newFrame);
        { // 0xCE63: the +0xC step toward zero
            int32_t c = (int16_t)rd_le_u16(r + 0xCu);

            if (c != 0) c += (c < 0) ? 1 : -1;
            wr_le_u16(r + 0xCu, (uint16_t)c);
        }
    } else { // 0xCDC7: the 8x8 direction table
        int32_t bx = (int16_t)rd_le_u16(r + 0xCu),
                cx = (int16_t)rd_le_u16(r + 0xEu);

        if (bx == 0) {
            if (cx != 0) newFrame = (uint16_t)(cx > 0 ? 0x60 : 0x58);
        } else if (cx == 0) {
            newFrame = (uint16_t)(bx > 0 ? 0x5C : 0x64);
        } else { // 1000:CDF4: idx = 8*f([+0xE]) + f([+0xC])
            uint32_t idx =
                (uint32_t)(((cx + 0x10) >> 2) * 8 + ((bx + 0x10) >> 2));

            if (idx < sizeof _obj_mover_dir) newFrame = _obj_mover_dir[idx];
        }
        wr_le_u16(r + 4u, newFrame);
    }
    _obj_walk_selectors(oldFrame, newFrame, &selC, &selB); // 0xCE88
    if (newFrame < 0x58u || newFrame > 0x67u) return; // D-101
    tab = _obj_mover_tab + 4u * (newFrame - 0x58u);
    _obj_walk_move_scan(r, tab, selC, selB); // 0xCE34 / 0xCE84
}


/**
 * @brief 1000:CD71: the behaviour-5 handler.
 *
 * @param r the record
 */
static void _obj_walk_mover(uint8_t *r)
{
    uint32_t f, mode;

    if (!(rd_le_u16(r + 8u) & 0x8000u)) return;
    mode = rd_le_u16(r + 0xAu);
    if (mode <= 2u) _obj_walk_mover_stepper(r, mode);
    f = rd_le_u16(r + 4u);
    if (f < 0x58u || f > 0x67u) return;
    {
        uint32_t ix = (f - 0x58u) * 4u;
        int32_t dx = (int8_t)_obj_mover_tab[ix];
        int32_t dy = (int8_t)_obj_mover_tab[ix + 1u];
        int32_t ax = (int16_t)rd_le_u16(r + 0u) + dx;
        int32_t bx = (int16_t)rd_le_u16(r + 2u) + dy;

        _obj_walk_mover_pass_entity(r, ax, bx);
        _obj_walk_mover_pass_records(r, ax, bx);
        _obj_walk_mover_pass_placed(r, ax, bx);
    }
}


/** @brief 0xC6AD (behaviour 8, states 0/1): toggle the direction.
 *
 * The record's +0xA selects one of seven states through 0x654E.  States 0/1
 * walk (vx +-1) with a 0xC62C terrain/0x1FD6 probe and stop on a blocked
 * probe or item word, restoring the frame 0xE0/0xD4; 2/3 play the 0xC615 animation (frames 0x116..0x11A); after
 * either, 0xC6B8 checks the eight fixed slots (frame 0xDC/0xE8, +8 = 2,
 * +0xA = 4/5); 4/5 land through 0xC71E; 6 is the exit (frame 0x2E ends it).
 *
 * @param r the record
 * @param frame the frame index
 */
static void _obj_walk_stack8_fail(uint8_t *r, uint32_t frame)
{
    r[0xAu] ^= 1u;
    wr_le_u16(r + 4u, (uint16_t)frame);
}


/**
 * @brief The stack-8 slot pass.
 *
 * @param r the record
 */
static void _obj_walk_stack8_slots(uint8_t *r)
{
    uint8_t i;

    if (r[7] == 0u) return;
    for (i = 0; i < OBJ_STATIC; i++) {
        uint8_t *slot = _obj_array + i * OBJ_STRIDE;
        int32_t dx = (int32_t)(int16_t)rd_le_u16(r + 0u) -
                     (int32_t)(int16_t)rd_le_u16(slot + 0u);
        int32_t dy;

        if (dx >= 0) {
            if (dx > 0x40) continue;
            dy = (int32_t)(int16_t)rd_le_u16(r + 2u) -
                 (int32_t)(int16_t)rd_le_u16(slot + 2u);
            if (dy < 0) dy = -dy;
            dy -= 8;
            if (dy >= 0) continue;
            wr_le_u16(r + 4u, 0x00E8u);
            wr_le_u16(r + 8u, 2u);
            wr_le_u16(r + 0xAu, 5u);
            return;
        }
        if (-dx > 0x40) continue;
        dy = (int32_t)(int16_t)rd_le_u16(r + 2u) -
             (int32_t)(int16_t)rd_le_u16(slot + 2u);
        if (dy < 0) dy = -dy;
        dy -= 8;
        if (dy >= 0) continue;
        wr_le_u16(r + 4u, 0x00DCu);
        wr_le_u16(r + 8u, 2u);
        wr_le_u16(r + 0xAu, 4u);
        return;
    }
}


/**
 * @brief The stack-8 mover.
 *
 * @param r the record
 * @param vx the x velocity
 * @param vy the y velocity
 * @param stopFrame the stop frame
 * @param endFrame the end frame
 */
static void _obj_walk_stack8_move(uint8_t *r, int32_t vx, int32_t vy,
                                  uint32_t stopFrame, uint32_t endFrame)
{
    const uint8_t *map = level_map();
    int32_t land, bx, ax, recY;
    uint32_t tile, word, rowFine;

    {
        uint32_t probeCell;

        /* 1000:C62C: AX = [0x4234] + 8, BX = 0x18, and 1000:E389's 0xE361
         * adds the record's own x/y - so the probe is at
         * (x + vy + 8, y + 0x18), not at the raw offsets. */
        land = _obj_core_step_probe_at(r, vy + 8, 0x18, &probeCell);
        (void)probeCell;
    }
    if (land < 0) {
        _obj_walk_stack8_fail(r, stopFrame);
        return;
    }
    bx = land - 0x18;
    ax = (int32_t)(int16_t)rd_le_u16(r + 0u) + vx;
    rowFine = (uint32_t)(bx + 0xC) & 0xFu;
    {
        /* 1000:C645..0xC662: DI = [0x1FA0] + 2*((bx+0xC) & 0xFFF0) + (ax>>4),
         * all 16-bit, with no range test. */
        uint32_t ci = (uint16_t)(2u * ((uint32_t)(bx + 0xC) & 0xFFF0u)) +
                      (((uint32_t)ax & 0xFFFFu) >> 4);

        tile = map ? _obj_core_map_buffer_byte((int32_t)ci) : 0xFFu;
    }
    word = level_item_word(tile, rowFine);
    if (word != 0u) {
        _obj_walk_stack8_fail(r, stopFrame);
        return;
    }
    recY = (int32_t)(int16_t)rd_le_u16(r + 2u) - bx;
    if (recY < 0 && bx + 8 < 0) {
        _obj_walk_stack8_fail(r, stopFrame);
        return;
    }
    wr_le_u16(r + 0u, (uint16_t)(rd_le_u16(r + 0u) + (uint16_t)vy));
    wr_le_u16(r + 2u, (uint16_t)bx);
    {
        uint16_t s = rd_le_u16(r + 8u);

        if (s == 0u) {
            uint16_t f;

            wr_le_u16(r + 8u, 3u);
            f = (uint16_t)((rd_le_u16(r + 4u) + 1u));
            if (f == endFrame) f -= 8u;
            wr_le_u16(r + 4u, f);
        } else {
            wr_le_u16(r + 8u, (uint16_t)(s - 1u));
        }
    }
}


/**
 * @brief The stack-8 animation tail.
 *
 * @param r the record
 * @param endFrame the end frame
 */
static void _obj_walk_stack8_anim(uint8_t *r, uint32_t endFrame)
{
    uint16_t s = rd_le_u16(r + 8u);

    if (s != 0u) {
        wr_le_u16(r + 8u, s - 1u);
        return;
    }
    wr_le_u16(r + 8u, 6u);
    {
        uint16_t f = (uint16_t)(rd_le_u16(r + 4u) + 1u);

        wr_le_u16(r + 4u, f);
        if (f == endFrame) wr_le_u16(r + 4u, (uint16_t)(f - 4u));
    }
}


/**
 * @brief The stack-8 landing.
 *
 * @param r the record
 * @param bx the frame value
 * @param ax the x delta
 * @param bx2 the y delta
 * @param idx the index
 */
static void _obj_walk_stack8_land(uint8_t *r, uint32_t bx, int32_t ax,
                                  int32_t bx2, uint32_t idx)
{
    uint16_t s = rd_le_u16(r + 8u);

    s = (uint16_t)(s - 1u);
    wr_le_u16(r + 8u, s);
    if ((int16_t)s >= 0) return;
    wr_le_u16(r + 8u, 2u);
    wr_le_u16(r + 4u, (uint16_t)(rd_le_u16(r + 4u) + 1u));
    if (bx == rd_le_u16(r + 4u)) {
        uint8_t cls = r[7];

        r[7] = 0u;
        if (cls)
            _obj_core_class_byte_set(cls, _obj_core_class_byte_at(cls) & 0x3Fu);
        if (cls >= 1u && OBJ_LIVE0 + cls <= _obj_live_count) {
            uint8_t *t = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE;

            wr_le_u16(t + 8u, 0xFFFFu);
            wr_le_u16(t + 0xAu, 0u);
            wr_le_u16(t + 0xCu, (uint16_t)ax);
            wr_le_u16(t + 0xEu, (uint16_t)bx2);
        }
        _obj_core_driver_call(3);
    }
    if (bx + 3u == rd_le_u16(r + 4u)) {
        wr_le_u16(r + 4u, (uint16_t)(rd_le_u16(r + 4u) - 0xCu));
        wr_le_u16(r + 0xAu, (uint16_t)(rd_le_u16(r + 0xAu) - 4u));
    }
    (void)idx;
}


/**
 * @brief The stack walk tail.
 *
 * @param r the record
 * @param idx the index
 */
static void _obj_walk_stack(uint8_t *r, uint32_t idx)
{
    switch (rd_le_u16(r + 0xAu)) {
    case 0u:
        _obj_walk_stack8_move(r, 0x11, 1, 0xE0u, 0xDCu);
        _obj_walk_stack8_slots(r);
        break;
    case 1u:
        _obj_walk_stack8_move(r, -1, -1, 0xD4u, 0xE8u);
        _obj_walk_stack8_slots(r);
        break;
    case 2u:
        _obj_walk_stack8_anim(r, 0x11Au);
        _obj_walk_stack8_slots(r);
        break;
    case 3u:
        _obj_walk_stack8_anim(r, 0x11Eu);
        _obj_walk_stack8_slots(r);
        break;
    case 4u: _obj_walk_stack8_land(r, 0xDDu, 0xA, -3, idx); break;
    case 5u: _obj_walk_stack8_land(r, 0xE9u, -0xA, -3, idx); break;
    default: { // state 6, the exit
        uint16_t s = rd_le_u16(r + 8u);

        s = (uint16_t)(s - 1u);
        wr_le_u16(r + 8u, s);
        if ((int16_t)s >= 0) break;
        wr_le_u16(r + 8u, 3u);
        {
            uint16_t f = (uint16_t)(rd_le_u16(r + 4u) + 1u);

            wr_le_u16(r + 4u, f);
            if (f == 0x2Eu) {
                _obj_slot_class[idx - 8u] = 0u;
                r[6] = 0xFFu;
            }
        }
        break;
    }
    }
}


/**
 * @brief Cull the projectile and advance its x, as `1000:D1E3` does.
 *
 * The original writes `BX` (the record x): it adds `+/- (|vx| >> 3)` and puts
 * the value back only when the cull fails.  On shipped data that is the wheel's
 * only horizontal motion: the path handlers wind `+8` and this routine turns
 * it into x steps.
 *
 * @param vx the x velocity
 * @param x the x coordinate (in/out)
 * @return the velocity, or 0 when culled
 */
static int32_t _obj_walk_proj_cull(int32_t vx, int16_t *x)
{ // 1000:D1E3 cull_entity_offscreen
    int32_t off;

    if (vx < 0) {
        off = (-vx) >> 3;
        *x = (int16_t)(*x - off);
        if (*x >= 0) return vx;
        *x = (int16_t)(*x + off);
        return 0;
    }
    off = vx >> 3;
    *x = (int16_t)(*x + off);
    if ((uint16_t)*x < 0x1F0u) return vx;
    *x = (int16_t)(*x - off);
    return 0;
}


/**
 * @brief 1000:D584: off the page; from 0x160 the target takes the crash state.
 *
 * @param r the record
 * @return -1 when off-page, -2 when the target crashes
 */
static int32_t _obj_walk_proj_offpage(uint8_t *r)
{
    if ((uint16_t)_obj_scan_y >= 0x160u) {
        int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

        *_obj_act_class = 0u;
        r[6] = 0xFFu;
        if (tgt >= 0) {
            uint8_t *t = _obj_array + (uint32_t)tgt * OBJ_STRIDE;

            wr_le_u16(t + 4u, 0x0028u);
            wr_le_u16(t + 6u, 0u);
            wr_le_u16(t + 8u, 0x0016u);
            wr_le_u16(t + 0xAu, 5u);
            _obj_core_driver_call(4);
        }
        _obj_scan_y = -1;
        return -2; // 0xD5D2: return one level up
    }
    _obj_scan_y = -1;
    return -1;
}


/**
 * @brief 1000:D41B advance_position_snap_row: advance the scratch position and
 * snap it to the row.
 *
 * BX/CX are deltas added to [0x4238]/[0x423C].  The water arm (0xD4D4) and the
 * off-page arm (0xD5D2) end `POP DI; POP BP; ADD SP,2; RET`: the `ADD SP,2`
 * skips 0xD41B's return address, so RET returns one level up and the rest of
 * the immediate caller is abandoned.  -2 signals that early-out.
 *
 * @param r the record
 * @param bx the x delta
 * @param cx the y delta
 * @return the snapped y, -1 when off-page, -2 for the early return
 */
static int32_t _obj_walk_proj_advance(uint8_t *r, int16_t bx, int16_t cx)
{
    uint32_t idx, sub;
    int32_t page;
    uint8_t tile;

    _obj_scan_y = (int16_t)(0x10 + cx);
    _obj_scan_x = (int16_t)(_obj_scan_x + bx);
    if ((uint16_t)_obj_scan_y >= 0x140u) // 1000:D42E
        return _obj_walk_proj_offpage(r);
    _obj_scan_y &= (int16_t)0xFFF0;
    sub = (uint16_t)_obj_scan_x & 0xFu;
    idx = 2u * (uint16_t)_obj_scan_y + ((uint16_t)_obj_scan_x >> 4);
    tile = (uint8_t)_obj_core_map_buffer_byte((int32_t)(idx - 0x20u));
    if (tile >= 0xF0u && tile <= 0xF2u) { // 1000:D46F: the water branch
        /* 0xD4D4: POP DI / POP BP / ADD SP,2 / RET - the early-out above. */
        *_obj_act_class = 0u;
        r[6] = 0xFFu;
        {
            int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

            if (tgt >= 0) {
                uint8_t *t = _obj_array + (uint32_t)tgt * OBJ_STRIDE;

                wr_le_u16(t + 0u, (uint16_t)(rd_le_u16(t + 0u) + 8u));
                wr_le_u16(t + 2u, (uint16_t)(rd_le_u16(t + 2u) + 8u));
                wr_le_u16(t + 4u, 0x00CDu);
                wr_le_u16(t + 6u, 0u);
                wr_le_u16(t + 8u, 0x001Cu);
                wr_le_u16(t + 0xAu, 9u);
                wr_le_u16(t + 0xCu, 0u);
            }
        }
        _obj_core_driver_call(6);
        return -2; // 0xD4D4: return one level up
    }
    page = (int32_t)(int8_t)(uint8_t)level_page_byte(
        _obj_core_map_buffer_byte((int32_t)idx), sub); // 1000:D4DA
    if (page < 0) { // 1000:D50F
        if ((uint16_t)_obj_scan_y >= 0x130u) return _obj_walk_proj_offpage(r);
        page = (int32_t)(int8_t)(uint8_t)level_page_byte(
            _obj_core_map_buffer_byte((int32_t)(idx + 0x20u)), sub);
        if (page < 0) { // 1000:D535
            _obj_scan_y = -1;
            return -1;
        }
        _obj_scan_y = (int16_t)(_obj_scan_y + page); // 1000:D4FC: no row step
        return _obj_scan_y;
    }
    if (page == 0) { // 1000:D540: the row above decides
        uint8_t above =
            (uint8_t)(_obj_core_map_buffer_byte((int32_t)(idx - 0x20u)));

        page = (int32_t)(int8_t)(uint8_t)level_page_byte(above, sub);
        if (page < 0) { // 1000:D565: re-read the cell, then one row up
            page = (int32_t)(int8_t)(uint8_t)level_page_byte(
                _obj_core_map_buffer_byte((int32_t)idx), sub);
            _obj_scan_y = (int16_t)(_obj_scan_y - 0x10 + page);
            return _obj_scan_y;
        }
        _obj_scan_y = (int16_t)(_obj_scan_y - 0x10); // 1000:D55E
    }
    _obj_scan_y = (int16_t)(_obj_scan_y - 0x10 + page); // 1000:D4F7
    return _obj_scan_y;
}


/**
 * @brief 1000:D3E3 bounce_value_4230: reflect the velocity and lose 0x20.
 *
 * Each arm keeps the reflected/stepped value only while it is still on the
 * reflected side of zero; a bounce that does not cross the sign is zeroed
 * (`1000:D3EE` `JS` / `1000:D3FD` `JNS` skip the `MOV [0x4230],0`).
 *
 * @param vx the x velocity
 * @param vy the y velocity
 */
static void _obj_walk_proj_bounce(int16_t *vx, int16_t *vy)
{
    int16_t v = (int16_t)(-*vx);

    if (v >= 0) {
        v = (int16_t)(v - 0x20);
        if (v < 0) v = 0;
    } else {
        v = (int16_t)(v + 0x20);
        if (v >= 0) v = 0;
    }
    *vx = v;
    if (*vy != 0 && (uint16_t)*vy < 0x10u) *vy = 0x10;
}


/** @brief 1000:D34E: the projectile's cell/item probe and terrain bounce.
 * Despite its name it draws nothing.
 *
 * @param r the record
 * @param bx the x delta
 * @param cx the y delta
 * @param vx the x velocity (in/out)
 * @param vy the y velocity (in/out)
 */
static void _obj_walk_proj_burst(uint8_t *r, int16_t bx, int16_t cx,
                                 int16_t *vx, int16_t *vy)
{
    _obj_scan_x = (int16_t)(_obj_scan_x + bx);
    _obj_scan_y = (int16_t)(_obj_scan_y + cx);
    {
        int16_t sx = _obj_scan_x, sy = _obj_scan_y;
        uint32_t si = 2u * ((uint16_t)sy & 0xFFF0u) + ((uint16_t)sx >> 4);
        uint16_t sub = (uint16_t)sx & 0xFu;
        uint32_t word = level_item_word(_obj_core_map_buffer_byte((int32_t)si),
                                        (uint16_t)sy & 0xFu);
        uint16_t hi = (*vx < 0) ? 0xFFFFu : 0u;
        uint16_t lo = hi ? 0u : 0xFFFFu, k;

        for (k = 0; k < sub; k++) {
            uint8_t carry = hi & 1u;

            hi >>= 1;
            lo = (uint16_t)((lo >> 1) | (carry << 15));
        }
        if ((word & lo) != 0u) { // 1000:D3B3
            _obj_walk_proj_bounce(vx, vy);
            return;
        }
    }
    _obj_scan_x = (*vx < 0) ? 0 : 0x10; // 1000:D3B9
    if (_obj_walk_proj_advance(r, bx, cx) < 0) return;
    {
        int32_t d = (int16_t)(cx - _obj_scan_y); // 1000:D3D2

        if (d < 0) return;
        if (d - 0xC < 0) return;
    }
    _obj_walk_proj_bounce(vx, vy);
}


/** @brief 1000:D31A shift_bit_into_delayed_byte: returns the caller's ZF (1 = the
 * counter path ran).
 *
 * @param r the record
 * @param s38 the frame scratch (in/out)
 * @param s3c the previous frame (in/out)
 * @return 1 when the delayed byte should advance, 0 otherwise
 */
static int32_t _obj_walk_hit_delay(uint8_t *r, uint16_t *s38, uint16_t *s3c)
{
    *s3c = (uint16_t)(*s3c - 1u);
    if ((int16_t)*s3c < 0) return 0;
    *s38 = (uint16_t)(*s38 >> 2);
    *s3c = (uint16_t)(*s3c << 3);
    *s38 = (uint16_t)(*s38 | (uint8_t)*s3c);
    r[7] = (uint8_t)*s38;
    *s38 = 0u;
    return 1;
}


/** @brief The five ops of `202F:0x666B` (1000:D228/D2AE/D2C0/D2DA/D2F4): the hit
 * record's frame/state animation.  `r` is the projectile (its +7 is the state
 * byte), `t` the hit record.
 *
 * @param r the record
 * @param t the target record
 * @param frame the frame index
 * @param vx the x velocity
 * @param vy the y velocity
 */
static void _obj_walk_hit_dispatch(uint8_t *r, uint8_t *t, int16_t frame,
                                   int16_t vx, int16_t vy)
{
    uint16_t s38 = (uint16_t)((r[7] & 7u) * 4u); // [0x4238]
    uint16_t s3c = (uint16_t)(r[7] >> 3); // [0x423C]
    uint8_t op = (uint8_t)(r[7] & 7u);

    if (op == 0u) { // 1000:D228 hit_op_frame_default
        s38 = 0x41u;
        if (vy == 0) s38 = (uint16_t)(frame + 4);
    } else if (op == 1u) { // 1000:D2AE hit_op_bit_test_42
        if (!_obj_walk_hit_delay(r, &s38, &s3c)) {
            r[7] = 0u;
            return;
        }
        s38 = 0x42u;
    } else if (op == 2u) { // 1000:D2C0 hit_op_bit_test_4d
        if (_obj_walk_hit_delay(r, &s38, &s3c)) return;
        wr_le_u16(t + 4u, (uint16_t)(rd_le_u16(t + 4u) + 1u));
        if (rd_le_u16(t + 4u) == 0x4Du) {
            r[7] = 0u;
            wr_le_u16(t + 4u, 0x43u);
            return;
        }
        r[7] = 0x3Au;
        return;
    } else if (op == 3u) { // 1000:D2DA hit_op_bit_test_50
        if (_obj_walk_hit_delay(r, &s38, &s3c)) return;
        wr_le_u16(t + 4u, (uint16_t)(rd_le_u16(t + 4u) + 1u));
        if (rd_le_u16(t + 4u) == 0x50u) {
            r[7] = 0u;
            wr_le_u16(t + 4u, 0x3Cu);
            return;
        }
        r[7] = 0x3Bu;
        return;
    } else if (op == 4u) { // 1000:D2F4 hit_op_terminate_50
        if (vx == 0) {
            r[7] = 0u;
            wr_le_u16(t + 4u, 0x50u);
            return;
        }
        if (_obj_walk_hit_delay(r, &s38, &s3c)) return;
        r[7] = 0x1Cu;
        wr_le_u16(t + 4u, (uint16_t)(rd_le_u16(t + 4u) + 1u));
        if (rd_le_u16(t + 4u) == 0x50u) wr_le_u16(t + 4u, 0x4Au);
        return;
    } else {
        return; // D-101: past the five-word table
    }
    /* 1000:D23F: the common tail */
    if (vx == 0) { // 1000:D294
        if (rd_le_u16(t + 4u) >= 0x50u) return;
        wr_le_u16(t + 4u,
                  (uint16_t)(rd_le_u16(t + 4u) <= 0x42u ? 0x50u : 0x51u));
        return;
    }
    if (vx < 0) s38 = (uint16_t)(s38 + 7u);
    s3c = rd_le_u16(t + 4u);
    wr_le_u16(t + 4u, s38);
    s38 = (uint16_t)(s38 - 0x3Cu);
    s3c = (uint16_t)(s3c - 0x3Cu);
    if ((int16_t)s3c < 0) return; // 1000:D261
    if (s38 >= 0x16u || s3c >= 0x16u) return; // D-101: the 0x667F table
    {
        uint8_t a = _obj_hit_frame[s38], b = _obj_hit_frame[s3c];

        if (a == b) return; // 1000:D278
        if (a != 0u) {
            r[7] = 0x3Au;
            wr_le_u16(t + 4u, 0x4Au);
        } else {
            r[7] = 0x3Bu;
            wr_le_u16(t + 4u, 0x4Du);
        }
    }
}


/** @brief 1000:D162: position the projectile's target and run the hit op.
 *
 * Both behaviour-4 phases fall through to this shared tail: the target record
 * (the wheel's rider, while mounted) is placed at `(x-8, y-0x10)` and gets the
 * `0x666B` frame/state op.  Phase 1 calls it after the burst, which is what
 * gives the mounted human its riding frames.
 *
 * @param r the projectile record
 * @param x the projectile x
 * @param y the projectile y
 * @param frame the projectile frame
 * @param vx the x velocity
 * @param vy the y velocity
 */
static void _obj_walk_proj_target(uint8_t *r, int32_t x, int32_t y,
                                  int32_t frame, int32_t vx, int32_t vy)
{
    int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

    if (tgt < 0) return; // 1000:D16A
    {
        uint8_t *t = _obj_array + (uint32_t)tgt * OBJ_STRIDE;

        wr_le_u16(t + 0u, (uint16_t)(x > 8 ? x - 8 : 0)); // 1000:D17A
        wr_le_u16(t + 2u, (uint16_t)(y - 0x10)); // 1000:D185
        _obj_walk_hit_dispatch(r, t, (int16_t)(frame), (int16_t)(vx),
                               (int16_t)(vy)); // 1000:D1BD
    }
}


/**
 * @brief 0xCEC3 entity_update_from_map: the behaviour-4 projectile.
 *
 * Phase 1 (vy == 0) rolls along the terrain: the tile at (x+8, y+0x10) gives a
 * signed slope ([0x1FD4]) added to vx with a +-1 step ([0x6695]); the fuse
 * +0xC is recomputed as 8 - (|vx|>>2) when it expires and the frame cycles
 * 0x38..0x3B.  Phase 2 (vy != 0) is the fall: the signed curve at 0x6652
 * (indexed by vy, clamped at 0x19) is added to y, and the landing test resolves
 * the target through +0xE, writing its 0x28/0/0x16/5 state and dispatching the
 * 0x666B op.  Both phases end at _obj_walk_proj_target (1000:D162).
 *
 * @param r the record
 * @param idx the index
 */
static void _obj_walk_projectile(uint8_t *r, uint32_t idx)
{
    int16_t x = (int16_t)rd_le_u16(r + 0u);
    int16_t y = (int16_t)rd_le_u16(r + 2u);
    int16_t frame = (int16_t)rd_le_u16(r + 4u);
    int16_t vx, vy, cxreg;

    (void)idx;
    if (r[6] & 0x40u) return; // 1000:CED6
    vx = (int16_t)rd_le_u16(r + 8u);
    vy = (int16_t)rd_le_u16(r + 0xAu);
    if (vy == 0) { // phase 1
        int16_t sx = (int16_t)(x + 8);
        int16_t sy = (int16_t)((y + 0x10) & ~0xF);
        uint16_t ci = 2u * (uint16_t)sy + ((uint16_t)sx >> 4);
        int16_t slope = (int16_t)level_slope_byte(
            _obj_core_map_buffer_byte((int32_t)ci), (uint16_t)sx & 0xFu);
        int16_t step = 1;

        vx = (int16_t)(vx + slope); // 1000:CF64
        if (vx != 0) {
            if (vx >= 0) step = -1; // 1000:CF6C
            vx = (int16_t)(vx + step);
            if (vx != 0) {
                int16_t fuse = (int16_t)(rd_le_u16(r + 0xCu) - 1u);

                wr_le_u16(r + 0xCu, (uint16_t)fuse); // 1000:CF79
                if (fuse < 0) {
                    int16_t sign = (vx < 0) ? -1 : 1;
                    int16_t a = (int16_t)(vx < 0 ? -vx : vx);

                    a = (int16_t)(-(a >> 2) + 8);
                    wr_le_u16(r + 0xCu, (uint16_t)a);
                    frame = (int16_t)(frame + sign); // 1000:CFAD
                    if ((uint16_t)frame <= 0x37u) frame = 0x3B;
                    else if ((uint16_t)frame >= 0x3Cu) frame = 0x38;
                }
            }
        }
        vx = (int16_t)_obj_walk_proj_cull(vx, &x); // 1000:CFC3
        _obj_scan_x = 8;
        {
            int32_t rc = _obj_walk_proj_advance(r, x, y);

            if (rc == -2) return; // 0xD4D4/0xD5D2: leave 1000:CEC3
            if (rc < 0) { // 1000:D030: fall into phase 2
                vy = 0x10;
                wr_le_u16(r + 0x10u, (uint16_t)y);
            } else {
                cxreg = _obj_scan_y; // 1000:CFD1
                _obj_scan_x = 0;
                _obj_scan_y = 0xC;
                if (vx >= 0) _obj_scan_x = 0x10;
                _obj_walk_proj_burst(r, x, cxreg, &vx, &vy); // 1000:CFEF
                { // 1000:CFF2
                    int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

                    if (tgt >= 0) {
                        _obj_foot_x = (uint16_t)(x + 8);
                        _obj_foot_y = (uint16_t)(cxreg + 0x10);
                        if (_obj_spawn_bg_mark(r) == 0) {
                            _obj_settled[tgt][0] = (uint16_t)x;
                            _obj_settled[tgt][1] = (uint16_t)(cxreg - 8);
                        }
                    }
                }
                /* 1000:D02D -> D162: phase 1 shares the target write and the
                 * hit op with phase 2, which is what keeps the wheel's rider
                 * in its riding frames. */
                _obj_walk_proj_target(r, x, cxreg, frame, vx, vy);
                /* 1000:D1C0: the phase-1 writeback ends the tick */
                wr_le_u16(r + 0xAu, (uint16_t)vy);
                wr_le_u16(r + 8u, (uint16_t)vx);
                wr_le_u16(r + 4u, (uint16_t)frame);
                wr_le_u16(r + 2u, (uint16_t)cxreg);
                wr_le_u16(r + 0u, (uint16_t)x);
                return;
            }
        }
    }
    /* phase 2 */
    vx = (int16_t)_obj_walk_proj_cull(vx, &x); // 1000:D039
    _obj_scan_x = 4;
    if ((uint8_t)vy < 0x19u)
        _obj_scan_x = _obj_proj_curve[(uint8_t)vy]; // 1000:D049
    {
        int16_t curve = (int16_t)(int8_t)(uint8_t)_obj_scan_x;

        y = (int16_t)(y + curve); // 1000:D065
        cxreg = y;
        if (curve >= 0) { // 1000:D069
            /* D135 = fall, D11B = bounce/land, D0B5 = target hit. */
            uint8_t mode = 0;

            _obj_scan_x = 8;
            {
                int32_t rc = _obj_walk_proj_advance(r, x, y);

                if (rc == -2) return; // 0xD4D4/0xD5D2: leave 1000:CEC3
                if (rc >= 0 && _obj_scan_y <= y) {
                    int16_t prev = (int16_t)rd_le_u16(r + 0x10u);
                    int32_t d = (int32_t)prev - (int32_t)_obj_scan_y;

                    /* 1000:D08B..D0A5: prev < 0, prev-scan_y < 0 and
                     * prev-scan_y <= 0x10 all take D0A8; only a distance
                     * greater than 0x10 stays on the fall arm (D135). */
                    if (prev < 0 || d < 0 || d <= 0x10) {
                        d = -d; // 1000:D0A8 NEG
                        /* The original tests the 16-bit NEG result's sign
                         * (`JS D11B`), so 0x8000 stays the bounce arm. */
                        if ((int16_t)d < 0 || d < 0x40) {
                            vy = -1; // 1000:D11B
                            _obj_scan_x = 5;
                            mode = 1;
                        } else {
                            int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

                            if (tgt < 0) {
                                vy = -1; // 1000:D0BD -> D11B
                                _obj_scan_x = 5;
                                mode = 1;
                            } else { // 1000:D0B5: the hit
                                uint8_t *t =
                                    _obj_array + (uint32_t)tgt * OBJ_STRIDE;

                                wr_le_u16(r + 0xEu, 0xFFFFu);
                                *_obj_act_class &= 0x7Fu;
                                r[6] &= 0x7Fu;
                                wr_le_u16(t + 0u,
                                          (uint16_t)(x > 8 ? x - 8 : 0));
                                wr_le_u16(t + 2u, (uint16_t)(_obj_scan_y - 8));
                                wr_le_u16(t + 4u, 0x0028u);
                                wr_le_u16(t + 6u, 0u);
                                wr_le_u16(t + 8u, 0x0016u);
                                wr_le_u16(t + 0xAu, 5u);
                                vy = 0;
                                wr_le_u16(r + 0x10u, (uint16_t)y);
                                _obj_scan_x = 4;
                                mode = 2;
                            }
                        }
                    }
                }
            }
            if (mode != 0) { // 1000:D127
                cxreg = _obj_scan_y;
                r[7] = 0x99u;
                _obj_core_driver_call((uint32_t)(uint16_t)_obj_scan_x);
            }
            /* 1000:D135 */
            vy = (int16_t)(vy + 1);
            _obj_scan_y = 0x10;
        } else { // 1000:D141
            vy = (int16_t)(vy + 1);
            _obj_scan_y = 0xC;
        }
    }
    {
        _obj_scan_x = (vx < 0) ? 0 : 8; // 1000:D14B
        _obj_walk_proj_burst(r, x, cxreg, &vx, &vy); // 1000:D15F
    }
    _obj_walk_proj_target(r, x, cxreg, frame, vx, vy); // 1000:D162
    /* 1000:D1C0: the writeback */
    wr_le_u16(r + 0xAu, (uint16_t)vy);
    wr_le_u16(r + 8u, (uint16_t)vx);
    wr_le_u16(r + 4u, (uint16_t)frame);
    wr_le_u16(r + 2u, (uint16_t)cxreg);
    wr_le_u16(r + 0u, (uint16_t)x);
}


/** @brief 0xC444 (behaviour 9): the marker use.  0xC541 first animates the map cell
 * the record points at (+8) every 4th frame; then the tile-aligned record
 * position is matched against the eight fixed slots, and the matched **slot**
 * (the human) takes x -= 8, frame 0x28, type 0x16, +0xA = 5 and effect 4 -
 * `1000:C4E9..C4F6` stores through SI, which is the scan's slot.
 *
 * The four stores target the matched `slot` (the human), not the marker, so
 * the marker's `+8` map-cell pointer keeps driving `1000:C541` and the human
 * takes the knock/consumed state.
 *
 * @param r the marker record
 */
static void _obj_walk_marker(uint8_t *r)
{
    const uint8_t *map = level_map();
    uint8_t i;

    if ((_obj_frame & 3u) == 0u) { // 1000:C541
        uint16_t f = rd_le_u16(r + 4u);

        if (_obj_marker_mode) {
            if (f == 0x26u) {
                wr_le_u16(r + 4u, f + 1u); // 1000:C562
            } else {
                r[6] |= 0x80u; // 1000:C557
                if (map && rd_le_u16(r + 8u) < OBJ_MAP_CELLS)
                    ((uint8_t *)map)[rd_le_u16(r + 8u)] = 0u;
            }
        } else if (r[6] & 0x80u) {
            r[6] = 0u; // 1000:C579: cleared, so the re-test below scans
        } else if (f != 0x26u) {
            wr_le_u16(r + 4u, f - 1u); // 1000:C570
            if (map && rd_le_u16(r + 8u) < OBJ_MAP_CELLS)
                ((uint8_t *)map)[rd_le_u16(r + 8u)] = 0xBBu;
        }
    }
    /* 1000:C447: the helper 0xC541 may have set or cleared [r+6] bit 7, and
     * the caller re-tests it here - the scan follows whenever it is clear. */
    if (r[6] & 0x80u) return;
    {
        uint16_t mx = (uint16_t)(rd_le_u16(r + 0u) + 8u) & ~0xFu;
        uint16_t my = (uint16_t)(rd_le_u16(r + 2u) + 8u) & ~0xFu;

        for (i = 0; i < OBJ_STATIC; i++) {
            uint8_t *slot = _obj_array + i * OBJ_STRIDE;
            uint32_t sx, sy, type;

            sx = (rd_le_u16(slot + 0u) + 8u) & ~0xFu;
            if (rd_le_u16(slot + 8u) == 0x0Fu)
                sx = (rd_le_u16(slot + 0u) + 0x10u) & ~0xFu;
            sy = (rd_le_u16(slot + 2u) + 0x0Cu) & ~0xFu;
            if (sx != mx || sy != my) continue;
            if (slot[6] & 0x80u) continue;
            type = rd_le_u16(slot + 8u);
            if (object_core_type_blocked(type)) continue;
            object_units_stack_scan_dissolve(i); // 1000:F453, DX = 5
            wr_le_u16(slot + 0u, (uint16_t)(rd_le_u16(slot + 0u) - 8u));
            wr_le_u16(slot + 4u, 0x0028u);
            wr_le_u16(slot + 8u, 0x0016u);
            wr_le_u16(slot + 0xAu, 5u);
            _obj_core_driver_call(4); // 1000:83B7, AX = 4
            {
                uint16_t gx = (uint16_t)((_obj_settled[i][0] + 8u) & ~0xFu);
                uint16_t gy = (uint16_t)((_obj_settled[i][1] + 0x0Cu) & ~0xFu);

                if (gx == mx && gy == my) _obj_settled[i][0] += 0x10u;
            }
            return;
        }
    }
}


void object_walk_update_list(void)
{
    for (uint16_t i = OBJ_LIVE0 + 1u; i <= _obj_live_count; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint8_t beh = (uint8_t)(_obj_slot_class[i - 8u] & 0x3Fu);

        _obj_act_class = &_obj_slot_class[i - 8u]; // [0x4244] - 1
        switch (beh) {
        case 1u: _obj_walk_step1(r); break;
        case 2u: _obj_walk_wander(r); break;
        case 3u: _obj_walk_seq(r); break;
        case 5u: _obj_walk_mover(r); break;
        case 4u: _obj_walk_projectile(r, i); break;
        case 6u: _obj_walk_torch(r); break;
        case 7u: _obj_walk_rope(r); break;
        case 8u: _obj_walk_stack(r, i); break;
        case 9u: _obj_walk_marker(r); break;
        default: break;
        }
    }
}
