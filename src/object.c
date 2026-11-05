/* object.c — the placed-object array (0x3E75), its renderer, its fill, the
 * adjacent-object claim and the goal predicates (re_map.md §"`1000:3A6A`").
 *
 * The array is **8 records of 0x12 bytes at 0x3E75**, then the live records
 * the claim appends at `0x3F17 + 0x12*n` (`[0x3E73]` counts the used slots
 * from 9).  Fields, all from consumers:
 *
 *   +0x0  u16 x                  the claim's window is |dx| <= 0x20
 *   +0x2  u16 y                                and |dy| <= 8
 *   +0x4  u16 frame / sprite id  0x10 the placed human, 0x20 the used-up one,
 *                                0x103 the claim's, the spawn's recipe frame
 *   +0x6  u16 class<<8 | flags   low byte bit 7 = inactive (`TESTB $0x80,0x6`);
 *                                0xFFFF removes it; the high byte is the
 *                                0x3E5E source-slot index (`MOV 0x7(%si),%al`)
 *   +0x8  u16 type               0x1D/0x1E are the "not a human" markers
 *                                (`check_highscore`); < 3 gates the claim
 *   +0xA..+0x11                  the spawn's four parameter words, copied
 *                                from the recipe (1000:3B43..1000:3B4C -> 1000:3C49..1000:3C61)
 *
 * The placed records are built by `load_terrain_b`'s tile walk
 * (`object_load_terrain_objects`, all 25 tiles), their sprites come from the
 * storage law derived from `draw_object_scaled_x1` (`1000:66EC`: four plane
 * passes of 4 bytes per row, `block = 4h`, see `_blit_sprite`), and the claim
 * (`object_claim_begin`/`object_claim_tick`) runs the whole flash/blink/animate/
 * append/settle sequence.  `[0x1AF2]` is the claimed record, `[0x180A]` the
 * player's own (`resolve_object_sprite` `1000:4147` gives it the program index).
 *
 * The ACTION machine (`script_op_dispatch` 1000:3979, `object_action_run`) is
 * ported with ops 1, 2/6, 3, 4, 5, 8, 9 and 10/11/12, the collision pass with
 * both direction actions (the pickup writes the class — level 1's win), the
 * object selector and the pending-action arm; op 7 is dead code and halts.
 *
 * The human stack (`0x6F22`) is ported through the verbs the rows reach: push
 * (`1000:E697`), grab (`1000:E58B`), pop (`1000:FDED`), the two leaves
 * (`1000:E860`/`1000:E8DD`), the snap (`1000:E963`), the stand (`1000:E99D`),
 * the selector (`1000:F17C`) with its two wrappers, and the dissolve
 * (`1000:FA13`) with the `0xF453` scan that calls it.
 *
 * The two former gaps are ported: `1000:E7C0` (type 2 command 8, the timed
 * frame stream at 0x6DF2) and `1000:EBCA` (type 15 command 8, clear the +0xA
 * hold), and `1000:D6B2`'s animator: phases 1/2 (`_obj_pending_moves_tick`, the
 * 0x67FF pending-move table) and phase 3 (`object_animator_tick`, the 0x6F22
 * groups and the 0x66B3/0x66B5/_obj_anim_x tables).  The class/behaviour
 * overlays of `1000:653C` and all six `1000:35D6` goal-test handlers are
 * ported.  The bare-RET slot `1000:EA0E` is in the table (type 16 command 8).
 */
#include "object.h"
#include "entity.h"
#include "level.h"
#include "osystem.h"
#include "palette.h"
#include "statusbar.h"
#include "byteorder.h"

#include <string.h>

#define OBJ_STATIC 8u /* the records 0x3640/check_highscore scan */
/* 0x3E75..0x407F: the array's initialiser (`1000:4CAB`) clears 29 records. */
#define OBJ_SLOTS  29u
#define OBJ_STRIDE 0x12u
/* `load_terrain_b` (`1000:516F`) sets [0x3E73] to 8; its per-tile walk appends
 * one live record per placed object (`0x3F17 + 0x12*([0x3E73]-8)`), so the
 * count is 8 + the walk's appends (level 1: one 0xAB cell = the spear). */
#define OBJ_LIVE0 8u

/* --- the level's object walk (`load_terrain_b`, 1000:5168) -------------
 *
 * The level start clears the array (`1000:4CAB`), sets `[0x3E73] = 8` and the
 * 20 behaviour bytes at `0x3E5F` to 0 (`1000:516F`), then walks the map **once
 * per tile**, in the order of the 25-entry table `202F:0x1EB3` (`1000:5184`:
 * `CX = [0x1EB1]` = 25 pairs, `BX` = the tile bytes, `SI` = the handlers at
 * `0x1ECC`).  Every match is handed to its handler with
 * `CX = col*16`, `DX = row*16` (`1000:51BF`'s four shifts each).
 *
 * All 25 tile handlers are ported (each one's body is summarised below or at
 * its function); the two that explain the shipped levels' records are:
 *
 *   0xAA  `1000:524B`  the human: the **first free static slot** (the scan
 *                    tests `+6` bit 7), `x = col*16`, `y = row*16 - 7`,
 *                    `+4 = 0x10`, `+6 = 0`, `+8 = 2`, `[0x3E20]++`.
 *                    Live-confirmed 2026-10-30: level 1's records read
 *                    `(48,25) (208,25) (256,137) (464,265)` = `row*16 - 7`
 *                    for rows 2/2/9/17, exactly this store (re_map.md).
 *   0xAB  `1000:527D`  the spear: **appended** at the live end, `x = col*16`,
 *                    `y = row*16` (no bias), `+4 = 0x60`, `+6 = 0`, `+8 = 0xFFFF`,
 *                    `+0xA = 0`, the behaviour 5 into `[0x3E5F + n]`, `[0x3E73]++`.
 *                    Level 1's map has exactly one 0xAB cell, at (28,2) — which
 *                    is why the live `[0x3E73] = 9`, slot byte `[0x3E5F] = 5`
 *                    and record 9 hold `+4 = 0x60, +8 = 0xFFFF`.
 */
#define OBJ_COLS        32u
#define OBJ_ROWS        20u
#define OBJ_FRAME_SLOTS 0x11Eu /* apply_relocations: 0x39A8..0x3E20 */

/* 0x32B9: the 0xA9 background-sprite records (12 bytes each), filled by that
 * tile handler (1000:5206); the settle's 1000:DF2C reads them. */
static struct {
    uint16_t x, y;
    uint8_t *dst; /* +4: the first of the five A9 log records   */
    uint8_t *mark; /* +8: the map cell pair 1000:D5E4 rewrites     */
} _obj_bg[32];

/* --- the claim: the entity bytecode's adjacent-object op (1000:3A6A) ---
 *
 * The op runs with SI = the **acting entity's** record (`0x8DF4`: the current
 * entity index `[0x3E22]` through the pointer table `[0x3E24]`), and is
 * dispatched from the jump table at `CS:0x399C`; the three handlers at
 * 1000:3A5A/1000:3A5F/1000:3A64 pass AX = 1, 0 and 3, the recipe index.  The claimed
 * record `[0x1AF2]` is only ever *written* here.
 *
 * The gate and the scan (1000:3A6D..1000:3AD9): the actor's type must be <= 2 and the
 * recipe (`202F:0x19BA + 12*AX`, `[recipe+0]` = the remaining uses) must
 * still have a use; then the other eight records are scanned in array order
 * for `|dx| <= 0x20`, `|dy| <= 8`, `type <= 2`, `+6` bit 7 clear.  The first
 * hit is the neighbour.
 *
 * The writes (1000:3ADC..1000:3B1E): the neighbour's source slot `[0x3E5F+class-1]` is
 * cleared and its live record (`0x3F17 + 0x12*(class-1)`) gets
 * `+6 = 0xFFFF`; `[0x1AF2]` becomes the neighbour with frame `0x103` and
 * flags 0; the **actor** gets frame `0x20`; the status bar goes off
 * (`[0x5B12] = 0`); the fade ramp `202F:0x48BC` is filled.
 *
 * The sequence (1000:3B33..1000:3CF0), one port tick per original frame (`0x9F56` = one
 * vsync, `0x54CA`/`0x9BF8` = a presented frame):
 *
 *   red     entries 1..0x7F -> (0x3F,0,0), div 5, then -> the level div 0x1E
 *   white   1..0x7F -> (0x3F,0x3F,0x3F), div 5, then `1000:3D04`'s two-frame
 *           page-strip save (not a view pan; the port keeps the view still),
 *           then -> the level div 0x19
 *   frame   one `0x9F56`
 *   blinks  10x: the 0x80 bank -> white div n, -> the level div n, n = 10..1
 *           (2 * (10+...+1) = 110 frames)
 *   anim    6 passes of 8 waits + the frame that does the `frame++` (1000:3BE9),
 *           then two frames = 56 frames; the neighbour goes 0x103 -> 0x109
 *   append  (1000:3BF9..1000:3C64) x from the neighbour, y = neighbour + 9, the stashed
 *           recipe fields, `[0x3E73]++`, the behaviour id into the source
 *           slots
 *   reveal  `init_dirty_cell_list` (`1000:3E73`) then `flush_dirty_cell_list`
 *           (`1000:3F4A`) looping `queue_dirty_cell_packed` (`1000:3FB3`) until
 *           `[0x1B02]`: 0x100 dirty cells queued from the 0x1B76 coordinate
 *           table over the 0x48BC list, shaded from 0x19EA — ported as
 *           `level_spark_begin`/`sparkStep`/`sparkDraw`
 *   hold    41 frames, then 1..0xBF -> white div 5, one frame, the neighbour's
 *           `+6 = 0xFFFF`, `[0xC] = [0x5B12] = 0xFF`, two page frames, then
 *           1..0xBF -> the level div 0x28
 */
enum {
    CLAIM_OFF,
    CLAIM_TO_RED, /* 1000:3B56  1..0x7F -> red, div 5                */
    CLAIM_RED_BACK, /* 1000:3B68  -> the level palette, div 0x1E       */
    CLAIM_TO_WHITE, /* 1000:3B82  1..0x7F -> white, div 5              */
    CLAIM_PAN, /* 1000:3B91  1000:3D04: the two-frame present    */
    CLAIM_WHITE_BACK, /* 1000:3B97  -> the level palette, div 0x19       */
    CLAIM_FRAME, /* 1000:3BA9  one 0x9F56 frame, then the blinks    */
    CLAIM_BLINK_WHITE, /* 1000:3BB3  the 0x80 bank -> white, div n        */
    CLAIM_BLINK_LEVEL, /* 1000:3BC3  the 0x80 bank -> the level, div n    */
    CLAIM_ANIM, /* 1000:3BD6  6 x 9 frames, `frame++` per pass     */
    CLAIM_HOLD, /* 1000:3C7D  41 frames                            */
    CLAIM_WHITE_OUT, /* 1000:3C88  1..0xBF -> white, div 5              */
    CLAIM_FRAME2, /* 1000:3C9F  one frame, then the remove           */
    CLAIM_FLIP, /* 1000:3CB5  the two page frames                  */
    CLAIM_SETTLE /* 1000:3CDE  1..0xBF -> the level, div 0x28       */
};

/* --- the renderer (render_objects 1000:5D56 -> 0x7829 -> 0x653C) -------
 *
 * The base sprite of a placed record is `select_object_tile` (1000:6625),
 * gated by the record's flags byte (+6 bit 7, 1000:653C's 794C) and by the
 * per-frame flag table `DS:0x3570 + frame` (bit 7 = skip; the shipped bytes
 * for frames 0x10..0x16 are 01 00 12 12 12 12 12, all drawable).
 *
 * The storage law was measured on the running game and reproduces the screen
 * pixel-exactly (re_map.md, "the renderer chain"): 16 px wide, h rows,
 * 0 = transparent, byte-per-pixel, and
 *
 *   value(col,row) = SPRITES.DAT[frame_data + (col % 4)*(h*4) + row*4 + (col/4)]
 *
 * i.e. four column-class blocks of h*4 bytes.  A frame advances 0x180 and
 * (for h = 24) its slot is 4*0x60 = 0x180 — block = h*4.
 *
 * The frames come from the descriptor table `DS:0x39A8`, which the game builds
 * at boot in `apply_relocations` (1000:569E) from the six manifests at
 * `DS:0x252C`.  The rule is derived and was verified against a live dump
 * (248/248 descriptors byte-identical, re_map.md "the renderer chain"):
 *
 *   id[F]  = word at image DS:0x39A8 + 4F        (pre-baked in the image)
 *   rank   = number of earlier slots with the same id
 *   off    = bswap(word at t1[id] + 4*rank + 2)
 *   X      = (w1<<11) | (hi(t2)<<6) | hi(t3)
 *   rows = X & 0x3F   ybias = (X>>6) & 0x1F   mode = (X>>11) & 3
 *
 * The six manifests are contiguous chunks of `APL\SPRITES.DAT`, so
 * `off` below is the sprite's file offset in that one file (bankbase =
 * {1:0x0000, 2:0x3080, 3:0xD3F0, 4:0xD3F0, 5:0xD3F0, 6:0xE130}, which tile the
 * file's 81744 bytes exactly).  The table below is generated from the image by
 * `tools/sprite_frames.py`; don't hand-edit it.
 *
 * `select_object_tile` (1000:6625) draws at `y + ybias` (`ADD DX,AX` after
 * `SHR AL,1 / AND 0x1F`) and dispatches on the descriptor's mode bits
 * (`CMP AX,1/2` at 1000:66CB/66D0): mode 1 -> `draw_object_scaled_x1`
 * (1000:66EC), mode 2 -> `draw_object_scaled_x2` (1000:6C74), and 0/3 fall to
 * the `RET` at 1000:66D5, i.e. draw nothing.  The port's `_obj_blit_frame` does
 * the same (16 cols / 32 cols / skip).
 */
typedef struct {
    uint32_t off;
    uint8_t rows, ybias, mode;
} ObjSprite;

/* --- the ACTION program machine (script_op_dispatch, 1000:3979) --------
 *
 * `pause_toggle` (`1000:3523`) is the ACTION key with **no direction held** and no dialog up
 * (`[0x6F1D] == 0`): it sets `[0x13A4] = 0xFF` and jumps to `script_op_dispatch` (`1000:4D79`).
 * The dispatch reads the acting record (`get_object_ptr` = `[0x3E24 + 2*[0x3E22]]`, the 29-record
 * pointer table baked in the image at `DS:0x3E24`), takes the current opcode
 * `prog[prog[1] + 1]` from the object's **program** and jumps through the 12-entry CS table at
 * `0x399C`:
 *
 *   1  0x3A26 register the object in the 0x6F22 table (8 slots, stride 0xE), frame 0x20, type 8
 *   2  0x9C1B the collision pass (also op 6) — see below
 *   3  0x3A1A -> 0xECFE  `cmd_select_8c_or_91_and_draw` (needs a class; sets frame 0x8C/0x91)
 *                        - ported inline (`OP_SELECT`)
 *   4  0x3A05 -> 0xEFA1/0xEFB9 per type (<= 2) -> `_obj_op_set_state`: type 1/frame 0xBB for type
 *                        0, type 2/frame 0x17 otherwise - ported (`OP_TYPE_A`)
 *   5  0x39B5 type += 0x1F and frame = 0x10E + 4*type         - ported (`OP_SETFRAME`)
 *   7  0x39CF -> 0xEA0F/0xEA4D the two ride commands - the port halts: port_deviations.md D-92
 *   8  0x39CC -> 0xEA71 `reset_span_entry_on_phase`            - ported (`OP_RESET`)
 *   9  0x39E4 the player claims (AX = 2), otherwise 0xEF0C/0xEF39 `cmd_walk_and_mark_state1_a/b`
 *                        (walk 0x10 / walk 0 and mark state 1) - ported (`OP_CLAIM_OR_TYPE`,
 *                        `_obj_op_mark_walk`)
 *   10 0x3A64 claim with AX = 3     11 0x3A5F claim with AX = 0     12 0x3A5A claim with AX = 1
 *
 * The **program** comes from `[0x5B56]` (1-based) through `get_action_program_ptr`
 * (`1000:98F8` = `[0x5B2B + 2*([0x5B56]-1)]`); the six shipped programs are the overlapping byte
 * runs at `0x5B37..0x5B53` and the index is chosen by `resolve_object_sprite` (`1000:4147`) from
 * the acting record's class through the 8-byte map `0x1C76` (behaviours 4/5/6/7 -> programs
 * 6/2/4/3), or forced to 5 for the player's own record (`[0x180A]`).
 *
 * **The collision pass (op 2/6, `1000:9C1B`)** requires the actor's type to be 0xF or <= 2, then
 * walks the *live* records (`0x3F17`, `[0x3E73] - 9` of them) with the slot cursor at `0x3E5F`:
 * skip a slot with bit 7, skip a record with `+6` bit 7, take the direction `0x5B81[behaviour]`
 * (must not be negative), require `|dx| < 0x10` and `0 <= dy < 0x18`, then call the direction's
 * action through `[0x5B93 + 2*direction]` (`0x9CF0` for 0, `0x9D38` for 1).  On success
 * `[0x1979] = 5`, `[0x1988] = 1`, `[0x5B56] = 1`.
 *
 * **The pickup (`0x9D38`, direction 1 = behaviours 5/6/7)** is what wins level 1: if the found
 * record's `+8 == 0xFFFF` and `+0xA == 0`, it writes the record's **ordinal** into the *actor's*
 * `+7` (the class — `[SI+3] = DL`, DL = the loop's 1-based counter) and marks the found record's
 * slot `| 0x80`.  Live 2026-10-30: the spear's slot went `0x05 -> 0x85` and a record got class 1,
 * so `pred_sprite_class_match(5)` matched (`0x85 & 0x3F = 5`) and the level ended.
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

static int32_t _obj_step_probe_at(const uint8_t *actor, int32_t dx, int32_t dy,
                                  uint32_t *cellOut);
static int32_t _obj_land_y(int32_t x, int32_t y, uint32_t *cellOut);
static uint32_t _obj_style_nudge(const uint8_t *map, uint32_t col,
                                 uint32_t row);
static int32_t
_obj_cmd_home_state(uint8_t *actor); /* 1000:EDB7               */
static void _obj_driver_call(uint32_t n); /* 1000:83B7               */
static void _obj_frame_step_down(uint8_t *actor, uint32_t lo,
                                 uint32_t hi); /* 1000:FDBF */
static uint8_t *_obj_link_record(uint32_t off); /* the +0xE record link */
static void _obj_build_shade(uint32_t frame, uint8_t *out);

/* 0xF2E0 (object_find_overlap): the static record within |dx|,|dy| <= 0x10 of
 * the actor whose type is "overlappable" ([0x6EC3 + type] == 0) becomes frame
 * 0x103, type 0x1E and params 6/0/0xA.  A record that is the player's is
 * replaced by the actor for the conversion ([0x4254]'s swap).  The original
 * runs 0xF453 scan_groups_for_record (DX = 5) before the conversion: the group
 * whose member list holds the acting record is dissolved. */
static void _obj_group_dissolve(uint8_t *e);
int32_t object_stack_scan_dissolve(uint32_t recIndex);

static void _obj_cmd_fall(uint8_t *actor, int32_t x, int32_t y, uint32_t frame);

static uint8_t _obj_array[OBJ_SLOTS * OBJ_STRIDE];
static int32_t _obj_current = -1; /* [0x1AF2], -1 = none */
static int32_t _obj_player = -1; /* [0x180A], -1 = none */
static uint32_t _obj_live_count =
    OBJ_LIVE0; /* [0x3E73]: the array's used slots     */
static uint32_t _obj_placed_humans; /* [0x3E20]: the 0xAA handlers' count   */
static uint32_t _obj_entity; /* [0x3E22]: the acting record          */
static uint32_t _obj_program_index =
    1; /* [0x5B56], 1-based                    */
static uint16_t _obj_cmd_timer; /* [0x6DEA]: the command one-shot */
static uint16_t _obj_stream_count; /* [0x6DEC]: stream hold countdown */
static const uint8_t *_obj_stream_ptr; /* [0x6DEE]: the script stream    */
static uint8_t _obj_stack_mask[2]; /* 0x64AD / 0x64C1: in-a-stack bits */
/* DI at the handler/hook call (1000:E467): the byte offset of the type row's
 * selected half inside _obj_cmd_rows.  Set by object_command_select, read by the
 * hooks (1000:F3ED/0xF423 and 0xF6F6). */
static uint32_t _obj_row_ofs;
static int16_t _obj_side; /* [0x4230]: which side the settle stepped from */
static int32_t _obj_lat; /* [0x6F1D]: sequence owns input */
/* 0x649A / 0x64A2 / 0x64AE / 0x64B6: the four corner-pair blocks the 0xAF..0xB2
 * tiles write at load ({x, y, x, y} followed by a zeroed 12-byte entry, or just
 * {x, y}).  No ported consumer reads them yet - they are the level's "pair"
 * markers, and the write is all the original does. */
static uint16_t _obj_pair_a[8], _obj_pair_b[2], _obj_pair_c[8], _obj_pair_d[2];
static uint32_t _obj_bg_count;
static uint32_t _obj_pending; /* [0x1979]                             */
static uint32_t _obj_cursor = 1; /* [0x1988]                             */
static uint32_t _obj_fault; /* op 7 was dispatched (see below)      */
/* [0x1987] / the port's "inside 0x36DB" state: the turn arms the flag
 * (1000:EE35) and the pending-event 4 charge loop consumes it (1000:370C). */
static uint8_t _obj_charge_arm;
static int32_t _obj_charging;
/* [0x42F0], the ACTION edge's guard, is **never set** in this image: the only
 * two references in the whole file are the test and the clear at 0xE3F9-0xE407,
 * so the edge (0xE40C's `CALL 0x392D`) is dead in the shipped build and the port
 * does not run it.  That is what lets a fire press reach the type row's fire
 * half (D-68). */
static const OSystem *_obj_osys; /* the fatal halt's route (osystem.h)   */
static uint16_t _obj_register[8][7]; /* 0x6F22: 8 x 0xE (clear is 0x70 bytes at
                                          * 0x60F4; op 1 walks AX = 7..0), bit15 = free */
static uint16_t _obj_span[2][14]; /* 0x6F92, 2 x 0x1C: {id, .. +6, +8, +0x12} */
/* The row each span stripe starts at.  The original has no field for it: the
 * stripe's already-painted pixels are the page, and 1000:5D65 redraws from
 * them (the 0x2DB0 area only holds the saved background).  The port rebuilds
 * the page every frame, so it keeps the seed row here. */
static uint16_t _obj_span_top[2];
/* [0x6530]: count_tile_collisions' level-complete flag (1000:C438). */
static uint8_t _obj_goal_flag;
static int32_t _obj_anim_div; /* [0x66B1]: the 6-tick divider */
/* 0x67F1's pending-move chain: tag, phase-2 countdown, slot, phase-1
 * divider/index/cursor into the 0x67FF (offset, divider) table. */
static uint16_t _pend_tag, _pend_count, _pend_slot, _pend_index, _pend_cursor;
static int32_t _pend_div;
static const int16_t _obj_anim_x[6][15][9] = {
    {
        /* length 3 */
        { -2, -1, 0, 0, 0, 0, 0, 0, -1 },
        { -1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 2 },
        { 2, 1, 0, 0, 0, 0, 0, 0, 3 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 4 */
        { -3, -2, -1, 0, 0, 0, 0, 0, -2 },
        { -2, -1, 0, 0, 0, 0, 0, 0, -1 },
        { -1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 2 },
        { 2, 1, 0, 0, 0, 0, 0, 0, 3 },
        { 3, 2, 1, 0, 0, 0, 0, 0, 4 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 5 */
        { -4, -3, -2, -1, 0, 0, 0, 0, -3 },
        { -3, -2, -1, 0, 0, 0, 0, 0, -2 },
        { -2, -1, 0, 0, 0, 0, 0, 0, -1 },
        { -1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 2 },
        { 2, 1, 0, 0, 0, 0, 0, 0, 3 },
        { 3, 2, 1, 0, 0, 0, 0, 0, 4 },
        { 4, 3, 2, 1, 0, 0, 0, 0, 5 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 6 */
        { -5, -4, -3, -2, -1, 0, 0, 0, -4 },
        { -4, -3, -2, -1, 0, 0, 0, 0, -3 },
        { -3, -2, -1, 0, 0, 0, 0, 0, -2 },
        { -2, -1, 0, 0, 0, 0, 0, 0, -1 },
        { -1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 2 },
        { 2, 1, 0, 0, 0, 0, 0, 0, 3 },
        { 3, 2, 1, 0, 0, 0, 0, 0, 4 },
        { 4, 3, 2, 1, 0, 0, 0, 0, 5 },
        { 5, 4, 3, 2, 1, 0, 0, 0, 6 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 7 */
        { -6, -5, -4, -3, -2, -1, 0, 0, -5 },
        { -5, -4, -3, -2, -1, 0, 0, 0, -4 },
        { -4, -3, -2, -1, 0, 0, 0, 0, -3 },
        { -3, -2, -1, 0, 0, 0, 0, 0, -2 },
        { -2, -1, 0, 0, 0, 0, 0, 0, -1 },
        { -1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 2 },
        { 2, 1, 0, 0, 0, 0, 0, 0, 3 },
        { 3, 2, 1, 0, 0, 0, 0, 0, 4 },
        { 4, 3, 2, 1, 0, 0, 0, 0, 5 },
        { 5, 4, 3, 2, 1, 0, 0, 0, 6 },
        { 6, 5, 4, 3, 2, 1, 0, 0, 7 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 8 */
        { -7, -6, -5, -4, -3, -2, -1, 0, -6 },
        { -6, -5, -4, -3, -2, -1, 0, 0, -5 },
        { -5, -4, -3, -2, -1, 0, 0, 0, -4 },
        { -4, -3, -2, -1, 0, 0, 0, 0, -3 },
        { -3, -2, -1, 0, 0, 0, 0, 0, -2 },
        { -2, -1, 0, 0, 0, 0, 0, 0, -1 },
        { -1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 2 },
        { 2, 1, 0, 0, 0, 0, 0, 0, 3 },
        { 3, 2, 1, 0, 0, 0, 0, 0, 4 },
        { 4, 3, 2, 1, 0, 0, 0, 0, 5 },
        { 5, 4, 3, 2, 1, 0, 0, 0, 6 },
        { 6, 5, 4, 3, 2, 1, 0, 0, 7 },
        { 7, 6, 5, 4, 3, 2, 1, 0, 0 },
    },
};

/* 0x6DF2: the static two-byte (frame, hold) stream the type-2 command-8
 * handler walks; 0xFF 0xFF terminates and wraps. */
static const uint8_t _obj_anim_stream[] = {
    0x10, 0xC8, 0x10, 0xC8, 0x12, 0x05, 0x10, 0x14, 0x12, 0x05, 0x10, 0xC8,
    0x10, 0xC8, 0x13, 0x06, 0x14, 0x06, 0x15, 0x06, 0x16, 0x28, 0x15, 0x06,
    0x14, 0x06, 0x13, 0x06, 0x10, 0xC8, 0x12, 0x05, 0x10, 0x0A, 0x12, 0x05,
    0x10, 0x32, 0x13, 0x06, 0x14, 0x06, 0x15, 0x06, 0x16, 0x23, 0x15, 0x06,
    0x14, 0x06, 0x13, 0x06, 0x10, 0xC8, 0x10, 0xC8, 0x12, 0x05, 0x10, 0x14,
    0x12, 0x05, 0x10, 0xC8, 0x10, 0xC8, 0xFF, 0xFF,
};

/* [0x3E5E + b] — the class byte of the object in source slot b (0 = none).
 * `pred_sprite_class_match` (1000:3659) reads it as `[0x3E5E + rec[+7]] & 0x3F`
 * and the claim frees the slot (`[0x3E5E + rec[7]] = 0`, 1000:3B1E); the spawn
 * writes the behaviour id at `[0x3E5F + n]`, i.e. slot (n+1). */
static uint8_t _obj_slot_class[0x40];

/** The original's `[0x3E5E + b]` is not an array: it is a byte window of the
 * data segment - the 21 class bytes 0x3E5E..0x3E72, the live-count word at
 * 0x3E73/0x3E74, then the 29 0x12-byte records at 0x3E75.  A record's `+7` is
 * that byte index (1000:3659, 0x4147, 0xF2CF read it directly; 0x653C, 0x9D5D,
 * 0xF603 and 0xDD3C reach it as 0x3E5F + cls - 1), and the byte can be any
 * value a record carries (1000:D12B writes 0x99), so the accessors map the
 * whole window instead of indexing _obj_slot_class past its end. */
static uint8_t _obj_class_byte_at(uint32_t b)
{
    b &= 0xFFu;
    if (b < 0x15u) return _obj_slot_class[b]; /* 0x3E5E..0x3E72 */
    if (b == 0x15u) return (uint8_t)_obj_live_count; /* 0x3E73 */
    if (b == 0x16u) return (uint8_t)(_obj_live_count >> 8); /* 0x3E74 */
    return _obj_array[b - 0x17u]; /* 0x3E75.., b <= 0xFF */
}

/** @brief Set one slot's class byte. */
static void _obj_class_byte_set(uint32_t b, uint8_t value)
{
    b &= 0xFFu;
    if (b < 0x15u) {
        _obj_slot_class[b] = value;
        return;
    }
    if (b == 0x15u) {
        _obj_live_count = (_obj_live_count & ~0xFFu) | value;
        return;
    }
    if (b == 0x16u) {
        _obj_live_count = (_obj_live_count & ~0xFF00u) | ((uint32_t)value << 8);
        return;
    }
    _obj_array[b - 0x17u] = value;
}

/* 1000:EEB8's class-0 record write.  `DEC DX` wraps to 0xFFFF, `MUL DL`
 * uses 0xFF, so the record base is 0x3F17 + 0x12*0xFF = 0x5105 and the
 * +8..+0xE writes land on 0x510D..0x5114.  No image instruction references
 * that range; the bytes are kept so the write is part of the state. */
static uint8_t _obj_ds_tail5105[0x10]; /** 0x5105..0x5114 */

/** @brief Accessor: one byte of the out-of-range DS tail record. */
uint8_t object_ds_tail5105(uint32_t off)
{ return off < sizeof _obj_ds_tail5105 ? _obj_ds_tail5105[off] : 0u; }

/* The four shipped recipes (202F:0x19BA, 12 bytes each): [uses][behaviour]
 * then the sprite word and the four parameter words, re-armed to one use
 * every level start (level_setup 1000:31C3). */
static const uint8_t _obj_recipes[4][12] = {
    { 1, 5, 0x00, 0x60, 0xFF, 0xFF, 0, 0, 0, 0, 0,
      0 }, /* op 11 -> behaviour 5 */
    { 1, 7, 0x00, 0x96, 0xFF, 0xFF, 0, 0, 0, 0, 0,
      0 }, /* op 12 -> behaviour 7 */
    { 1, 6, 0x00, 0x9C, 0xFF, 0xFF, 0, 0, 0, 0, 0,
      0 }, /* AX=2, never fired    */
    { 1, 4, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF },
    /* op 10: the image's 01 04 00 38 00 00 00 00 ff ff ff ff */
};
static uint8_t _obj_uses[4]; /* the live copy of [DI+0] */

/* the frame counter the low-phase predicate gates on ([0x4103]) */
static uint32_t _obj_frame;
static uint32_t _low_phase_count; /* [0x1976]: 0x8C = 140 */

static int32_t _claim_phase = CLAIM_OFF;
static int32_t _claim_neighbour = -1;
static uint32_t _claim_behav, _claim_sprite;
static uint32_t _claim_params[4];
static uint32_t _claim_blinks, _claim_timer;
static uint8_t _claim_red[768], _claim_white[768];

/* 202F:0x1EB3, the tile ids the walk matches, in table order. */
static const uint8_t _obj_tiles[25] = {
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1,
    0xB2, 0xB3, 0xB4, 0xB5, 0xC0, 0xB6, 0xB7, 0xB8, 0xBA,
    0xBB, 0xBE, 0xBF, 0xC1, 0xF0, 0xF1, 0xF2,
};

/* 0xBE / 0xBF's hand-off: 0xBE keeps the record it appended (`0x1F1C`) and the
 * slot 0xBF joins into (`0x1F1E`); the port keeps the record index. */
static int32_t _obj_join_slot = -1;

static const ObjSprite _obj_sprites[OBJ_FRAME_SLOTS] = {
    /* 000 */ { 12416, 24, 0, 1 },
    { 12800, 24, 0, 1 },
    { 13184, 23, 1, 1 },
    { 13552, 23, 1, 1 },
    /* 004 */ { 13920, 24, 0, 1 },
    { 14304, 24, 0, 1 },
    { 14688, 23, 1, 1 },
    { 15056, 23, 1, 1 },
    /* 008 */ { 15424, 24, 0, 1 },
    { 15808, 24, 0, 1 },
    { 16192, 23, 1, 1 },
    { 16560, 23, 1, 1 },
    /* 00C */ { 16928, 24, 0, 1 },
    { 17312, 24, 0, 1 },
    { 17696, 23, 1, 1 },
    { 18064, 23, 1, 1 },
    /* 010 */ { 18432, 24, 0, 1 },
    { 18816, 0, 24, 1 },
    { 18816, 24, 0, 1 },
    { 19200, 24, 0, 1 },
    /* 014 */ { 19584, 24, 0, 1 },
    { 19968, 24, 0, 1 },
    { 20352, 24, 0, 1 },
    { 0, 13, 2, 1 },
    /* 018 */ { 20736, 0, 24, 1 },
    { 20736, 0, 24, 1 },
    { 20736, 0, 24, 1 },
    { 20736, 0, 24, 1 },
    /* 01C */ { 20736, 0, 24, 1 },
    { 54256, 0, 0, 1 },
    { 54256, 0, 0, 1 },
    { 54256, 0, 16, 1 },
    /* 020 */ { 20736, 24, 0, 1 },
    { 208, 16, 0, 1 },
    { 464, 16, 0, 1 },
    { 21120, 24, 0, 1 },
    /* 024 */ { 21504, 23, 0, 1 },
    { 21872, 24, 0, 1 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    /* 028 */ { 54256, 9, 15, 2 },
    { 54544, 15, 9, 2 },
    { 55024, 18, 6, 2 },
    { 55600, 20, 4, 2 },
    /* 02C */ { 56240, 22, 2, 2 },
    { 56944, 22, 1, 2 },
    { 720, 14, 0, 1 },
    { 944, 14, 0, 1 },
    /* 030 */ { 1168, 14, 0, 1 },
    { 1392, 14, 0, 1 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    /* 038 */ { 1616, 16, 0, 1 },
    { 1872, 16, 0, 1 },
    { 2128, 16, 0, 1 },
    { 2384, 16, 0, 1 },
    /* 03C */ { 57648, 32, 0, 2 },
    { 58672, 31, 1, 2 },
    { 59664, 32, 0, 2 },
    { 60688, 31, 1, 2 },
    /* 040 */ { 61680, 0, 0, 3 },
    { 61680, 32, 0, 2 },
    { 62704, 31, 1, 2 },
    { 63696, 32, 0, 2 },
    /* 044 */ { 64720, 31, 1, 2 },
    { 65712, 32, 0, 2 },
    { 66736, 31, 1, 2 },
    { 67728, 0, 0, 3 },
    /* 048 */ { 67728, 32, 0, 2 },
    { 68752, 31, 1, 2 },
    { 69744, 32, 0, 2 },
    { 70768, 32, 0, 2 },
    /* 04C */ { 71792, 32, 0, 2 },
    { 72816, 32, 0, 2 },
    { 73840, 32, 0, 2 },
    { 74864, 32, 0, 2 },
    /* 050 */ { 75888, 32, 0, 2 },
    { 76912, 32, 0, 2 },
    { 22256, 24, 0, 1 },
    { 22640, 23, 0, 1 },
    /* 054 */ { 23008, 24, 0, 1 },
    { 23392, 24, 0, 1 },
    { 23776, 23, 1, 1 },
    { 24144, 24, 0, 1 },
    /* 058 */ { 2640, 16, 0, 1 },
    { 2896, 16, 0, 1 },
    { 3152, 14, 0, 1 },
    { 3376, 7, 4, 1 },
    /* 05C */ { 3488, 5, 5, 1 },
    { 3568, 11, 2, 1 },
    { 3744, 16, 0, 1 },
    { 4000, 16, 0, 1 },
    /* 060 */ { 4256, 16, 0, 1 },
    { 4512, 16, 0, 1 },
    { 4768, 14, 2, 1 },
    { 4992, 7, 5, 1 },
    /* 064 */ { 5104, 5, 6, 1 },
    { 5184, 11, 3, 1 },
    { 5360, 16, 0, 1 },
    { 5616, 16, 0, 1 },
    /* 068 */ { 24528, 5, 13, 1 },
    { 24608, 5, 13, 1 },
    { 24688, 5, 12, 1 },
    { 24768, 5, 14, 1 },
    /* 06C */ { 24848, 6, 13, 1 },
    { 24944, 5, 14, 1 },
    { 25024, 5, 15, 1 },
    { 25104, 5, 15, 1 },
    /* 070 */ { 25184, 6, 13, 1 },
    { 25280, 5, 13, 1 },
    { 25360, 5, 12, 1 },
    { 25440, 5, 14, 1 },
    /* 074 */ { 25520, 5, 14, 1 },
    { 25600, 5, 14, 1 },
    { 25680, 5, 14, 1 },
    { 25760, 4, 15, 1 },
    /* 078 */ { 25824, 4, 15, 1 },
    { 25888, 6, 0, 1 },
    { 25984, 5, 0, 1 },
    { 26064, 5, 1, 1 },
    /* 07C */ { 26144, 6, 0, 1 },
    { 26240, 5, 0, 1 },
    { 26320, 5, 1, 1 },
    { 26400, 24, 0, 1 },
    /* 080 */ { 26784, 24, 0, 1 },
    { 27168, 24, 0, 1 },
    { 5872, 16, 0, 1 },
    { 6128, 15, 1, 1 },
    /* 084 */ { 6368, 16, 0, 1 },
    { 6624, 15, 1, 1 },
    { 6864, 16, 0, 1 },
    { 7120, 15, 1, 1 },
    /* 088 */ { 7360, 16, 0, 1 },
    { 7616, 15, 1, 1 },
    { 7856, 16, 0, 1 },
    { 8112, 16, 0, 1 },
    /* 08C */ { 27552, 24, 0, 1 },
    { 27936, 24, 0, 1 },
    { 28320, 24, 0, 1 },
    { 28704, 0, 24, 1 },
    /* 090 */ { 28704, 0, 24, 1 },
    { 28704, 24, 0, 1 },
    { 29088, 24, 0, 1 },
    { 29472, 24, 0, 1 },
    /* 094 */ { 29856, 0, 24, 1 },
    { 29856, 0, 24, 1 },
    { 8368, 6, 10, 1 },
    { 8464, 13, 1, 1 },
    /* 098 */ { 8672, 15, 1, 1 },
    { 8912, 15, 1, 1 },
    { 9152, 14, 1, 1 },
    { 9376, 12, 1, 1 },
    /* 09C */ { 9568, 12, 4, 1 },
    { 9760, 13, 3, 1 },
    { 9968, 11, 5, 1 },
    { 10144, 0, 16, 1 },
    /* 0A0 */ { 10144, 0, 16, 1 },
    { 10144, 7, 7, 1 },
    { 10256, 10, 3, 1 },
    { 10416, 14, 0, 1 },
    /* 0A4 */ { 10640, 14, 0, 1 },
    { 10864, 10, 3, 1 },
    { 11024, 7, 7, 1 },
    { 11136, 0, 16, 1 },
    /* 0A8 */ { 11136, 0, 16, 1 },
    { 11136, 10, 4, 1 },
    { 11296, 13, 1, 1 },
    { 29856, 24, 0, 1 },
    /* 0AC */ { 30240, 24, 0, 1 },
    { 30624, 24, 0, 1 },
    { 31008, 24, 0, 1 },
    { 31392, 16, 8, 1 },
    /* 0B0 */ { 31648, 16, 8, 1 },
    { 31904, 23, 1, 1 },
    { 32272, 22, 2, 1 },
    { 32624, 23, 1, 1 },
    /* 0B4 */ { 32992, 15, 9, 1 },
    { 33232, 13, 11, 1 },
    { 33440, 13, 11, 1 },
    { 0, 0, 0, 0 },
    /* B8.. */ { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 33648, 23, 1, 1 },
    /* 0BC */ { 34016, 24, 0, 1 },
    { 34400, 23, 0, 1 },
    { 34768, 23, 1, 1 },
    { 35136, 24, 0, 1 },
    /* 0C0 */ { 35520, 23, 0, 1 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    /* C8.. */ { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
    { 11504, 11, 5, 1 },
    { 11680, 11, 5, 1 },
    { 11856, 8, 8, 1 },
    /* 0D0 */ { 11984, 8, 8, 1 },
    { 12112, 5, 11, 1 },
    { 12192, 5, 11, 1 },
    { 12272, 9, 7, 1 },
    /* 0D4 */ { 35888, 24, 0, 1 },
    { 36272, 23, 1, 1 },
    { 36640, 24, 0, 1 },
    { 37024, 24, 0, 1 },
    /* 0D8 */ { 37408, 24, 0, 1 },
    { 37792, 23, 1, 1 },
    { 38160, 24, 0, 1 },
    { 38544, 24, 0, 1 },
    /* 0DC */ { 38928, 23, 1, 1 },
    { 39296, 23, 1, 1 },
    { 39664, 24, 0, 1 },
    { 40048, 23, 1, 1 },
    /* 0E0 */ { 40416, 24, 0, 1 },
    { 40800, 23, 1, 1 },
    { 41168, 24, 0, 1 },
    { 41552, 24, 0, 1 },
    /* 0E4 */ { 41936, 24, 0, 1 },
    { 42320, 23, 1, 1 },
    { 42688, 24, 0, 1 },
    { 43072, 24, 0, 1 },
    /* 0E8 */ { 43456, 23, 1, 1 },
    { 43824, 23, 1, 1 },
    { 44192, 24, 0, 1 },
    { 44576, 23, 1, 1 },
    /* 0EC */ { 44944, 4, 14, 1 },
    { 45008, 4, 15, 1 },
    { 45072, 4, 13, 1 },
    { 45136, 4, 12, 1 },
    /* 0F0 */ { 45200, 5, 14, 1 },
    { 45280, 4, 14, 1 },
    { 45344, 4, 13, 1 },
    { 45408, 4, 12, 1 },
    /* 0F4 */ { 45472, 4, 2, 1 },
    { 45536, 4, 14, 1 },
    { 45600, 4, 15, 1 },
    { 45664, 4, 13, 1 },
    /* 0F8 */ { 45728, 4, 12, 1 },
    { 45792, 5, 14, 1 },
    { 45872, 4, 14, 1 },
    { 45936, 4, 13, 1 },
    /* 0FC */ { 46000, 4, 12, 1 },
    { 46064, 4, 2, 1 },
    { 77936, 24, 8, 2 },
    { 78704, 24, 8, 2 },
    /* 100 */ { 79472, 22, 10, 2 },
    { 80176, 22, 10, 2 },
    { 80880, 27, 5, 2 },
    { 46128, 24, 0, 1 },
    /* 104 */ { 46512, 24, 0, 1 },
    { 46896, 24, 0, 1 },
    { 47280, 24, 0, 1 },
    { 47664, 24, 0, 1 },
    /* 108 */ { 48048, 24, 0, 1 },
    { 48432, 24, 0, 1 },
    { 48816, 19, 5, 1 },
    { 49120, 11, 13, 1 },
    /* 10C */ { 49296, 14, 10, 1 },
    { 49520, 14, 10, 1 },
    { 49744, 24, 0, 1 },
    { 50128, 24, 0, 1 },
    /* 110 */ { 50512, 24, 0, 1 },
    { 50128, 24, 0, 1 },
    { 50896, 24, 0, 1 },
    { 51280, 24, 0, 1 },
    /* 114 */ { 51664, 24, 0, 1 },
    { 51280, 24, 0, 1 },
    { 52048, 23, 1, 1 },
    { 52416, 23, 1, 1 },
    /* 118 */ { 52784, 23, 1, 1 },
    { 52416, 23, 1, 1 },
    { 53152, 23, 1, 1 },
    { 53520, 23, 1, 1 },
    /* 11C */ { 53888, 23, 1, 1 },
    { 53520, 23, 1, 1 },
};

static const uint8_t *_obj_bank; /* APL\\SPRITES.DAT, the sprite image */
static uint32_t _obj_bank_size;

/* --- the class/behaviour overlays of 1000:653C -------------------------
 *
 * `scan_object_rows` (1000:6433) sends every record whose `+7` (the class slot
 * id) is non-zero to `draw_object_cell` (1000:653C) instead of drawing its base
 * tile directly.  That routine draws the base - unless the frame's own byte in
 * `0x3570` has bit 7 set - and then composes the class's *carried* sprite: the
 * placed record the class id names is moved to the record's position plus a
 * per-entry (dx, dy), given a frame and drawn, and one more sprite may be drawn
 * at the record's own position.
 *
 * The four tables are copied from the image (DS offset + 0x106F0 in the file):
 *
 *   0x3566  10 bytes    behaviour class -> the overlay column (0 = no overlay);
 *                       the port's `_obj_cls_col` copies 0x3566..0x35A5, i.e. the
 *                       col table plus the frame table's first bytes, which is
 *                       exactly what the original's unmasked `[0x3566+beh]` read
 *   0x3570  0x11A       frame -> overlay row | 0x80 ("no base tile"), frames
 *                       0..0x119 (the bytes before the 0x368A index table)
 *   0x368A  75 x 3      row x column -> overlay entry index (0 = none)
 *   0x3766  144 x 4     entry: dx, dy, frame, trailing frame (0 = none)
 *
 * Frames past the table read row 0 (the port's frames end at 0x11D, and
 * 0x11A..0x11D have no row in the image either).
 */
static const uint8_t _obj_cls_col[0x40] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x00, 0x12, 0x12, 0x12, 0x12, 0x12,
    0x00, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x15, 0x16, 0x16,
    0x16, 0xC3, 0xC4, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t _obj_frame_row[0x11A] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x00, 0x12, 0x12, 0x12, 0x12, 0x12, 0x00,
    0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x15, 0x16, 0x16, 0x16, 0xC3,
    0xC4, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC7,
    0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x18, 0x19, 0x00,
    0x00, 0x1A, 0x1B, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x1D, 0x1D, 0x1E, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x00, 0x00, 0x00, 0x2E, 0x2F, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0x38, 0x39, 0x38, 0x3A, 0x3B,
    0x3C, 0x3B, 0x3D, 0x3E, 0x3F, 0x3E,
};
static const uint8_t _obj_overlay_idx[0xE1] = {
    0x40, 0x41, 0x42, 0x41, 0x01, 0x1C, 0x30, 0x02, 0x1D, 0x31, 0x03, 0x1E,
    0x32, 0x04, 0x1F, 0x33, 0x05, 0x20, 0x34, 0x06, 0x21, 0x35, 0x07, 0x22,
    0x36, 0x08, 0x23, 0x37, 0x09, 0x24, 0x38, 0x0A, 0x25, 0x39, 0x0B, 0x26,
    0x3A, 0x0C, 0x27, 0x3B, 0x0D, 0x28, 0x3C, 0x0E, 0x29, 0x3D, 0x0F, 0x2A,
    0x3E, 0x10, 0x2B, 0x3F, 0x11, 0x2C, 0x40, 0x11, 0x2C, 0x40, 0x12, 0x2D,
    0x41, 0x13, 0x2E, 0x42, 0x14, 0x2F, 0x43, 0x15, 0x8E, 0x8F, 0x16, 0x46,
    0x4C, 0x17, 0x47, 0x4D, 0x18, 0x48, 0x4E, 0x19, 0x49, 0x4F, 0x1A, 0x4A,
    0x50, 0x1B, 0x4B, 0x51, 0x00, 0x44, 0x00, 0x00, 0x45, 0x00, 0x52, 0x00,
    0x00, 0x53, 0x00, 0x00, 0x54, 0x00, 0x00, 0x55, 0x00, 0x00, 0x56, 0x00,
    0x00, 0x57, 0x00, 0x00, 0x58, 0x00, 0x00, 0x59, 0x00, 0x00, 0x5A, 0x00,
    0x00, 0x5B, 0x00, 0x00, 0x5C, 0x00, 0x00, 0x5D, 0x00, 0x00, 0x5E, 0x00,
    0x00, 0x5F, 0x00, 0x00, 0x60, 0x00, 0x00, 0x61, 0x00, 0x00, 0x62, 0x00,
    0x00, 0x63, 0x00, 0x00, 0x64, 0x00, 0x00, 0x65, 0x00, 0x00, 0x66, 0x00,
    0x00, 0x67, 0x00, 0x00, 0x68, 0x00, 0x00, 0x69, 0x00, 0x00, 0x6A, 0x00,
    0x70, 0x6B, 0x00, 0x71, 0x6C, 0x00, 0x72, 0x6D, 0x00, 0x73, 0x6E, 0x00,
    0x74, 0x6F, 0x00, 0x75, 0x76, 0x00, 0x00, 0x77, 0x00, 0x00, 0x78, 0x00,
    0x00, 0x79, 0x00, 0x00, 0x7A, 0x00, 0x00, 0x7B, 0x00, 0x00, 0x7C, 0x7F,
    0x82, 0x7D, 0x80, 0x83, 0x7E, 0x81, 0x84, 0x85, 0x88, 0x8B, 0x86, 0x89,
    0x8C, 0x87, 0x8A, 0x8D, 0x00, 0x00, 0x00, 0x00, 0x02,
};
static const uint8_t _obj_overlay_ent[0x240] = {
    0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x59, 0x68, 0x04, 0x08, 0x5A, 0x69,
    0x06, 0x06, 0x5A, 0x6A, 0x04, 0x09, 0x5A, 0x6B, 0x01, 0x08, 0x5A, 0x6C,
    0x00, 0x09, 0x5A, 0x6D, 0xFC, 0x0A, 0x5B, 0x6E, 0xFF, 0x09, 0x5A, 0x6F,
    0xFF, 0x07, 0x66, 0x70, 0xFB, 0x05, 0x66, 0x71, 0xFC, 0x06, 0x66, 0x72,
    0xFC, 0x07, 0x66, 0x73, 0x00, 0x06, 0x66, 0x74, 0x01, 0x07, 0x66, 0x75,
    0x03, 0x07, 0x65, 0x76, 0x02, 0x08, 0x66, 0x77, 0x06, 0x08, 0x58, 0x78,
    0x00, 0x08, 0x66, 0x70, 0x00, 0x10, 0x66, 0x70, 0x00, 0x20, 0x66, 0x70,
    0x00, 0xFA, 0x5C, 0x00, 0xFE, 0xFB, 0x5B, 0x79, 0xFD, 0xFB, 0x5B, 0x7A,
    0xFC, 0xFC, 0x5A, 0x7B, 0x01, 0xF9, 0x65, 0x7C, 0x02, 0xF9, 0x65, 0x7D,
    0x05, 0xFB, 0x66, 0x7E, 0x00, 0x0C, 0x99, 0x68, 0x05, 0x0B, 0x9A, 0x69,
    0x07, 0x0A, 0x9B, 0x6A, 0x05, 0x0D, 0x9A, 0x6B, 0x01, 0x0C, 0x99, 0x6C,
    0xFF, 0x0D, 0x98, 0x6D, 0xFB, 0x0E, 0x97, 0x6E, 0xFE, 0x0D, 0x98, 0x6F,
    0x00, 0x0D, 0x99, 0x70, 0xFC, 0x0A, 0x98, 0x71, 0xFB, 0x0B, 0x97, 0x72,
    0xFC, 0x0D, 0x98, 0x73, 0x00, 0x0D, 0x99, 0x74, 0x02, 0x0D, 0x9A, 0x75,
    0x06, 0x0D, 0x9B, 0x76, 0x02, 0x0E, 0x9A, 0x77, 0x05, 0x0D, 0x99, 0x78,
    0x00, 0x0D, 0x99, 0x70, 0x00, 0x15, 0x99, 0x70, 0x00, 0x25, 0x99, 0x70,
    0x04, 0x05, 0xA2, 0x68, 0x06, 0x04, 0xA2, 0x69, 0x04, 0x03, 0xA3, 0x6A,
    0x03, 0x05, 0xA3, 0x6B, 0x00, 0x05, 0xA3, 0x6C, 0x03, 0x06, 0xA2, 0x6D,
    0xFE, 0x07, 0xA1, 0x6E, 0x02, 0x06, 0xA2, 0x6F, 0x00, 0x05, 0xA4, 0x70,
    0xFD, 0x03, 0xA4, 0x71, 0xFC, 0x02, 0xA4, 0x72, 0xFE, 0x05, 0xA4, 0x73,
    0x00, 0x05, 0xA4, 0x74, 0xFE, 0x05, 0xA5, 0x75, 0x02, 0x06, 0xA6, 0x76,
    0xFF, 0x06, 0xA5, 0x77, 0x06, 0x03, 0xAA, 0x78, 0x00, 0x05, 0xA4, 0x70,
    0x00, 0x0D, 0xA4, 0x70, 0x00, 0x1D, 0xA4, 0x70, 0xFC, 0x08, 0x96, 0x00,
    0x02, 0x08, 0x96, 0x00, 0xFD, 0xF9, 0x97, 0x79, 0xFC, 0xF9, 0x97, 0x7A,
    0xFB, 0xFA, 0x97, 0x7B, 0x03, 0xF9, 0x97, 0x7C, 0x04, 0xF9, 0x97, 0x7D,
    0x05, 0xFA, 0x97, 0x7E, 0x00, 0xF8, 0xA2, 0x79, 0xFF, 0xF8, 0xA2, 0x7A,
    0xFE, 0xF9, 0xA2, 0x7B, 0xFF, 0xF8, 0xA5, 0x7C, 0x00, 0xF8, 0xA5, 0x7D,
    0x01, 0xF9, 0xA5, 0x7E, 0x06, 0x09, 0x60, 0x00, 0x01, 0x10, 0x61, 0x00,
    0xF7, 0x08, 0x64, 0x00, 0xF9, 0x09, 0x60, 0x00, 0xFC, 0x10, 0x5F, 0x00,
    0x0A, 0x09, 0x5C, 0x00, 0xFD, 0x09, 0x5C, 0xEC, 0xFE, 0x09, 0x5C, 0xED,
    0x00, 0x08, 0x5C, 0xEE, 0x00, 0x06, 0x5B, 0xEF, 0xFE, 0x08, 0x5B, 0xF0,
    0xFF, 0x08, 0x5B, 0xF1, 0x00, 0x07, 0x5B, 0xF2, 0xFF, 0x06, 0x5B, 0xF3,
    0xFC, 0xFD, 0x5B, 0xF4, 0x02, 0x08, 0x64, 0xF5, 0x01, 0x08, 0x64, 0xF6,
    0xFF, 0x07, 0x64, 0xF7, 0x00, 0x06, 0x64, 0xF8, 0x03, 0x08, 0x64, 0xF9,
    0x01, 0x08, 0x64, 0xFA, 0x00, 0x07, 0x64, 0xFB, 0x02, 0x06, 0x64, 0xFC,
    0x04, 0xFB, 0x65, 0xFD, 0x04, 0x07, 0x5B, 0x68, 0x08, 0x07, 0x5B, 0x69,
    0x09, 0x06, 0x5B, 0x6A, 0xFC, 0x07, 0x64, 0x70, 0xF9, 0x07, 0x64, 0x71,
    0xF6, 0x03, 0x65, 0x72, 0x06, 0x04, 0xA2, 0x68, 0x08, 0x04, 0xA2, 0x69,
    0x0A, 0x03, 0xA2, 0x6A, 0xFB, 0x04, 0xA5, 0x70, 0xF8, 0x04, 0xA5, 0x71,
    0xF6, 0x03, 0xA5, 0x72, 0xFE, 0x09, 0x5C, 0xEC, 0xFF, 0x09, 0x5C, 0xED,
    0x01, 0x08, 0x5C, 0xEE, 0x02, 0x08, 0x64, 0xF5, 0x01, 0x08, 0x64, 0xF6,
    0x00, 0x06, 0x64, 0xF8, 0xFB, 0xFA, 0x59, 0x00, 0xFB, 0xFD, 0x58, 0x00,
    0xFB, 0x01, 0x58, 0x00, 0xFA, 0xFF, 0x97, 0x00, 0xFA, 0x05, 0x97, 0x00,
    0xFA, 0x0A, 0x97, 0x00, 0xFC, 0xF9, 0xA4, 0x00, 0xFB, 0xFE, 0xA4, 0x00,
    0xFC, 0x03, 0xA4, 0x00, 0xFA, 0xFD, 0x66, 0x00, 0xFD, 0xF9, 0x66, 0x00,
    0xFF, 0xF8, 0x66, 0x00, 0xF8, 0x05, 0x97, 0x00, 0xFB, 0x00, 0x97, 0x00,
    0xFD, 0xFE, 0x97, 0x00, 0xFB, 0xFC, 0xA4, 0x00, 0xFD, 0xF8, 0xA4, 0x00,
    0xFF, 0xF8, 0xA4, 0x00, 0x00, 0xF2, 0x96, 0x00, 0x00, 0xF7, 0xA1, 0x00,
};

/* The base tile of one frame at a world position (select_object_tile
 * 1000:6625's draw): the sprite table entry decides the mode, the row count and
 * the pixel bias. */
/* The DEFS field-G sprites: level_setup_screen stores each G block's far
 * pointer into the frame table at 0x3CAC (slots 0xC1 + 4*layer + i), so the
 * F0/F1/F2 decoration records (frames 0xC1/0xC5/0xC9) draw the level's G art -
 * the water-hole sprites.  The image's own slots are zero there, which is why
 * the generator leaves them blank; the live table holds the G pointers. */
static const uint8_t *_obj_g_sprites;
static uint32_t _obj_g_blocks, _obj_gn;
static const uint8_t *_obj_e_sprite, *_obj_h_sprites;

/* --- 1000:5C2F draw_objects_pass2: the two TER.DAT slots (DS:0x649A) ----
 *
 * `update_object_list` ends with a two-entry loop over `0x649A` (`0x14` bytes
 * each): `AX = [slot] - 0xC`, `BX = [slot+2]`, `CX = [slot+0xC]`, and the slot is
 * skipped when `word [slot+0x10]` has bit 15 set.  Otherwise `ES` becomes the
 * `apl\TER.dat` slot (`[0x13AF]`) and `0x5C2F` reads, for the index
 * `byte[0x2D7E + CX]`: `BE16(4*idx + 2) + 0x40` (the source), `BE16(2*idx + 0x20)`
 * (a y offset) and `BE16(2*idx + 0x30)` (the rows), then jumps to
 * `set_object_layer` (`0x5C5F`, mode 2) - the same blitter as the entity cells.
 *
 * **On shipped data the pass draws nothing**: `object_array_init` (`0x4CAB`)
 * writes 0xFFFF into both flag words (`[0x64AA]`, `[0x64BE]`) at every level
 * start, and the live game confirms it - read in play (level 1):
 *
 *   slot0 = 00 x16, ff ff, 00 00      slot1 = the same
 *
 * so the bit-15 test skips both.  The port models the slots and the init so the
 * behaviour is the same, and a test drives the body with the flag cleared.
 */
static uint8_t _obj_ter_slot[2][0x14];

/* the index table at DS:0x2D7E (the caller's `+0xC` word goes through it) */
static const uint8_t _obj_ter_index[20] = {
    0x00, 0x01, 0x02, 0x03, 0x03, 0x02, 0x04, 0x05, 0x06, 0x07,
    0x06, 0x05, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x92, 0x2D,
};
static const uint8_t *_obj_ter_bank;
static uint32_t _obj_ter_size;

/* --- 1000:C295 update_list: the placed records' behaviour walk ---------
 *
 * `update_world` (1000:80AD at 1000:80BD) walks the live records from 0x3F17
 * with the parallel behaviour bytes at 0x3E5F and dispatches through the
 * ten-word table at 202f:0x6535 (`byte & 0x3F`).  The handlers below are the
 * animation/movement ones; 5's/7's move scan (1000:E039) and 4's projectile
 * machinery are ported too (D-88/D-97/D-99 closed 2026-11-04), and the render
 * pass still draws every record.
 */
static const uint8_t _obj_seq6568[0x64] = {
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

/* DS:0x6EC3, 16 bytes: object_find_overlap's "this type does not take part"
 * flag.  Types 0..2 and 8 are 0 (they *do*), the rest 1.  Types >= 0x10 read
 * past the table in the original; the port treats them as "no". */
static const uint8_t _obj_type_overlaps[16] = { 0, 0, 0, 1, 1, 1, 1, 1,
                                                0, 1, 1, 1, 1, 1, 1, 1 };
/* 0x6847: the per-record settled position (4 bytes per record). */
static uint16_t _obj_settled[OBJ_SLOTS][2];

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

/* 0x68DE and the bytes after it: the equal-landing y step, indexed by
 * `frame - 0x58` (1000:E163).  Frames 0x96.. reach past the 16-byte table, so
 * the image window it lands in is kept verbatim (0x60 bytes). */
static const int8_t _obj_land_step[0x60] = {
    0x00,  0x02,  0x03,  0x03,  -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    0x05,  0x02,  0x00,  0x5C,  0x5C,  0x5C,  0x5C,  0x5C,  0x5D,
    0x5E,  0x5F,  0x60,  0x61,  0x62,  0x63,  0x64,  0x64,  0x64,  0x64,  0x0F,
    -16,   0x00,  0x02,  0x06,  0x04,  0x00,  0x01,  0x09,  0x08,  0x48,  0x00,
    0x04,  -1,    0x02,  0x01,  0x03,  -1,    0x06,  0x07,  0x05,  0x00,  -0x39,
    -0x27, 0x00,  0x00,  -0x08, -0x1C, 0x09,  -0x1A, -5,    -0x1A, 0x2A,  -0x19,
    0x64,  -0x19, -0x70, -0x19, -0x40, -0x19, 0x02,  -0x18, 0x31,  -0x18, 0x60,
    -0x18, -0x23, -0x18, 0x3D,  -0x17, 0x52,  -0x17, 0x63,  -0x17, -99,   -0x17,
    0x0E,  -0x16, 0x0F,  -0x16, 0x4D,  -0x16, 0x71,  -0x16,
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

/* 0x68FE: the frames whose pair B gets a second landing pass (bits 0..3 and
 * 12..15 of the word at 1000:E111). */
#define OBJ_PAIRB_FRAMES 0xF00Fu

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
 * state.  The draw dispatch (0x666B) and the terrain bounce (0xD34E) are both
 * ported (D-97/D-99 closed 2026-11-04); 0xD34E draws nothing.
 */
static const int8_t _obj_proj_curve[25] = {
    0, -4, -3, -3, -2, -2, -2, -1, -1, -1, -1, 0, 0,
    0, 0,  0,  1,  1,  1,  1,  2,  2,  2,  3,  3,
};

/* 0x6531: cmd 9's mode flag (zero in the image; the port keeps it too). */
static uint8_t _obj_marker_mode;

/** [0x6531]: menu_flag_tick (1000:3540) raises it after the goal pan's hold
 * and clears it when [0x6530] falls; 1000:C541's mode arm reads it. */
void object_set_marker_mode(int32_t on) { _obj_marker_mode = on ? 1u : 0u; }
/** @brief Accessor: the marker mode flag. */
int32_t object_marker_mode(void) { return _obj_marker_mode; }

/* the six programs at 0x5B37..0x5B53 (202F image, 12 bytes each). */
static uint8_t _obj_programs[6][12] = {
    { 0x02, 0x01, 0x02, 0x01, 0x04, 0x01, 0x03, 0x04, 0x05, 0x06, 0x04, 0x01 },
    { 0x04, 0x01, 0x03, 0x04, 0x05, 0x06, 0x04, 0x01, 0x03, 0x07, 0x08, 0x06 },
    { 0x04, 0x01, 0x03, 0x07, 0x08, 0x06, 0x04, 0x01, 0x03, 0x05, 0x09, 0x06 },
    { 0x04, 0x01, 0x03, 0x05, 0x09, 0x06, 0x04, 0x01, 0x09, 0x0A, 0x0B, 0x0C },
    { 0x04, 0x01, 0x09, 0x0A, 0x0B, 0x0C, 0x01, 0x01, 0x06, 0x01, 0xD0, 0xD1 },
    { 0x01, 0x01, 0x06, 0x01, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7 },
};

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
 * index), i.e. an out-of-range program id (ignored), skip, return, return —
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
/* 0x5BA4's words as the original's unbounded `word [0x5BA4 + 2*type]` sees
 * them.  Only types 0..2 are the real table; 3..11 run through the 0x5BAA
 * frame table and 12..0x21 through the pause text.  The game's type writers
 * stop at 0x21; a malformed save can exceed it (D-179). */
static const uint16_t _obj_throw_dx_word[0x22] = {
    0x0008, 0xFFF8, 0x0000, 0x0000, 0x0000, 0x6000, 0x969C, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x5005, 0x454C, 0x5341, 0x2045, 0x4553, 0x454C,
    0x5443, 0x5920, 0x554F, 0x2052, 0x504F, 0x4954, 0x4E4F, 0x0D0D, 0x4F43,
    0x544E, 0x4E49, 0x4555, 0x5420, 0x4948, 0x2053, 0x454C,
};
static const uint8_t _obj_drop_frame[8] = { 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x60, 0x9C, 0x96 };

/* DS:0x6EB6 — the per-behaviour *state* byte `test_class_against_6eb6`
 * (0xF2CF) gates on: `MOV BL,[BX+0x3E5E] / AND BL,0x3F / CMP AL,[BX+0x6EB6]`
 * with BX = the behaviour (0..0x3F).  The table proper is 21 bytes —
 * behaviours 5/6/7 -> 1/3/2, 8 -> 0x0F, 9 -> 0xFF, 10..15 -> 0xC2/0x1D/0xA0/
 * 0x00, and 16..20 -> 1 — and the original reads straight on into the next
 * tables (0x6ECB is `_obj_type_overlaps[8]`, then the 0/1 run that follows), so
 * the port keeps the whole 64-byte read window verbatim instead of bounding
 * the index (port_deviations.md used to carry the 21-vs-8 truncation as
 * D-160; closed 2026-11-04, re_history.md). */
static const uint8_t _obj_beh_state[0x40] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x02, 0x0F, 0xFF, 0xC2,
    0x1D, 0xA0, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x60, 0x9C, 0x96, 0xFE, 0xFE, 0xFF,
};

/* DS:0x6908, DS:0x698A, DS:0x6918 — the command dispatch's three tables
 * (1000:E3F6), copied verbatim from the image (file offset = DS offset + 0x106F0):
 *  - _obj_cmd_key      the input bit (low nibble of [0x5ACE]) -> command byte: bit 7
 *                   = no handler, bit 6 = one-shot, low 6 bits = the command
 *                   number; the 4 bytes past the 12-byte table are the 0x6914
 *                   tail word the original reads for indexes 12..15, copied too;
 *  - _obj_cmd_rows     the per-type rows, 32 bytes each (two 16-byte halves, bit 7
 *                   of the input picks the second), ending exactly at 0x6DEA;
 *  - _obj_cmd_handlers the 57 handler addresses, index n = word at 0x6918+2(n-1).
 * Index 2 is 1000:E609 (its tail holds the human-stack push), 10/11 the leaves,
 * 14 the snap — re_map.md, "What selects the stack's verbs". */
static const uint16_t _obj_cmd_handlers[57] = {
    0xE4F8u, 0xE609u, 0xE6FBu, 0xE72Au, 0xE764u, 0xE790u, 0xE7C0u, 0xE802u,
    0xE831u, 0xE860u, 0xE8DDu, 0xE93Du, 0xE952u, 0xE963u, 0xE99Du, 0xEA0Eu,
    0xEA0Fu, 0xEA4Du, 0xEA71u, 0xEAACu, 0xEAE6u, 0xEB5Cu, 0xEB6Au, 0xEB78u,
    0xEBA0u, 0xEBCAu, 0xEBD0u, 0xEC98u, 0xEC99u, 0xECA8u, 0xECB7u, 0xECFEu,
    0xED4Fu, 0xED69u, 0xED83u, 0xED9Du, 0xEDB7u, 0xEE07u, 0xEE3Bu, 0xEF0Au,
    0xEF0Bu, 0xEF0Cu, 0xEF39u, 0xEF66u, 0xEFA1u, 0xEFB9u, 0xEFD1u, 0xEFD4u,
    0xEFD7u, 0xF067u, 0xF078u, 0xF08Du, 0xF0A2u, 0xF0B3u, 0xF0C3u, 0xF0C6u,
    0xF0C9u,
};

static const uint8_t _obj_cmd_key[16] = {
    0x48u, 0x00u, 0x04u, 0xFFu, 0x02u, 0x01u, 0x03u, 0xFFu,
    0x06u, 0x07u, 0x05u, 0x00u, 0xC7u, 0xD9u, 0x00u, 0x00u,
};

static uint8_t _obj_cmd_rows[35][32] = {
    /* type  0 */ {
        0xFFu, 0x01u, 0x05u, 0x05u, 0x05u, 0x02u, 0x04u, 0x04u,
        0x04u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0xFFu, 0x00u, 0x00u, 0x1Du, 0x00u, 0x11u, 0x00u, 0x00u,
        0x00u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  1 */
    {
        0xFFu, 0x01u, 0x06u, 0x06u, 0x06u, 0x02u, 0x03u, 0x03u,
        0x03u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0xFFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u, 0x1Eu,
        0x00u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  2 */
    {
        0x00u, 0x01u, 0x00u, 0x06u, 0x00u, 0x02u, 0x00u, 0x04u,
        0x00u, 0x07u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  3 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0xE8u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0xE8u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  4 */
    {
        0x00u, 0x00u, 0x00u, 0x35u, 0x00u, 0x02u, 0x00u, 0x34u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0xE8u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  5 */
    {
        0x00u, 0x01u, 0x00u, 0x35u, 0x00u, 0x00u, 0x00u, 0x34u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x31u, 0xE8u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  6 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x60u, 0xE8u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x60u, 0xE8u, 0x00u, 0x00u,
    },
    /* type  7 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0xDDu, 0xE8u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0xDDu, 0xE8u, 0x00u, 0x00u,
    },
    /* type  8 */
    {
        0x00u, 0x0Eu, 0x00u, 0x0Du, 0x00u, 0x0Fu, 0x00u, 0x0Cu,
        0x00u, 0x00u, 0x00u, 0x12u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x12u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type  9 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x48u, 0xDBu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x11u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x48u, 0xDBu, 0x00u, 0x00u,
    },
    /* type 10 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x61u, 0xDBu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x61u, 0xDBu, 0x00u, 0x00u,
    },
    /* type 11 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xACu, 0xEAu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xACu, 0xEAu, 0x00u, 0x00u,
    },
    /* type 12 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xE6u, 0xEAu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xE6u, 0xEAu, 0x00u, 0x00u,
    },
    /* type 13 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x5Cu, 0xEBu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x5Cu, 0xEBu, 0x00u, 0x00u,
    },
    /* type 14 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x6Au, 0xEBu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x6Au, 0xEBu, 0x00u, 0x00u,
    },
    /* type 15 */
    {
        0x00u, 0x00u, 0x19u, 0x19u, 0x19u, 0x00u, 0x18u, 0x18u,
        0x18u, 0x1Au, 0x00u, 0x00u, 0x1Fu, 0xDCu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x1Bu, 0x1Bu, 0x1Bu, 0x00u, 0x1Bu, 0x1Bu,
        0x1Bu, 0x00u, 0x00u, 0x00u, 0x1Fu, 0xDCu, 0x00u, 0x00u,
    },
    /* type 16 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x93u, 0xDDu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x10u, 0x00u, 0x00u, 0x93u, 0xDDu, 0x00u, 0x00u,
    },
    /* type 17 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xB7u, 0xECu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xB7u, 0xECu, 0x00u, 0x00u,
    },
    /* type 18 */
    {
        0x00u, 0x27u, 0x27u, 0x22u, 0x25u, 0x25u, 0x25u, 0x21u,
        0x27u, 0x27u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x26u, 0x26u, 0x26u, 0x25u, 0x25u, 0x25u, 0x26u,
        0x26u, 0x26u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type 19 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x0Au, 0xEFu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x0Au, 0xEFu, 0x00u, 0x00u,
    },
    /* type 20 */
    {
        0x00u, 0x27u, 0x27u, 0x24u, 0x27u, 0x25u, 0x27u, 0x23u,
        0x27u, 0x27u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x26u, 0x26u, 0x26u, 0x25u, 0x25u, 0x25u, 0x26u,
        0x26u, 0x26u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type 21 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x0Bu, 0xEFu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x0Bu, 0xEFu, 0x00u, 0x00u,
    },
    /* type 22 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x66u, 0xEFu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x66u, 0xEFu, 0x00u, 0x00u,
    },
    /* type 23 */
    {
        0x00u, 0x31u, 0x31u, 0x31u, 0x25u, 0x2Fu, 0x25u, 0x31u,
        0x31u, 0x31u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x30u, 0x30u, 0x30u, 0x25u, 0x25u, 0x25u, 0x30u,
        0x30u, 0x30u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type 24 */
    {
        0x00u, 0x31u, 0x31u, 0x31u, 0x25u, 0x2Fu, 0x25u, 0x31u,
        0x31u, 0x31u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x30u, 0x30u, 0x30u, 0x25u, 0x25u, 0x25u, 0x30u,
        0x30u, 0x30u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type 25 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x03u, 0x67u, 0xF0u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x03u, 0x67u, 0xF0u, 0x00u, 0x00u,
    },
    /* type 26 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x03u, 0x78u, 0xF0u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x03u, 0x78u, 0xF0u, 0x00u, 0x00u,
    },
    /* type 27 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xC7u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xC7u, 0xD9u, 0x00u, 0x00u,
    },
    /* type 28 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xDBu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xDBu, 0x00u, 0x00u,
    },
    /* type 29 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xDAu, 0xDDu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xDAu, 0xDDu, 0x00u, 0x00u,
    },
    /* type 30 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x7Eu, 0xDEu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x7Eu, 0xDEu, 0x00u, 0x00u,
    },
    /* type 31 */
    {
        0x00u, 0x00u, 0x00u, 0x37u, 0x00u, 0x36u, 0x00u, 0x38u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xC8u, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xC8u, 0xD9u, 0x00u, 0x00u,
    },
    /* type 32 */
    {
        0x00u, 0x00u, 0x00u, 0x37u, 0x00u, 0x36u, 0x00u, 0x38u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xD9u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xD9u, 0x00u, 0x00u,
    },
    /* type 33 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xC9u, 0xF0u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xC9u, 0xF0u, 0x00u, 0x00u,
    },
    /* type 34 */
    {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xD1u, 0xDEu, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xD1u, 0xDEu, 0x00u, 0x00u,
    },
};

/* [0x4234] / [0x4238] / [0x423C] - the selector's three parameters.  The
 * wrappers 1000:E93D/E952 set them and jump, and 1000:E6FB/E764 overwrite the
 * frame/type pair for the walk step, so they are state, not arguments. */
static uint16_t _obj_sel_xoff, _obj_sel_frame, _obj_sel_type;

/* The settle's static data: the per-type gate at 0x6EBE (32 bytes) and the two
 * 32-byte footing offset tables at 0x689E / 0x68BE, copied from the image. */
static const uint8_t _obj_type_gate[32] = {
    0x0F, 0xFF, 0xC2, 0x1D, 0xA0, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t _obj_foot_a[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x2C, 0x2C, 0x2C, 0x2C,
    0x2C, 0x2C, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x50,
};
static const uint8_t _obj_foot_b[32] = {
    0x08, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x2C, 0x2C, 0x2C,
    0x2C, 0x2C, 0x2C, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x50, 0x50,
    0x50, 0x50, 0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* [0x4220] / [0x4222]: the settle's footing, read by 1000:DF2C. */
static uint16_t _obj_foot_x, _obj_foot_y;

/* 1000:E4BD's `CALL word ptr [BX + 0x6916]` — the ported handlers, by address.
 * Returns 0 when the address has no port yet (the caller reports it). */
/* [0x6916] / [0x6917]: the per-call output state the handlers may set; 1000:E4E7
 * copies it to [0x1979] / [0x1988] when it is non-zero. */
static uint8_t _obj_out_state, _obj_out_state_hi;

/* ===================================================================== *
 * The other record types' command handlers (the rest of DS:0x6918)
 * ===================================================================== *
 *
 * `1000:E4BD` calls `[0x6916 + 2*index]`; the index is the byte the type row
 * (`DS:0x698A`, 32 bytes per type: two 16-byte halves, a bit-7 input selecting
 * the second) picks for this input.  Types 0/1/4/5/9/10/15/18/20/23/24/31/32
 * select the handlers below - the ones the earlier passes did not port.
 *
 * They exchange arguments through the dispatcher's own scratch words, the same
 * ones the selector uses: `[0x4230]` (_obj_side), `[0x4234]` (_obj_sel_xoff),
 * `[0x4238]` (_obj_sel_frame), `[0x423C]` (_obj_sel_type).  `[0x4224]`/`[0x4226]`
 * are the level-select character's x/y (`frontend.c`'s charX/charY; `[0x4224]`
 * is what `set_scroll_x` `1000:AA05` centres on) and `[0x4228]` a saved
 * argument.
 */

/* [0x13A4]: the ACTION press's latch - the ONLY one.  pause_toggle raises it
 * (`1000:3538`), poll_input clears it on the release (`1000:85A9`) and masks
 * the ACTION bit while it is set (`1000:859F`); the home-state transition
 * raises it again (`1000:EE01`) and the frame scan clears it (`1000:FC81`).
 * `1000:EE07` refuses to turn while it is set.  It is deliberately NOT set by
 * a charge: during one pause_toggle is blocked by `[0x6F1D]` (`1000:3530`),
 * so the fire column keeps reaching `1000:EE07` (D-80). */
static uint8_t _obj_action_latch;

static uint32_t _obj_sfx; /* the last `1000:83B7` driver argument     */
static uint16_t _obj_pt_x,
    _obj_pt_y; /* [0x4224] / [0x4226]                      */
static uint16_t _obj_aux; /* [0x4228]                                */
static uint8_t _obj_debounce; /* [0x197A]: the previous dispatch code     */

/* [0x6FE6]: the "goal reached" marker 0xEAE6 and 0xF1B2 write; nothing in the
 * port reads it yet (the original's level-end path does). */
static uint8_t _obj_flag6f_e6;

/* DS:0x6E5C - the frame list 0xECB7 walks: twelve ramp words and then 3-word
 * vectors from 0x6E74 (re_formats.md §"the value table"). */
static const uint16_t _obj_anim_list[0x1A] = {
    0x0010, 0x007F, 0x0080, 0x0081, 0x0080, 0x0081, 0x0080, 0x0081, 0x0080,
    0x0081, 0x007F, 0x0010, 0xFFFF, 0x0003, 0xFFFB, 0x0000, 0x0000, 0x0002,
    0xFFF9, 0x0000, 0x0000, 0x0001, 0xFFF7, 0x0000, 0x0000, 0x0000,
};

/* DS:0x6868 - the 20-byte frame table 0xDBCD walks (the frame is a table byte,
 * `+0xC` its cursor), and DS:0x687C - the 4-byte {dx, dy, flag, hold} table
 * 0xDE7E walks. */
static const uint8_t _obj_tab6868[0x14] = {
    0xCD, 0xCE, 0xCD, 0xCE, 0xCD, 0xCE, 0xCF, 0xD0, 0xCF, 0xD0,
    0xCF, 0xD0, 0xD1, 0xD2, 0xD1, 0xD2, 0xD1, 0xD2, 0xD3, 0x00,
};
static const uint8_t _obj_tab687_c[0x20] = {
    0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01,
    0x06, 0xFF, 0x32, 0x01, 0x06, 0xFF, 0x32, 0x01, 0x06, 0x01, 0x06,
    0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0xFF, 0x07, 0x01, 0x07,
};

/* DS:0x6E3C - the byte EBD0 adds to its 1/3 step, indexed by the node's
 * progress >> 3 (the values run 3, 2 x 7, 1 x 12, then 0). */
static const uint8_t _obj_path_scale[0x20] = {
    0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* --- 1000:F6F6: the landing / bounce machine (handlers 0xF067 / 0xF078) ---
 *
 * `CALL 1000:F3ED` gates it on the type-row/0x6EBE rule (the same gate the
 * settle's hook uses).  Then, per tick:
 *
 *   F70D  `+0xA` (a frame counter) counts down; when it hits 0, `+4` (the
 *         frame) advances.  (`DEC` / `JNZ`, not `JNS`.)
 *   F715  `+0xC` is the animation index; its step comes from the byte curve at
 *         0x6EF3 (index clamped at 0x18, where the step is a flat 3), and the
 *         step accumulates in `+0xE` - the vertical speed.
 *   F72E  the record's `+0x10` is a horizontal speed: x += vx, y += vy.
 *   F740  probe the map at (x + vx + 8, y + vy + 0x18): the cell at that row and
 *         one row above it decide everything.
 *
 * The branches:
 *   F76C  a 0xF0/0xF1/0xF2 cell one row above is a platform: snap x to the tile
 *         grid (`+8`, `& ~0xF`, `-4`, +8 unless 0xF2), y to `rowY - 0x1C`, frame
 *         0xCD, type 0x1C, a nine-frame hold, effect 6.
 *   F7BB  while the record is moving horizontally, compare the feet cell with
 *         the byte the *type* selects in the table at 0x6F0B (type - 0x19 as a
 *         word offset, so types below 0x19 wrap back into 0x6EF3's tail - the
 *         table is really `0x6F0B + (type - 0x19)` in the segment); if that is
 *         the feet cell and the cell below has a page byte of 0, land.
 *   F7F3  the page byte under the feet: 0 -> F834, positive -> F87F, negative ->
 *         retry one column right (+0x10) and F8EC when that is negative too.
 *   F834  the zero case: when the cell above equals the type's 0x6F0B byte, land;
 *         otherwise the page byte of the cell above decides (its bit 7 -> F87F,
 *         else the probe moves one column left and falls through to F87F).
 *   F87F  the snap test: CX is the *row-aligned probe row* (set at F758), so
 *         `rowY + page - 0x18 - (y + vy)` is the surface's offset inside the row;
 *         within 3 px the record snaps to it (`bx -= d`) and lands.
 *   F8A1  the vertical speed: negative -> F8D2 (land with the scratch pair); >=
 *         0x30 -> F8AD (frame 0x28, type 0x16, effect 4); otherwise F8C8 arms the
 *         camera's offset walk (`[0x2D8E] = vx >> 3`) and lands with effect 5.
 *   F8EC  the item word under (x + [0x4238], y + vy + 0x18) decides between the
 *         two landings: the bit for the fine x (a 32-bit shift seeded from the
 *         sign of `+0x10`) masks it; nonzero -> F975, zero -> F966.
 *   F966  commit (ax, bx) as the position; a y >= 0x140 falls into the hard
 *         landing.
 *   F975  commit only y, clear `+0x10`, and raise `+0xC` to at least 0x10.
 *
 * `[0x4230]`/`[0x4234]` are the caller's landing frame/type pair (0xF067 sets
 * 0/0 with `[0x4238] = 0x10`; 0xF078 sets 8/1 with 0), `[0x4238]` its x offset
 * for the item-word probe, `[0x423C]` the scratch page byte / item word, and
 * `[0x422C]`/`[0x422E]` the 32-bit mask.
 */
static const int8_t _obj_fall_step[0x19] = {
    /* 0x6EF3, 0x18 + the clamp entry */
    -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0,
    0,  1,  1,  1,  1,  1,  2,  2, 0, 0, 0, 0,
};
/* 0x6EF2..0x6F14: the feet-cell comparison byte, indexed by the *type* (the
 * code reads 0x6F0B + (type - 0x19), which for types 0..0x18 wraps back into
 * 0x6EF3's tail; the flat table below is the same bytes).  Entries 0x19/0x1A
 * are the image's pre-load bytes only: `level_setup_screen` writes the level's
 * field-K second word over 0x6F0B/0x6F0C (1000:4E99), so those two are read
 * through `_obj_type_tile_byte` instead. */
static const uint8_t _obj_type_tile[35] = {
    0x96, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x02, 0x00, 0x00, 0x49, 0xFA, 0x64, 0xFA, 0x78, 0xFA, 0x78, 0xFA,
};

/* 0x5B37..0x5B56: the six overlapping programs and [0x5B56].  The port keeps
 * six 12-byte copies; the original region is the 0x20-byte union, so save the
 * union and overlay each program's cursor byte at its table start. */
static const uint8_t _obj_program_base[0x20] = {
    0x02, 0x01, 0x02, 0x01, 0x04, 0x01, 0x03, 0x04, 0x05, 0x06, 0x04,
    0x01, 0x03, 0x07, 0x08, 0x06, 0x04, 0x01, 0x03, 0x05, 0x09, 0x06,
    0x04, 0x01, 0x09, 0x0A, 0x0B, 0x0C, 0x01, 0x01, 0x06, 0x01,
};
static const uint32_t _obj_program_start[6] = { 0u, 4u, 10u, 16u, 22u, 28u };
static uint8_t _obj_program_region[0x20];

/* 0x67FF: the pending-move (offset, divider) table; the word after each
 * entry is the next offset, so 0xFFFF ends the run and resets the cursor. */
static const uint16_t _pend_off[18] = {
    1, 2, 3, 5, 2, 3, 5, 6, 2, 1, 2, 3, 2, 3, 4, 5, 2, 0xFFFF,
};
static const uint16_t _pend_wait[17] = {
    0x28, 0x14, 0x0F, 0x05, 0x0A, 0x14, 0x1E, 0x19, 0x08,
    0x0C, 0x0F, 0x12, 0x32, 0x17, 0x3C, 0x0A, 0x04,
};

/** @brief Accessor: one pending event's offset. */
uint16_t object_pend_off_for_probe(uint32_t i)
{ return i < sizeof _pend_off / sizeof _pend_off[0] ? _pend_off[i] : 0u; }
/** @brief Accessor: one pending event's wait. */
uint16_t object_pend_wait_for_probe(uint32_t i)
{ return i < sizeof _pend_wait / sizeof _pend_wait[0] ? _pend_wait[i] : 0u; }

/** [0x5B56] is shared with the status bar (its action row's group), so every
 * write goes through here. */
static void _obj_set_program_index(uint32_t index)
{
    if (index < 1u || index > 6u) return;
    _obj_program_index = index;
}

/** the acting record: get_object_ptr (1000:8DF4) = the 29-word table baked at
 * DS:0x3E24, indexed by [0x3E22] — i.e. _obj_array[_obj_entity]. */
static uint8_t *_obj_actor(void)
{ return _obj_array + _obj_entity * OBJ_STRIDE; }

/** @brief Accessor: the static record count. */
uint32_t object_static_slots(void) { return OBJ_STATIC; }

/** The array's initial state (`1000:4CAB`): every one of the 29 records gets
 * `x = y = 0xFFFF` and `+6 = 0x80` (flags bit 7 = free, class 0), the rest 0;
 * `[0x3E20]`/`[0x3E22]`/`[0x32B9]` are cleared and the recipes re-armed.
 * The live level confirms it: slots 4..7 read `x = y = 0xFFFF, flags = 0x80`. */
void object_init(void)
{
    memset(_obj_array, 0, sizeof _obj_array);
    for (uint32_t i = 0; i < OBJ_SLOTS; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        wr_le_u16(r + 0u, 0xFFFFu);
        wr_le_u16(r + 2u, 0xFFFFu);
        wr_le_u16(r + 6u, 0x0080u);
    }
    memset(_obj_slot_class, 0, sizeof _obj_slot_class);
    _obj_current = -1;
    _obj_player = -1;
    _obj_live_count = OBJ_LIVE0;
    _obj_placed_humans = 0u;
    _obj_entity = 0u; /* [0x3E22] cleared (1000:4CB7)      */
    _obj_bg_count = 0u; /* [0x32B9] cleared (1000:4CBA)      */
    _obj_pending = 0u; /* [0x1979]                          */
    _obj_cursor = 1u; /* [0x1988]                          */
    _obj_set_program_index(1u); /* [0x5B56]                          */
    _obj_cmd_timer = 0u; /* [0x6DEA]                          */
    _obj_stream_count = 0u; /* [0x6DEC]                          */
    _obj_stream_ptr = _obj_anim_stream; /* [0x6DEE] = 0x6DF2                 */
    _obj_anim_div = 0; /* [0x66B1]                          */
    _pend_tag = _pend_count = _pend_slot = _pend_index = _pend_cursor = 0u;
    _pend_div = 0;
    _obj_stack_mask[0] = _obj_stack_mask[1] = 0u;
    _obj_fault = 0u;
    memset(_obj_register, 0xFF, sizeof _obj_register); /* 0x6F22, all free  */
    memset(_obj_span, 0xFF,
           sizeof _obj_span); /* 0x6F92: 0x38 bytes at 0x60FF */
    memset(_obj_span_top, 0, sizeof _obj_span_top);
    _obj_goal_flag = 0u; /* [0x6530] */
    for (uint32_t i = 0; i < 4u; i++) _obj_uses[i] = _obj_recipes[i][0];
    object_ter_slot_init(); /* 1000:4CC3/4CC6: both slots off */
}

/** @brief Accessor: the placed human count. */
uint32_t object_placed_humans(void) { return _obj_placed_humans; }

/** @brief Accessor: one record slot. */
uint8_t *object_slot(uint32_t i)
{ return i < OBJ_SLOTS ? _obj_array + i * OBJ_STRIDE : NULL; }

/** @brief Accessor: the record slot count. */
uint32_t object_slots(void) { return OBJ_SLOTS; }

/** @brief Accessor: the live record count. */
uint32_t object_live_count(void) { return _obj_live_count; }

/** @brief Accessor: one slot's class byte. */
uint8_t object_slot_class(uint32_t slot) { return _obj_class_byte_at(slot); }

/** @brief Test seam: resolve a link offset. */
uint8_t *object_link_for_probe(uint32_t off) { return _obj_link_record(off); }

/** @brief Accessor: one span entry. */
uint8_t *object_span_at_for_probe(uint32_t i)
{ return i < 2u ? (uint8_t *)_obj_span[i] : NULL; }

/** test seam: the stripe's seed row, which 1000:FC81 sets when it seeds a new
 * entry (`+6 == 0`, 1000:FD6C). */
void object_span_seed_for_probe(uint32_t i, uint32_t top)
{
    if (i < 2u) _obj_span_top[i] = (uint16_t)top;
}

/** the throw's 0x5BA4 word and a recipe's parameter, for the tests */
int32_t object_throw_dx_for_probe(uint32_t type)
{ return type < 0x22u ? (int16_t)_obj_throw_dx_word[type] : 0; }
/** @brief Accessor: one recipe parameter. */
uint16_t object_recipe_param_for_probe(uint32_t recipe, uint32_t k)
{
    return recipe < 4u && k < 4u ? rd_le_u16(_obj_recipes[recipe] + 4u + 2u * k)
                                 : 0u;
}
/** the tests' setter for [0x3E5E + slot] (the behaviour bytes). */
void object_set_slot_class_for_probe(uint32_t slot, uint8_t cls)
{
    if (slot < sizeof _obj_slot_class) _obj_slot_class[slot] = cls;
}

/** @brief Test seam: set one slot's class. */
void object_set_slot_class(uint32_t slot, uint32_t cls)
{
    if (slot < sizeof _obj_slot_class) _obj_slot_class[slot] = (uint8_t)cls;
}

/** the 0x6EB6 read window, for the tests (D-160's closure): behaviour -> the
 * state byte `test_class_against_6eb6` compares against. */
uint8_t object_beh_state_for_probe(uint32_t beh)
{ return _obj_beh_state[beh & 0x3Fu]; }

/** @brief Accessor: the current record index. */
int32_t object_current(void) { return _obj_current; }

/** @brief Accessor: set the current record. */
void object_set_current(int32_t slot)
{ _obj_current = (slot >= 0 && (uint32_t)slot < OBJ_SLOTS) ? slot : -1; }

/** @brief Accessor: the player record index. */
int32_t object_player(void) { return _obj_player; }

/** @brief Accessor: set the player record. */
void object_set_player(int32_t slot)
{ _obj_player = (slot >= 0 && (uint32_t)slot < OBJ_SLOTS) ? slot : -1; }

/* --- object_test_dispatch's predicates (1000:35D6) ---------------------
 *
 * The six handlers are reachable from the level's goal-test word at
 * [0x17F2] through the CS word table at 0x35F4.  All six are ported here
 * (0x3600 is object_low_phase_match below); the earlier "[0x1996] byte" reading
 * was wrong (re_history.md).
 *
 * 0xF761 is NOT a random generator (the earlier docs called it that): it adds
 * the record's x/y to its arguments and turns them into a **map address** —
 * `cell = 32*((y+0xC)/16) + (x+8)/16` — returning the map byte there and a
 * tile-row pointer in BP.  So `pred_map_cell_equals` compares the map cell
 * under the current object with the test's byte.
 *
 * The convention is the original's: -1 = true, 0 = false.  Every handler runs
 * against SI = the current record, i.e. what `object_current()` selects. */

/** 1000:3640 — -1 when NO record's type is 0x1D. */
int32_t object_none_type1d(void)
{
    for (uint32_t i = 0; i < OBJ_STATIC; i++)
        if (rd_le_u16(_obj_array + i * OBJ_STRIDE + 8u) == 0x1Du) return 0;
    return -1;
}

/** 1000:3659 — the current record's +7 indexes the source-slot table 0x3E5E;
 * a zero slot is false, and the slot's low six bits are compared with the
 * test's byte. */
int32_t object_class_matches(uint32_t want)
{
    uint32_t b;

    b = _obj_actor()[7u];
    if (!b) return 0;
    return ((_obj_class_byte_at(b) & 0x3Fu) == (uint8_t)want) ? -1 : 0;
}

/** 1000:3678 — the current record's type equals the test's word. */
int32_t object_type_equals(uint32_t type)
{ return rd_le_u16(_obj_actor() + 8u) == type ? -1 : 0; }

/* 0xF761's map addressing for a record: `cell = 32*((y+0xC)&0xFFF0)/16 +
 * ((x+8)>>4)` — the row is the record's y rounded down to the 16-px grid,
 * doubled into the 32-column row stride.  0xFFFF when it falls outside the
 * map (the original would read whatever follows it). */
/** The map buffer as `1000:3138` builds it: 640 map bytes, a **32-byte gap the
 * loader never writes** at `+0x280..+0x29F`, then a **full second copy of the
 * map at `+0x2A0`** (1000:3146 `ADD DI,0x20` / 1000:3149 `REP MOVSB`).  The
 * routines that probe a neighbour row (`1000:D9E8`'s `[DI-0x20]`/`[DI+0x20]`,
 * `1000:E2B5`, `1000:E039`) address that tail through this.  Before the buffer
 * and past the second copy are the port's bounds (port_deviations.md D-100). */
static uint32_t _obj_map_buffer_byte(int32_t idx)
{
    const uint8_t *map = level_map();

    if (!map) return 0u;
    if (idx < 0) return 0xFFu;
    if (idx < 640) return map[idx];
    if (idx < 672) return 0u;
    if (idx < 1312) return map[idx - 672];
    return 0u;
}

/** 1000:E361: the map cell at (x + 8, y + 0xC).  Returns the cell's *index* so
 * the callers that read a neighbour (`1000:3600`'s `[DI+0x20]`, one map row
 * below) can address it; 0xFFFF when the point is outside the map. */
static uint32_t _obj_map_cell_index(const uint8_t *rec)
{
    uint32_t x = (uint32_t)rd_le_u16(rec + 0u) + 8u;
    uint32_t y = (uint32_t)rd_le_u16(rec + 2u) + 0xCu;
    uint32_t cell;

    if (!level_map() || y < 16u) return 0xFFFFu;
    cell = 32u * (y / 16u) + (x / 16u);
    return cell < 640u ? cell : 0xFFFFu;
}

/** @brief Accessor: the record's map cell. */
static uint32_t _obj_map_cell(const uint8_t *rec)
{
    const uint8_t *map = level_map();
    uint32_t cell = _obj_map_cell_index(rec);

    return cell < 640u && map ? map[cell] : 0xFFFFu;
}

/** The map cell under a record, for the score (55FA calls 0xF761 with AX = 8,
 * BX = 0xC on each of the eight static records): 0xFFFF outside the map. */
uint32_t object_record_map_cell(uint32_t slot)
{
    if (slot >= OBJ_STATIC) return 0xFFFFu;
    return _obj_map_cell(_obj_array + slot * OBJ_STRIDE);
}

/** 1000:36AB — the record's type is <= 2 and the map cell under it equals the
 * test's byte. */
int32_t object_map_cell_equals(uint32_t want)
{
    if (rd_le_u16(_obj_actor() + 8u) > 2u) return 0;
    return _obj_map_cell(_obj_actor()) == (uint16_t)want ? -1 : 0;
}

/** 1000:3688 — the entity table (count 0x64C2, records 0x64C4, stride 0x10):
 * -1 when there are none or every one's +4 is 5 (the knocked/finished state),
 * 0 as soon as one is not. */
int32_t object_entities_finished(void)
{
    uint32_t n = entity_count();

    for (uint32_t i = 0; i < n; i++) {
        const Entity *e = entity_at(i);
        if (e && e->handler != 5u) return 0;
    }
    return -1;
}

/** --- pred_type_low_phase_match (1000:3600) ----------------------------
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
 * record - see `object_current`.
 */
int32_t object_low_phase_match(void)
{
    static uint32_t lastFrame = ~0u;

    if (rd_le_u16(_obj_actor() + 8u) > 2u) return 0;
    if (lastFrame == _obj_frame) return 0; /* once per frame */
    lastFrame = _obj_frame;
    {
        /* 1000:361F..3629: the feet cell, then the cell one map row *below* it
         * (`[DI+0x20]`, which is `map[cell + 32]`).  Either being the goal tile
         * 0xA8 counts; the earlier reading of this as a `[0x1996]` byte was
         * wrong - see re_history.md. */
        const uint8_t *map = level_map();
        uint32_t cell = _obj_map_cell_index(_obj_actor());
        int32_t goal = (cell < 640u && map && map[cell] == 0xA8u);

        /* The original's buffer has a 32-byte gap at +0x280..+0x29F and the full
         * map again at +0x2A0 (1000:3138), so the last row's below-cell reads the
         * gap (0x00 = tile 0) — the port's row-0 wrap is D-100. */
        if (!goal && cell < 640u && map) {
            uint32_t below = cell + 32u;

            if (below >= 640u) below -= 640u;
            if (map[below] == 0xA8u) goal = 1;
        }
        if (!goal) {
            _low_phase_count = 0u; /* 1000:362C */
            return 0;
        }
    }
    if (++_low_phase_count != 0x8Cu) return 0;
    return -1;
}

/** 0x54F4: the whole 768-byte ramp := one 6-bit colour.  The fill's register
 * order is (BL, AH, AL) = (R, G, B), hence the red call's AX = 0, BL = 0x3F. */
static void _claim_fill_ramp(uint8_t *ramp, uint32_t r, uint32_t g, uint32_t b)
{
    for (uint32_t i = 0; i < 256u; i++) {
        ramp[i * 3u] = (uint8_t)r;
        ramp[i * 3u + 1u] = (uint8_t)g;
        ramp[i * 3u + 2u] = (uint8_t)b;
    }
}

/** The scan at 1000:3A91..1000:3AD9, for the acting record `actor`. */
int32_t object_find_neighbour(int32_t actor)
{
    const uint8_t *cur;
    int32_t cx, cy;

    if (actor < 0 || (uint32_t)actor >= OBJ_STATIC) return -1;
    cur = _obj_array + actor * OBJ_STRIDE;
    cx = (int16_t)rd_le_u16(cur + 0u);
    cy = (int16_t)rd_le_u16(cur + 2u);

    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        const uint8_t *r = _obj_array + i * OBJ_STRIDE;
        int32_t dx, dy;

        if ((int32_t)i == actor) continue; /* 1000:3A9C */
        if (rd_le_u16(r + 8u) > 2u) continue; /* type <= 2 */
        dx = (int32_t)(int16_t)rd_le_u16(r + 0u) - cx;
        dy = (int32_t)(int16_t)rd_le_u16(r + 2u) - cy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 0x20 || dy > 8) continue;
        if (r[6] & 0x80u) continue; /* active    */
        return (int32_t)i;
    }
    return -1;
}

/** @brief Begin the claim animation for one recipe. */
int32_t object_claim_begin(uint32_t recipe, int32_t actor)
{
    int32_t n;
    uint8_t *rec, *live;
    uint32_t cls;

    if (actor < 0 || (uint32_t)actor >= OBJ_STATIC) return -1;
    if (recipe >= 4u) return -1;
    if (rd_le_u16(_obj_array + actor * OBJ_STRIDE + 8u) > 2u) return -1;
    if (!_obj_uses[recipe]) return -1; /* 1000:3A89: the use counter */

    n = object_find_neighbour(actor);
    if (n < 0) return -1;

    rec = _obj_array + n * OBJ_STRIDE;
    cls = rec[7u];
    if (cls) { /* 1000:3AE2..1000:3B00 */
        _obj_slot_class[cls] = 0u; /* 0x3E5F + cls - 1 */
        live = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE;
        wr_le_u16(live + 6u, 0xFFFFu); /* 0x3F17 + 0x12*(cls-1) */
    }
    object_set_current(n); /* 1000:3B03 */
    wr_le_u16(rec + 4u, 0x0103u); /* 1000:3B07 */
    wr_le_u16(rec + 6u, 0u); /* 1000:3B0D */
    wr_le_u16(_obj_array + actor * OBJ_STRIDE + 4u, 0x0020u); /* 1000:3B13 */
    statusbar_set_active(0); /* 1000:3B23: [0x5B12] = 0 */
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
    for (uint32_t k = 0; k < 4u; k++)
        _claim_params[k] = rd_le_u16(_obj_recipes[recipe] + 4u + 2u * k);

    _claim_neighbour = n;
    _claim_blinks = 10u; /* 1000:3BAC */
    _claim_timer = 0u;
    _claim_fill_ramp(_claim_red, 0x3Fu, 0u, 0u); /* 1000:3B4F: AX=0, BL=0x3F */
    _claim_fill_ramp(_claim_white, 0x3Fu, 0x3Fu,
                     0x3Fu); /* 1000:3B7A: AX=0x3F3F   */
    _claim_phase = CLAIM_TO_RED;
    palette_build_fade(1u, 0x7Fu, 5u, _claim_red + 3u); /* 1000:3B56 */
    return n;
}

/** @brief Accessor: the claim is running. */
int32_t object_claim_active(void) { return _claim_phase != CLAIM_OFF; }

/** @brief Accessor: the claim's pan phase. */
int32_t object_claim_pan_active(void) { return _claim_phase == CLAIM_PAN; }

/** One frame of the sequence; the fades advance through `palette_tick`, which
 * the level's own tick drives.  Every `palette_fade_active()` wait below is the
 * original's `0x9F8B` (spin while the fade runs). */
void object_claim_tick(const uint8_t *levelPal)
{
    uint8_t *nrec = _claim_neighbour >= 0
                        ? _obj_array + _claim_neighbour * OBJ_STRIDE
                        : NULL;

    switch (_claim_phase) {
    case CLAIM_OFF: return;
    case CLAIM_TO_RED: /* 1000:3B56 */
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_RED_BACK;
        palette_build_fade(1u, 0x7Fu, 0x1Eu, levelPal + 3u); /* 1000:3B68 */
        break;
    case CLAIM_RED_BACK: /* 1000:3B68 */
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_TO_WHITE;
        palette_build_fade(1u, 0x7Fu, 5u, _claim_white + 3u); /* 1000:3B82 */
        break;
    case CLAIM_TO_WHITE: /* 1000:3B91: 1000:3D04 */
        if (palette_fade_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_PAN; /* two frames */
        break;
    case CLAIM_PAN: /* 1000:3D04, two frames */
        /* 1000:3D04 presents the world twice (5810, 5d56, 3d5e); on the first
         * pass 1000:3D41 hands min(player, claimed) to 1000:3DC0, which SAVES a
         * 64x18-byte page strip at that point to A000:0xF000 — a scratch save
         * for the sequence, not a scroll: no [0x0]/[0x2] write exists anywhere
         * in it.  The camera stays where the follower left it (frozen above). */
        if (++_claim_timer < 2u) break;
        _claim_timer = 0u;
        _claim_phase = CLAIM_WHITE_BACK;
        palette_build_fade(1u, 0x7Fu, 0x19u, levelPal + 3u); /* 1000:3B97 */
        break;
    case CLAIM_WHITE_BACK: /* 1000:3BA9 */
        if (palette_fade_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_FRAME;
        break;
    case CLAIM_FRAME:
        if (++_claim_timer <= 1u) break; /* 1000:3BA9's 0x9F56 */
        _claim_timer = 0u;
        _claim_phase = CLAIM_BLINK_WHITE;
        palette_build_fade(0x80u, 0x40u, _claim_blinks,
                           _claim_white); /* 1000:3BB3 */
        break;
    case CLAIM_BLINK_WHITE: /* 1000:3BB3 */
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_BLINK_LEVEL;
        palette_build_fade(0x80u, 0x40u, _claim_blinks,
                           levelPal + 0x80u * 3u); /* 1000:3BC3 */
        break;
    case CLAIM_BLINK_LEVEL: /* 1000:3BC3 */
        if (palette_fade_active()) return;
        if (--_claim_blinks == 0u) {
            _claim_timer = 0u;
            _claim_phase = CLAIM_ANIM;
            break;
        }
        _claim_phase = CLAIM_BLINK_WHITE;
        palette_build_fade(0x80u, 0x40u, _claim_blinks, _claim_white);
        break;
    case CLAIM_ANIM: /* 1000:3BD6..1000:3BF6 */
        if (++_claim_timer % 9u == 0u) /* the 1000:3BE9 frame++ */
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
            wr_le_u16(nr + 4u, (uint16_t)_claim_sprite);
            wr_le_u16(nr + 6u, 0u);
            wr_le_u16(nr + 8u, (uint16_t)_claim_params[0]);
            wr_le_u16(nr + 0xAu, (uint16_t)_claim_params[1]);
            wr_le_u16(nr + 0xCu, (uint16_t)_claim_params[2]);
            wr_le_u16(nr + 0xEu, (uint16_t)_claim_params[3]);
            _obj_slot_class[_obj_live_count - 7u] = (uint8_t)_claim_behav;
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

                _obj_build_shade(_claim_sprite, shade);
                level_spark_begin(rd_le_u16(nr + 0u), rd_le_u16(nr + 2u),
                                  shade);
            }
        }
        _claim_phase = CLAIM_HOLD;
        break;
    case CLAIM_HOLD: /* 1000:3C7D: 0x28+1 */
        if (++_claim_timer < 41u) break;
        _claim_timer = 0u;
        _claim_phase = CLAIM_WHITE_OUT;
        palette_build_fade(1u, 0xBFu, 5u, _claim_white + 3u); /* 1000:3C90 */
        break;
    case CLAIM_WHITE_OUT: /* 1000:3C88 */
        if (palette_fade_active()) return;
        _claim_timer = 0u;
        _claim_phase = CLAIM_FRAME2;
        break;
    case CLAIM_FRAME2: /* 1000:3C9F */
        if (++_claim_timer <= 1u) break; /* one 0x9F56 */
        _claim_timer = 0u;
        wr_le_u16(nrec + 6u, 0xFFFFu); /* 50A6: removed */
        statusbar_set_active(1); /* 50B0: [0x5B12] */
        _claim_phase = CLAIM_FLIP;
        break;
    case CLAIM_FLIP: /* 1000:3CB5: LOOP CX=2 */
        if (_claim_timer == 0u)
            level_set_updates(1); /* 1000:3CAB: [0xC] = 0xFF */
        if (++_claim_timer < 2u) break;
        _claim_timer = 0u;
        _claim_phase = CLAIM_SETTLE;
        palette_build_fade(1u, 0xBFu, 0x28u, levelPal + 3u); /* 50DE */
        break;
    case CLAIM_SETTLE: /* 1000:3CDE */
        if (palette_fade_active()) return;
        _claim_phase = CLAIM_OFF;
        _claim_neighbour = -1;
        break;
    default: _claim_phase = CLAIM_OFF; break;
    }
}

/** 1000:524B (tile 0xAA): the human, into the first free static slot. */
static int32_t _obj_place_human(uint32_t col, uint32_t row)
{
    uint32_t y = row * 16u + _obj_style_nudge(NULL, col, row) - 7u;

    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        if (!(r[6] & 0x80u)) continue;
        wr_le_u16(r + 0u, (uint16_t)(col * 16u));
        wr_le_u16(r + 2u, (uint16_t)y); /* 0x524B: nudge, then y = row*16 - 7 */
        wr_le_u16(r + 4u, 0x0010u);
        wr_le_u16(r + 6u, 0u);
        wr_le_u16(r + 8u, 0x0002u);
        _obj_placed_humans++;
        return (int32_t)i;
    }
    return -1;
}

/** 1000:527D (tile 0xAB): the spear, appended at the live end. */
static void _obj_append_spear(uint32_t col, uint32_t row)
{
    uint8_t *r;
    uint32_t probe;
    int32_t bx;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u + _obj_style_nudge(NULL, col, row)));
    wr_le_u16(r + 4u, 0x0060u);
    wr_le_u16(r + 6u, 0u);
    wr_le_u16(r + 8u, 0xFFFFu);
    wr_le_u16(r + 0xAu, 0u);
    _obj_slot_class[_obj_live_count - 7u] = 5u; /* [0x3E5F + count-8] = 0x05 */
    _obj_live_count++;
    /* 1000:527D ends JMP 0x5639: the shared ground snap. */
    bx = _obj_step_probe_at(r, 8, 0x10, &probe);
    if (bx >= 0) wr_le_u16(r + 2u, (uint16_t)(bx - 0x10));
}

/** 1000:54D9 (0xBB): {x, y, +4 = 0x26, +6 = 0, +8 = the map cell - 1},
 * behaviour 9. */
static void _obj_spawn_bb(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u + _obj_style_nudge(NULL, col, row)));
    wr_le_u16(r + 4u, 0x0026u);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, (uint16_t)(row * OBJ_COLS + col));
    _obj_slot_class[_obj_live_count - 7u] = 9u;
    _obj_live_count++;
}

/** 1000:54FC (0xBE): {x, y, +4 = 0x82, +6 = 0, +8 = 0, +0xA = x, +0xE = 1},
 * behaviour 2 - and it remembers the record for 0xBF. */
static void _obj_spawn_be(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u + _obj_style_nudge(NULL, col, row)));
    wr_le_u16(r + 4u, 0x0082u);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, 0x0000u);
    wr_le_u16(r + 0xAu, (uint16_t)(col * 16u));
    wr_le_u16(r + 0xEu, 0x0001u);
    _obj_slot_class[_obj_live_count - 7u] = 2u;
    _obj_join_slot =
        (int32_t)(_obj_live_count + 1u); /* the record, not the count */
    _obj_live_count++;
}

/** 1000:552B (0xBF): the join half - written into the record 0xBE made. */
static void _obj_join_bf(uint32_t col)
{
    if (_obj_join_slot < 0) return;
    wr_le_u16(_obj_array + (uint32_t)_obj_join_slot * OBJ_STRIDE + 0xCu,
              (uint16_t)(col * 16u));
}

/** 1000:5535 / 1000:553D (0xC0 / 0xC1): 0xC0 runs the human handler and keeps
 * its slot as the player's record ([0x180A]); 0xC1 appends {x, y-0x10,
 * +4 = 0x100}, behaviour 0. */
static void _obj_spawn_c0(uint32_t col, uint32_t row)
{
    int32_t slot = _obj_place_human(col, row);

    if (slot >= 0) _obj_player = slot;
}

/** @brief Spawn the C1 decoration at (col, row). */
static void _obj_spawn_c1(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u,
              (uint16_t)(row * 16u + _obj_style_nudge(NULL, col, row) - 0x10u));
    wr_le_u16(r + 4u, 0x0100u);
    wr_le_u16(r + 6u, 0x0000u);
    _obj_slot_class[_obj_live_count - 7u] = 0u;
    _obj_live_count++;
}

/** 1000:5310 / 1000:532F / 1000:5338 / 1000:5357: the four data-only tiles. */
static void _obj_write_pair(uint32_t tile, uint32_t x, uint32_t y)
{
    switch (tile) {
    case 0xAFu:
        _obj_pair_a[0] = (uint16_t)x;
        _obj_pair_a[1] = (uint16_t)y;
        _obj_pair_a[2] = (uint16_t)x;
        _obj_pair_a[3] = (uint16_t)y;
        _obj_pair_a[4] = _obj_pair_a[5] = _obj_pair_a[6] = _obj_pair_a[7] = 0u;
        break;
    case 0xB0u:
        _obj_pair_b[0] = (uint16_t)x;
        _obj_pair_b[1] = (uint16_t)y;
        break;
    case 0xB1u:
        _obj_pair_c[0] = (uint16_t)x;
        _obj_pair_c[1] = (uint16_t)y;
        _obj_pair_c[2] = (uint16_t)x;
        _obj_pair_c[3] = (uint16_t)y;
        _obj_pair_c[4] = _obj_pair_c[5] = _obj_pair_c[6] = _obj_pair_c[7] = 0u;
        break;
    default:
        _obj_pair_d[0] = (uint16_t)x;
        _obj_pair_d[1] = (uint16_t)y;
        break;
    }
}

/** 1000:53BA (0xB6): a placed object in the first *static* slot - frame 0xFE,
 * type 0x1D, +0xA = 0xFA, +0xC = the record-area pointer - yet the live count
 * still grows.  The pointer is the next live record, stored in the same
 * `record - _obj_array + 4` form as the other links. */
static void _obj_spawn_b6(uint32_t col, uint32_t row)
{
    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;

        if (!(r[6] & 0x80u)) continue;
        wr_le_u16(r + 0u, (uint16_t)(col * 16u));
        wr_le_u16(r + 2u, (uint16_t)(row * 16u +
                                     _obj_style_nudge(NULL, col, row) - 0x10u));
        wr_le_u16(r + 4u, 0x00FEu);
        wr_le_u16(r + 6u, 0x0000u);
        wr_le_u16(r + 8u, 0x001Du);
        wr_le_u16(r + 0xAu, 0x00FAu);
        wr_le_u16(
            r + 0xCu,
            (uint16_t)(((_obj_live_count + 1u) * OBJ_STRIDE + 4u) & 0xFFFFu));
        _obj_slot_class[_obj_live_count - 7u] = 0u;
        _obj_live_count++;
        return;
    }
}

/** 1000:53FD (0xB7): TWO appended records; the first carries the only computed
 * class (`+6 = (count - 6) << 8`), behaviour 8, and the 0xB6 neighbour patch:
 * cells -5..-2 -> frame 0x116 / +0xA += 2, cells 0..+3 -> 0x11A / +3, none ->
 * the initial 0xD4 / 0.  The left group is tested first.  The second record is
 * empty, behaviour 0x85. */
static void _obj_spawn_b7(const uint8_t *map, uint32_t col, uint32_t row)
{
    uint8_t *r;
    uint32_t cell;
    int32_t variant = 0; /* 0 none, 1 left, 2 right */

    if (_obj_live_count + 2u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    cell = row * OBJ_COLS + col;
    if (map) {
        static const int32_t left[4] = { -5, -4, -3, -2 };
        static const int32_t right[4] = { 0, 1, 2, 3 };

        for (uint32_t k = 0; k < 4u && !variant; k++) {
            int64_t c = (int64_t)cell + left[k];

            if (c >= 0 && c < 640 && map[c] == 0xB6u) variant = 1;
        }
        for (uint32_t k = 0; k < 4u && !variant; k++) {
            int64_t c = (int64_t)cell + right[k];

            if (c >= 0 && c < 640 && map[c] == 0xB6u) variant = 2;
        }
    }
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u,
              (uint16_t)(row * 16u + _obj_style_nudge(NULL, col, row) - 8u));
    wr_le_u16(r + 4u, variant ? (variant == 1u ? 0x0116u : 0x011Au) : 0x00D4u);
    wr_le_u16(r + 6u, (uint16_t)(((_obj_live_count - 6u) << 8) & 0xFF00u));
    wr_le_u16(r + 8u, 0x0000u);
    wr_le_u16(r + 0xAu, variant == 1u ? 2u : (variant == 2u ? 3u : 0u));
    _obj_slot_class[_obj_live_count - 7u] = 8u;
    _obj_live_count++;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    memset(r, 0, OBJ_STRIDE);
    _obj_slot_class[_obj_live_count - 7u] = 0x85u;
    _obj_live_count++;
}

/** 1000:5481 (0xB8): one appended record, frame 0xAF, behaviour 3, and
 * `+0xE = SI - 1` - the map cell pointer, stored here as the cell index. */
static void _obj_spawn_b8(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u,
              (uint16_t)(row * 16u + _obj_style_nudge(NULL, col, row) - 8u));
    wr_le_u16(r + 4u, 0x00AFu);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, 0x0000u);
    wr_le_u16(r + 0xAu, 0x0000u);
    wr_le_u16(r + 0xCu, 0x0000u);
    wr_le_u16(r + 0xEu, (uint16_t)(row * OBJ_COLS + col));
    _obj_slot_class[_obj_live_count - 7u] = 3u;
    _obj_live_count++;
}

/* 1000:52A2 (0xAC, frame 0x96 -> behaviour 7), 1000:52C7 (0xAD, frame 0x9C ->
 * behaviour 6) and 1000:52EB (0xAE, frame 0x38 -> behaviour 4): one appended
 * record each, then the shared tail 1000:5639 snaps its y to the terrain step.
 * Note the behaviours: the campfire (0x96) is 7 and the torch (0x9C) is 6 - the
 * summary row in re_map.md had those two swapped until this read.
 * 1000:5662 (called first) nudges y by 3 on a style-6 level when the cell
 * diagonally below-left is marked in the static 0x1F20 table. */
/** 1000:5662: on a style-6 level the tile at the handler's `SI + 0x1F` (the
 * walk's SI is one past the matched cell, so this is the cell directly below)
 * selects a +3 row nudge through the static 0x1F20 bit table.  Every spawn
 * handler calls it before storing its y. */
static uint32_t _obj_style_nudge(const uint8_t *map, uint32_t col, uint32_t row)
{
    static const uint8_t nudgeBits[19] = {
        0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07, 0xE0, 0x00,
        0x02, 0x02, 0x00, 0x03, 0xC3, 0x81, 0x80, 0x01, 0x80,
    };
    uint32_t cell = row * OBJ_COLS + col;
    uint32_t tile;

    if (!map) map = level_map();
    if (level_style() != 6u || !map || cell + OBJ_COLS >= 640u) return 0u;
    tile = map[cell + OBJ_COLS];
    if (tile > 0x90u) return 0u;
    return (nudgeBits[tile >> 3] & (uint8_t)(1u << (7u - (tile & 7u)))) ? 3u
                                                                        : 0u;
}

/** @brief Append the style record's decoration rows. */
static void _obj_append_style(const uint8_t *map, uint32_t col, uint32_t row,
                              uint32_t frame, uint32_t behaviour, uint32_t f8,
                              uint32_t fc, uint32_t fe)
{
    uint8_t *r;
    uint32_t y3, probeCell;
    int32_t bx;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    y3 = _obj_style_nudge(map, col, row);
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u + y3));
    wr_le_u16(r + 4u, frame);
    wr_le_u16(r + 6u, 0u);
    wr_le_u16(r + 8u, f8);
    wr_le_u16(r + 0xAu, 0u);
    wr_le_u16(r + 0xCu, fc);
    wr_le_u16(r + 0xEu, fe);
    _obj_slot_class[_obj_live_count - 7u] = (uint8_t)behaviour;
    _obj_live_count++;
    bx = _obj_step_probe_at(r, 8, 0x10, &probeCell); /* 1000:5639 */
    if (bx >= 0) wr_le_u16(r + 2u, (uint16_t)(bx - 0x10));
}

/** 1000:51F9 decode_terrain_row (tile 0xA9): five records from the 0x1EFE
 * offset table at (col*16 + dx, row*16 + 1 + dy), frames 0x33..0x37, `+8 = 0`,
 * behaviour 0, plus one 0x32B9 background entry whose `+4` points at their first
 * record and whose `+8` is the map pointer + 0x20.
 * Its first two instructions mark the two map cells one row below as 0xC2/0xC9 -
 * the ids 1000:DF2C tests, so this handler is what makes the settle's background
 * mark reachable. */
static void _obj_append_row9(uint8_t *map, uint32_t col, uint32_t row)
{
    static const uint8_t table[5][3] = {
        /* 0x1EFE: dx, dy, frame */
        { 0x10, 0x13, 0x33 }, { 0x16, 0x15, 0x34 }, { 0x1C, 0x16, 0x35 },
        { 0x23, 0x15, 0x36 }, { 0x29, 0x13, 0x37 },
    };
    uint32_t first = _obj_live_count + 1u;
    uint8_t *firstRec = _obj_array + first * OBJ_STRIDE;

    for (uint32_t i = 0; i < 5u; i++) {
        uint8_t *r;

        if (_obj_live_count + 1u >= OBJ_SLOTS) return;
        r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
        wr_le_u16(r + 0u, (uint16_t)(col * 16u + table[i][0]));
        wr_le_u16(r + 2u, (uint16_t)(row * 16u + 1u + table[i][1]));
        wr_le_u16(r + 4u, table[i][2]);
        wr_le_u16(r + 6u, 0u);
        wr_le_u16(r + 8u, 0u);
        _obj_slot_class[_obj_live_count - 7u] = 0u; /* behaviour 0 */
        _obj_live_count++;
    }
    /* 1000:51F9: mark the two cells one row below as the ids 1000:DF2C looks
     * for.  The handler's SI is one past the matched cell, so the pair lands at
     * (col+1, row+1) and (col+2, row+1) - live level 16 row 17 reads
     * 0x33 0xC2 0xC9 at cols 14..16 with the 0xA9 at col 14 (the 0x33 is the
     * level's own tile, untouched).  The original's buffer carries a duplicated
     * first row at +0x20, so its write always lands; the port's map is exactly
     * 640 bytes, so the pair is bounds-checked instead. */
    {
        uint32_t cell = row * OBJ_COLS + col;

        if (cell + OBJ_COLS + 2u < 640u) {
            map[cell + OBJ_COLS + 1u] = 0xC2u;
            map[cell + OBJ_COLS + 2u] = 0xC9u;
        }
    }
    if (_obj_bg_count < sizeof _obj_bg / sizeof _obj_bg[0]) {
        _obj_bg[_obj_bg_count].x = (uint16_t)(col * 16u);
        _obj_bg[_obj_bg_count].y = (uint16_t)(row * 16u + 1u);
        _obj_bg[_obj_bg_count].dst = firstRec;
        _obj_bg[_obj_bg_count].mark = &map[(row + 1u) * OBJ_COLS + col + 1u];
        _obj_bg_count++; /* [0x32B9] */
    }
}

/** 1000:556B terrain_append_decoration, the tile handler 0xF0/0xF1/0xF2 share
 * (0xF0 is a four-instruction stub that sets BX and jumps in; the frame is
 * 0xC1 / 0xC5 / 0xC9, per the table in re_map.md): an appended record
 * {x, y, frame, +6 = 0, +8 = frame, +0xA = 0} with behaviour 1. */
static void _obj_append_decoration(uint32_t x, uint32_t y, uint8_t tile)
{
    static const uint16_t frame[3] = { 0x00C1u, 0x00C5u, 0x00C9u };
    uint16_t f = frame[tile - 0xF0u];
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)x);
    wr_le_u16(r + 2u, (uint16_t)y);
    wr_le_u16(r + 4u, f);
    wr_le_u16(r + 6u, 0u);
    wr_le_u16(r + 8u, f);
    wr_le_u16(r + 0xAu, 0u);
    _obj_slot_class[_obj_live_count - 7u] = 1u; /* [0x3E5F + count-8] = 0x01 */
    _obj_live_count++;
}

/** @brief Spawn every map cell that holds one placed-object tile. */
static void _spawn_terrain_tile(uint8_t *map, uint32_t tile)
{
    for (uint32_t row = 0; row < OBJ_ROWS; row++)
        for (uint32_t col = 0; col < OBJ_COLS; col++) {
            if (map[row * OBJ_COLS + col] != tile) continue;
            /* live-confirmed (2026-10-30): level 1's records read
             * (48,25) (208,25) (256,137) (464,265) = row*16 - 7, exactly
             * what 0x524B writes. */
            if (tile == 0xBBu) _obj_spawn_bb(col, row);
            else if (tile == 0xBEu) _obj_spawn_be(col, row);
            else if (tile == 0xBFu) _obj_join_bf(col);
            else if (tile == 0xC0u) _obj_spawn_c0(col, row);
            else if (tile == 0xC1u) _obj_spawn_c1(col, row);
            else if (tile == 0xB6u) _obj_spawn_b6(col, row);
            else if (tile == 0xB7u) _obj_spawn_b7(map, col, row);
            else if (tile == 0xB8u) _obj_spawn_b8(col, row);
            else if (tile >= 0xAFu && tile <= 0xB2u)
                _obj_write_pair(tile, col * 16u, row * 16u);
            else if (tile == 0xA9u) _obj_append_row9(map, col, row);
            else if (tile == 0xAAu) _obj_place_human(col, row);
            else if (tile == 0xABu) _obj_append_spear(col, row);
            else if (tile == 0xACu)
                _obj_append_style(map, col, row, 0x96u, 7u, 0xFFFFu, 0u, 0u);
            else if (tile == 0xADu)
                _obj_append_style(map, col, row, 0x9Cu, 6u, 0xFFFFu, 0u, 0u);
            else if (tile == 0xAEu)
                /* 1000:52EB differs from 0x52A2/0x52C7: +8 and +0xA are 0
                 * and BOTH +0xC and +0xE are 0xFFFF (+0xE is the give/hold
                 * gate 0x9CF6 tests with bit 15). */
                _obj_append_style(map, col, row, 0x38u, 4u, 0x0000u, 0xFFFFu,
                                  0xFFFFu);
            else _obj_append_decoration(col * 16u, row * 16u, tile);
        }
}

/** @brief Spawn the map's placed objects. */
uint32_t object_load_terrain_objects(uint8_t *map)
{
    if (!map) return _obj_live_count;
    _obj_live_count = OBJ_LIVE0; /* 1000:516F */
    memset(_obj_slot_class + 1u, 0, 20u); /* 1000:5175: 0x3E5F..0x3E72 */
    for (uint32_t t = 0; t < 25u; t++) {
        uint8_t tile = _obj_tiles[t];
        /* all 25 tiles' bodies are ported (re_map.md has each one's
         * summary); the guard is the table walk's tile list. */
        if (tile != 0xA9u && tile != 0xAAu && tile != 0xABu && tile != 0xACu &&
            tile != 0xADu && tile != 0xAEu && tile != 0xAFu && tile != 0xB0u &&
            tile != 0xB1u && tile != 0xB2u && tile != 0xB6u && tile != 0xB7u &&
            tile != 0xB8u && tile != 0xBBu && tile != 0xBEu && tile != 0xBFu &&
            tile != 0xC0u && tile != 0xC1u && (tile < 0xF0u || tile > 0xF2u))
            continue;
        _spawn_terrain_tile(map, tile);
    }
    return _obj_live_count;
}

/** @brief Accessor: the frame slot count. */
uint32_t object_frame_slots(void) { return OBJ_FRAME_SLOTS; }

/** @brief Accessor: the frame counter. */
uint32_t object_frame_counter_probe(void) { return _obj_frame; }

/** one sprite-table entry, for the tests (the D-69 tail copies). */
void object_frame_for_probe(uint32_t frame, uint32_t *off, uint32_t *rows)
{
    if (frame >= OBJ_FRAME_SLOTS) {
        *off = 0;
        *rows = 0;
        return;
    }
    *off = _obj_sprites[frame].off;
    *rows = _obj_sprites[frame].rows;
}

/** 0x32B9's queue for the drawer at 1000:5F54 (level.c's level_draw_bg_sprites). */
uint32_t object_bg_count(void) { return _obj_bg_count; }

/** @brief Accessor: one background decoration's position. */
void object_bg_at(uint32_t i, uint32_t *x, uint32_t *y)
{
    if (i >= _obj_bg_count) {
        *x = 0;
        *y = 0;
        return;
    }
    *x = _obj_bg[i].x;
    *y = _obj_bg[i].y;
}

/** @brief Publish the object sprite bank. */
void object_publish_bank(const uint8_t *bank, uint32_t size)
{
    _obj_bank = bank;
    _obj_bank_size = size;
}

/** 1000:653C's base blit: the sprite at `src`, top-left (x,y), 0 = transparent.
 * `cols` is 16 for mode 1 (`1000:66EC`) and 32 for mode 2 (`1000:6C74`) — the
 * only difference between the two (the frame stride confirms it: mode-2 frames
 * advance rows*32 bytes).  The layout is derived, not probed: the blit is four
 * plane passes (`67BA: AH = 1 << ([0x6C32] & 3)`, `OUT 0x3C4`), each row-taking
 * `LODSW`/store/INC DI x2 (`67F9..681A`) = **4 bytes, one per pixel, 4 px per
 * byte of one plane**; between passes `6C15 ADD SI, [0x6C2C]` and [0x6C2C] is 0
 * unless clipped at the top, so the frame is consumed linearly in four blocks
 * of 4h (`block = col mod 4`, `byte = row*4 + col/4`). */
static void _blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                         uint32_t cols, int32_t x, int32_t y)
{
    uint32_t w = cols >> 2, block = rows * w;

    for (uint32_t row = 0; row < rows; row++) {
        int32_t py = y + (int32_t)row;
        if (py < 0 || py >= 200) continue;
        for (uint32_t col = 0; col < cols; col++) {
            uint8_t v = src[(col & 3u) * block + row * w + (col >> 2)];
            int32_t px = x + (int32_t)col;
            if (!v) continue;
            if (px < 0 || px >= 320) continue;
            page[(uint32_t)py * 320u + (uint32_t)px] = v;
        }
    }
}

/** @brief Accessor: the overlay row for one frame. */
static uint32_t _obj_overlay_row(uint32_t frame)
{
    /* the image's table at DS:0x3570 runs to frame 0x119 (0x29A bytes before
     * the 0x368A index table); frames past it read row 0, as the port's
     * frames 0x11A..0x11D do. */
    return frame < 0x11Au ? _obj_frame_row[frame] : 0u;
}

/** @brief Publish the G sprite blocks. */
void object_publish_g_sprites(const uint8_t *data, uint32_t blocks, uint32_t n)
{
    _obj_g_sprites = data;
    _obj_g_blocks = blocks;
    _obj_gn = n;
}

/** @brief Publish the DEH sprite banks. */
void object_publish_deh_sprites(const uint8_t *e, const uint8_t *h)
{
    _obj_e_sprite = e;
    _obj_h_sprites = h;
}

/** @brief Blit one G-bank frame; 1 = inside the bank. */
static int32_t _obj_blit_g_frame(uint8_t *page, uint32_t frame, int32_t x,
                                 int32_t y)
{
    uint32_t slot, layer, i, block;

    if (!_obj_g_sprites || !_obj_gn || frame < 0xC1u || frame >= 0xC1u + 12u)
        return 0;
    slot = frame - 0xC1u;
    layer = slot / 4u;
    i = slot % 4u;
    if (i >= _obj_gn) return 0;
    block = layer * _obj_gn + i;
    if (block >= _obj_g_blocks) return 0;
    _blit_sprite(page, _obj_g_sprites + block * 0x100u, 16u, 16u, x, y);
    return 1;
}

/** @brief Blit one object frame. */
static void _obj_blit_frame(uint8_t *page, uint32_t frame, int32_t x, int32_t y)
{
    const ObjSprite *sp;
    uint32_t cols;

    if (_obj_blit_g_frame(page, frame, x, y)) return;
    /* The DEFS field-D/H pointers land in these slots: 0x33..0x37 (the five
     * A9 logs, all the same 8x16 field-E sprite) and 0x26/0x27 (field H). */
    if (_obj_e_sprite && frame >= 0x33u && frame <= 0x37u) {
        _blit_sprite(page, _obj_e_sprite, 8u, 16u, x, y);
        return;
    }
    if (_obj_h_sprites && (frame == 0x26u || frame == 0x27u)) {
        _blit_sprite(page, _obj_h_sprites + (frame - 0x26u) * 0x100u, 16u, 16u,
                     x, y);
        return;
    }
    if (frame >= OBJ_FRAME_SLOTS) return;
    sp = &_obj_sprites[frame];
    if (!sp->rows || sp->mode > 2u) return; /* blank / mode 0 or 3 */
    cols = sp->mode == 2u ? 32u : 16u; /* 1000:6C74 vs 1000:66EC */
    if ((uint32_t)sp->off + sp->rows * cols > _obj_bank_size) return;
    _blit_sprite(page, _obj_bank + sp->off, sp->rows, cols, x,
                 y + (int32_t)sp->ybias);
}

/** 1000:653C draw_object_cell's class half: the class id names both the
 * behaviour class (0x3E5E) and the placed record (0x3F17 + 0x12*(id-1)) that
 * carries the extra sprite.  That record's x/y/frame are overwritten with the
 * carrier-relative position, as the original does - it is a per-frame cache,
 * not a scratch copy. */
static void _obj_draw_class_overlay(uint8_t *page, int32_t scrollX,
                                    int32_t scrollY, uint8_t *rec)
{
    uint32_t cls = rec[7];
    uint32_t col, row, idx, x, y, frame;
    const uint8_t *e;
    uint8_t *carried;

    if (!cls) return;
    /* 1000:6570: CX = rec[+7]; SUB CX,1; SI = 0x3E5F + CX, i.e. the byte at
     * 0x3E5E + class. */
    col = _obj_cls_col[_obj_class_byte_at(cls) & 0x3Fu]; /* 1000:657F */
    if (!col) return;
    frame = rd_le_u16(rec + 4u);
    /* 1000:65A7: `draw_object_cell` enters the class branch whenever the
     * frame's 0x3570 byte is NON-ZERO and only then masks it to 0x7F - a
     * 0x80 byte means row 0, whose 0x368A entry can still be live. */
    if (_obj_overlay_row(frame) == 0u) return;
    row = _obj_overlay_row(frame) & 0x7Fu;
    idx = _obj_overlay_idx[3u * row + col]; /* 1000:65C2 */
    if (!idx) return;
    e = _obj_overlay_ent + 4u * idx; /* 1000:65D5 */
    x = rd_le_u16(rec + 0u);
    y = rd_le_u16(rec + 2u);
    if (e[2]) { /* 1000:65E4 */
        if (8u + cls < OBJ_SLOTS) {
            carried = _obj_array + (8u + cls) * OBJ_STRIDE;
            wr_le_u16(carried + 0u,
                      (uint16_t)(x + (uint16_t)(int16_t)(int8_t)e[0]));
            wr_le_u16(carried + 2u,
                      (uint16_t)(y + (uint16_t)(int16_t)(int8_t)e[1]));
            wr_le_u16(carried + 4u, e[2]);
            _obj_blit_frame(
                page, e[2], (int32_t)(int16_t)rd_le_u16(carried + 0u) - scrollX,
                (int32_t)(int16_t)rd_le_u16(carried + 2u) - scrollY);
        }
    }
    if (e[3]) /* 1000:6605 */
        _obj_blit_frame(page, e[3], (int32_t)(int16_t)x - scrollX,
                        (int32_t)(int16_t)y - scrollY);
}

/** The one blitter, for the entity pass (1000:5C5F -> 1000:6C74): the same law,
 * mode 2's 32-px cells. */
void object_blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                        uint32_t cols, int32_t x, int32_t y)
{ _blit_sprite(page, src, rows, cols, x, y); }

/** One placed/live record through the object drawer.  `scan` selects the
 * scan_object_rows path (slots 8+): its class slot 0x3E5E + i-8 can mark the
 * record taken, and a +7 == 0 record is drawn straight through
 * select_object_tile (1000:6625) with no 0x3570 gate.  The list passes
 * (1000:6495 -> 1000:653C) gate the base on 0x3570 instead. */
static void _obj_draw_record(uint8_t *page, int32_t scrollX, int32_t scrollY,
                             uint32_t i, int32_t scan)
{
    uint8_t *r = _obj_array + i * OBJ_STRIDE;
    int32_t x = (int32_t)(int16_t)rd_le_u16(r + 0u) - scrollX;
    int32_t y = (int32_t)(int16_t)rd_le_u16(r + 2u) - scrollY;
    uint32_t frame = rd_le_u16(r + 4u);

    if (r[6] & 0x80u) return; /* 1000:794C / 1000:6458 */
    /* 1000:6450/1000:6467: the record's class slot (0x3E5E + record-8)
     * skips it when bit 7 is set - the pickup's taken mark (1000:9D47).
     * The carried record is drawn by its carrier's class overlay only. */
    if (scan && (_obj_slot_class[i - 8u] & 0x80u)) return;
    if (!r[7]) {
        /* 1000:653C skips only the EARLY base draw when 0x3570[frame] bit 7
         * is set; its tail (1000:6610) draws the base at 1000:661C instead.
         * With no class overlay the record is always drawn exactly once, so
         * the climb frames 0x23..0x25 (0x3570 = 0xC3/0xC4/0xC5) must draw. */
        _obj_blit_frame(page, frame, x, y);
        return;
    }
    if (!(_obj_overlay_row(frame) & 0x80u)) _obj_blit_frame(page, frame, x, y);
    _obj_draw_class_overlay(page, scrollX, scrollY, r);
    if (_obj_overlay_row(frame) & 0x80u) _obj_blit_frame(page, frame, x, y);
}

/** 1000:C23C partition_entities_by_type: the eight placed slots split by their
 * type word `+8`; 6..8 (the human-stack states - grab 6, push/stand 7,
 * step-off/register 8) go to the 0x651E list drawn **after** the live
 * records, everything else to the 0x650C list drawn **before** them.  The
 * acting record is skipped in the slot walk and appended last in its own
 * class, so it is the last drawn there. */
static void _obj_partition(uint8_t *back, uint32_t *nb, uint8_t *front,
                           uint32_t *nf)
{
    uint32_t b = 0, f = 0;

    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint32_t t;

        if (r[6] & 0x80u) continue;
        if (i == _obj_entity) continue;
        t = rd_le_u16(r + 8u);
        if (t >= 6u && t <= 8u) front[f++] = (uint8_t)i;
        else back[b++] = (uint8_t)i;
    }
    {
        uint32_t t;

        if (_obj_entity >= OBJ_SLOTS) {
            *nb = b;
            *nf = f;
            return;
        }
        t = rd_le_u16(_obj_array + _obj_entity * OBJ_STRIDE + 8u);
        if (t >= 6u && t <= 8u) front[f++] = (uint8_t)_obj_entity;
        else back[b++] = (uint8_t)_obj_entity;
    }
    *nb = b;
    *nf = f;
}

/** 1000:5D65 object_update_loop: the two handled-object slots ([0x6F92]).
 *
 * A non-free entry draws a two-pixel-wide vertical stripe of colour 0x18 from
 * the row its `map_move_test` (1000:FC81) seeded it at down to its current top
 * (+0xC).  The original paints onto the page and keeps the covered background
 * in the entry's 0x280-byte area at 0x2DB0 (`blit_object_pixel` 1000:5E0F
 * re-writes the saved bytes, `blit_object_pixel2` 1000:5E5D saves and paints
 * 0x18), so the stripe survives between frames; the port rebuilds the page
 * every frame and redraws it from `_obj_span_top`.
 *
 * The counters: 1000:5DD1 grows the stripe up to four rows an odd frame (the
 * `[0x4103] & 1` test at 5DD4) while +0xC and +6 advance together, stopping
 * when +6 reaches +8; 1000:5D9D retracts it the same way while +6 falls back
 * to 0.  Either end writes +0x12 = 0xFFFF.  A finished stripe keeps its
 * pixels: 1000:5D8C only stops *touching* them, it never erases them.
 *
 * Two pixels: the write goes to planes p and p+1 (`p = x & 3`) of the same
 * 4-pixel group, i.e. pixels x and x+1 - except p == 3, where the mask wraps
 * and the pointer steps on, which is the same two pixels.
 */
static void _obj_span_draw(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    for (uint32_t i = 0; i < 2u; i++) {
        uint8_t *e = (uint8_t *)_obj_span[i];
        uint32_t state, cur, len, tgt, x, seed;

        if (rd_le_u16(e + 0u) & 0x8000u) continue; /* 1000:5D71: free  */
        wr_le_u16(e + 0xEu, rd_le_u16(e + 0x6u)); /* 1000:5D86: +0xE */
        state = rd_le_u16(e + 0x12u);
        cur = rd_le_u16(e + 0xCu);
        len = rd_le_u16(e + 0x6u);
        tgt = rd_le_u16(e + 0x8u);
        if (state <= 1u && (_obj_frame & 1u)) {
            uint32_t e13 = rd_le_u16(e + 0xEu);
            int32_t stop = 0;

            for (uint32_t k = 0; k < 4u && !stop; k++) {
                if (state != 0u) { /* growing (1000:5DCE) */
                    cur = (uint16_t)(cur + 1u);
                    len = (uint16_t)(len + 1u);
                    if (len == tgt) { /* 1000:5DE2 */
                        state = 0xFFFFu;
                        break;
                    }
                    e13 = (uint16_t)(e13 + 1u);
                    if (e13 == tgt) stop = 1; /* 1000:5DED */
                } else { /* retracting (1000:5D9D) */
                    cur = (uint16_t)(cur - 1u);
                    len = (uint16_t)(len - 1u);
                    if ((int16_t)len < 0) { /* 1000:5DAE -> 5DBB */
                        len = 0u;
                        state = 0xFFFFu;
                        stop = 1;
                    } else {
                        e13 = (uint16_t)(e13 - 1u);
                        if ((int16_t)e13 < 0) stop = 1;
                    }
                }
            }
            wr_le_u16(e + 0xEu, (uint16_t)e13);
            wr_le_u16(e + 0xCu, (uint16_t)cur);
            wr_le_u16(e + 0x6u, (uint16_t)len);
            wr_le_u16(e + 0x12u, (uint16_t)state);
        }
        seed = _obj_span_top[i];
        x = rd_le_u16(e + 0xAu);
        for (int32_t row = (int32_t)(int16_t)seed; row < (int32_t)(int16_t)cur;
             row++) {
            int32_t px = (int32_t)x - scrollX;
            int32_t py = row - scrollY;

            if (py < 0 || py >= 200) continue;
            if (px >= 0 && px < 320)
                page[(uint32_t)py * 320u + (uint32_t)px] = 0x18u;
            if (px + 1 >= 0 && px + 1 < 320)
                page[(uint32_t)py * 320u + (uint32_t)(px + 1)] = 0x18u;
        }
    }
}

/** render_objects' object pass (1000:5D56 -> draw_object_group 1000:6421):
 * the 0x650C list, then scan_object_rows (slots 8..[0x3E73]), then the
 * 0x651E list. */
void object_render(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    uint8_t back[9], front[9];
    uint32_t nb = 0, nf = 0;

    if (!page || !_obj_bank) return;
    /* render_objects (1000:5D56) also calls 1000:5D65, the two handled-object
     * slots' stripe pass, before the records. */
    _obj_span_draw(page, scrollX, scrollY);

    _obj_partition(back, &nb, front, &nf); /* 1000:C23C */

    for (uint32_t k = 0; k < nb; k++) /* 1000:6429 */
        _obj_draw_record(page, scrollX, scrollY, back[k], 0);

    /* scan_object_rows (1000:6433): AX = [0x3E73] - 8, then `DEC AX / JNS`,
     * so the walk covers 0x3F05 .. the record at [0x3E73] INCLUSIVE -
     * `[0x3E73]` counts the top live index, not the number of records
     * (level 1: 9 draws slots 8 and 9). */
    for (uint32_t i = 8u; i <= _obj_live_count && i < OBJ_SLOTS; i++)
        _obj_draw_record(page, scrollX, scrollY, i, 1);

    for (uint32_t k = 0; k < nf; k++) /* 1000:642E */
        _obj_draw_record(page, scrollX, scrollY, front[k], 0);
}

/** @brief Initialise the terrain slots. */
void object_ter_slot_init(void)
{
    memset(_obj_ter_slot, 0, sizeof _obj_ter_slot); /* 1000:4CB2.. */
    wr_le_u16(_obj_ter_slot[0] + 0x10u, 0xFFFFu); /* 1000:4CC3 [0x64AA] */
    wr_le_u16(_obj_ter_slot[1] + 0x10u, 0xFFFFu); /* 1000:4CC6 [0x64BE] */
}

/** @brief Accessor: one terrain slot. */
uint8_t *object_ter_slot_at(uint32_t i)
{ return i < 2u ? _obj_ter_slot[i] : NULL; }

/** @brief Publish the terrain bank. */
void object_publish_ter(const uint8_t *bank, uint32_t size)
{
    _obj_ter_bank = bank;
    _obj_ter_size = size;
}

/** 1000:5C2F + the 0x649A loop of 1000:5823. */
void object_render_ter(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    if (!page || !_obj_ter_bank) return;
    for (uint32_t s = 0; s < 2u; s++) {
        uint8_t *e = _obj_ter_slot[s];
        uint32_t index, idx, src, yoff, rows;

        if (rd_le_u16(e + 0x10u) & 0x8000u) continue; /* 1000:587F */
        index = rd_le_u16(e + 0xCu);
        idx = index < sizeof _obj_ter_index ? _obj_ter_index[index] : 0u;
        if (4u * idx + 4u > _obj_ter_size || 2u * idx + 0x32u > _obj_ter_size)
            continue;
        src = (uint32_t)rd_be_u16(_obj_ter_bank + 4u * idx + 2u) + 0x40u;
        yoff = rd_be_u16(_obj_ter_bank + 2u * idx + 0x20u);
        rows = rd_be_u16(_obj_ter_bank + 2u * idx + 0x30u);
        if (src + rows * 8u > _obj_ter_size) continue;
        object_blit_sprite(page, _obj_ter_bank + src, rows, 32u,
                           (int32_t)(int16_t)rd_le_u16(e + 0u) - 0xCu - scrollX,
                           (int32_t)(int16_t)rd_le_u16(e + 2u) + (int32_t)yoff -
                               scrollY);
    }
}

/** --- object_test_dispatch (1000:35D6) ---------------------------------
 *
 * The level record's `+0x04` word (the 13-byte header copied to `202F:0x17F2`)
 * is the whole goal test: the low byte is the mode, the high byte its
 * argument (the handlers read `DI = 0x17F3`).  The mode's low 7 bits index the
 * CS word table at `0x35F4` = `0x3600, 0x3640, 0x3659, 0x3678, 0x3688,
 * 0x36AB`; bit 7 first runs the entity scan whose result the dispatch
 * discards, so it changes nothing and the port masks it off.
 *
 * Returns 1 = the goal is met (the original's -1).
 */
int32_t object_test_dispatch(uint16_t mode)
{
    uint32_t op = mode & 0x7Fu;
    uint32_t arg = (mode >> 8) & 0xFFu;

    switch (op) {
    case 0: return object_low_phase_match() == -1; /* 1000:3600 */
    case 1: return object_none_type1d() == -1; /* 1000:3640 */
    case 2: return object_class_matches(arg) == -1; /* 1000:3659 */
    case 3: return object_type_equals(arg) == -1; /* 1000:3678 */
    case 4: return object_entities_finished() == -1; /* 1000:3688 */
    case 5: return object_map_cell_equals(arg) == -1; /* 1000:36AB */
    default: return 0;
    }
}

/* --- 1000:C2CE count_tile_collisions: the goal scan --------------------
 *
 * update_world's seventh call (1000:80C6), inside the [0xC] gate: clear
 * [0x6530], count the enabled static records standing on the goal tile 0xBA,
 * add the command-4 live records (twice when their +0xE has bit 15 clear),
 * and raise [0x6530] when the count is non-zero and reaches the level header's
 * goal byte [0x17F0].  menu_flag_tick (1000:3540) is the flag's only reader.
 */

/* 0x6549: the five-byte id exclusion mask (nothing writes it). */
static const uint8_t _obj_goal_mask[5] = { 0x1F, 0x99, 0x02, 0x1C, 0x20 };

/** the 0xBA test's map address: 32*((y + 0xC) & 0xFFF0)/16 + (x + 8)/16, the
 * 16-bit adds wrapping as the original's do. */
static uint32_t _obj_goal_cell_index(uint32_t x, uint32_t y)
{
    uint32_t yy = (uint32_t)(uint16_t)(y + 0x0Cu);
    uint32_t xx = (uint32_t)(uint16_t)(x + 8u);

    return 32u * ((yy & 0xFFF0u) >> 4) + (xx >> 4);
}

/** @brief Find the group that lists record i; its base point goes to x/y. */
static int32_t _goal_group(uint32_t i, uint32_t *x, uint32_t *y)
{
    for (uint32_t g = 0; g < 8u; g++) { /* 1000:C319 */
        uint8_t *e = (uint8_t *)_obj_register[g];
        uint8_t *p;
        int32_t hit = 0;

        if (rd_le_u16(e + 0u) & 0x8000u) continue; /* 1000:C32B */
        p = e + 5;
        while (1) { /* 1000:C339 */
            if (*p == (uint8_t)i) {
                hit = 1;
                break;
            }
            p++;
            if (*p & 0x80u) break; /* 1000:C347 */
        }
        if (!hit) continue;
        *x = rd_le_u16(e + 0u); /* 1000:C325 */
        *y = rd_le_u16(e + 2u); /* 1000:C31E */
        return (int32_t)g;
    }
    return -1;
}

/** @brief The goal scan over the placed records. */
void object_goal_scan(void)
{
    uint32_t count = 0;

    _obj_goal_flag = 0; /* 1000:C2CE */

    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint32_t x, y, type;

        if (r[6] & 0x80u) continue; /* 1000:C2E7 */
        x = rd_le_u16(r + 0u);
        y = rd_le_u16(r + 2u);
        type = rd_le_u16(r + 8u);
        if (type == 8u) { /* 1000:C2F7 -> C300: a group base's own point */
            if (_goal_group(i, &x, &y) < 0) continue; /* 1000:C35B */
        } else {
            /* 1000:C389 test_goal_cell_occupancy: the id's mask bit skips. */
            uint32_t bit;

            if (type >= 40u) continue; /* the mask read is past its table */
            bit = 1u << (7u - (type & 7u));
            if (_obj_goal_mask[type >> 3] & bit) continue; /* 1000:C3A5 */
        }
        if (_obj_map_buffer_byte((int32_t)_obj_goal_cell_index(x, y)) == 0xBAu)
            count++; /* 1000:C386 */
    }

    /* 1000:C3D6: the live records whose behaviour byte is 4. */
    for (uint32_t k = 0; OBJ_LIVE0 + 1u + k <= _obj_live_count; k++) {
        uint8_t *r = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;

        if (r[6] & 0x80u) continue; /* 1000:C3F1 */
        if ((_obj_slot_class[1u + k] & 0x3Fu) != 4u) continue; /* 1000:C3FC */
        if (_obj_map_buffer_byte((int32_t)_obj_goal_cell_index(
                rd_le_u16(r + 0u), rd_le_u16(r + 2u))) != 0xBAu)
            continue; /* 1000:C417 */
        count++;
        if (!(rd_le_u16(r + 0xEu) & 0x8000u)) count++; /* 1000:C421 */
    }

    /* 1000:C434: a non-zero count that reaches the level's goal byte. */
    if (count != 0u && count >= level_goal_count()) _obj_goal_flag = 0xFFu;
}

/** @brief Accessor: the goal flag. */
int32_t object_goal_flag(void) { return _obj_goal_flag != 0u; }

/** 1000:D5E4: the per-frame 0x32B9 pass over the 0xA9 log records.  A record
 * whose +8 the settle's 0xDF2C poked (the tables 0x689E/0x68BE write 0xFF at
 * firstRecord + 8 + 0x12*k) falls: +2 per tick, and past y 0x138 it sets +6 bit
 * 7, which hides it.  The same pass rewrites the two map marker cells from the
 * table at 0x6697, encoding which records are marked.
 *
 * Live-verified 2026-11-02 (walking left onto level 16's logs, ~30 ms samples):
 * the five records fell in turn (y 276 -> 314, +6 = 0xFF) and the markers went
 * C2/C9 -> C2/CA (log 13) -> C3/00 (logs 11-13) -> 00/00, exactly the table
 * arithmetic below. */
static void _obj_bg_tick(void)
{
    /* DS:0x6697: five 2-byte record entries, then the two marker runs. */
    static const uint8_t tbl[26] = {
        0x04, 0x00, 0x02, 0x00, 0x01, 0x04, 0x00, 0x02, 0x00,
        0x01, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0x00,
        0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0x00,
    };
    uint32_t i;

    for (i = 0; i < _obj_bg_count; i++) {
        uint8_t *r = _obj_bg[i].dst;
        uint32_t cx = 0, dx = 0, t = 0, k;

        if (!r) continue;
        for (k = 0; k < 5u; k++, r += OBJ_STRIDE, t += 2u) {
            if (!(r[6] & 0x80u)) {
                uint32_t y;

                if (rd_le_u16(r + 8u) == 0u) continue; /* 1000:D614: no OR  */
                y = (uint32_t)rd_le_u16(r + 2u) + 2u;
                wr_le_u16(r + 2u, (uint16_t)y);
                if (y > 0x138u) r[6] = 0xFFu; /* 1000:D61F         */
            }
            cx |= tbl[t];
            dx |= tbl[t + 1u];
        }
        if (_obj_bg[i].mark) {
            _obj_bg[i].mark[0] = tbl[0x0Au + cx]; /* 1000:D638         */
            _obj_bg[i].mark[1] = tbl[0x12u + dx]; /* 1000:D641         */
        }
    }
}

/** 0xC77B (behaviour 1): an 8-tick frame stepper between +8 and +8 + [0x6532]. */
static void _obj_walk_step1(uint8_t *r)
{
    uint32_t a = rd_le_u16(r + 0xAu);

    if (a != 0u) {
        wr_le_u16(r + 0xAu, a - 1u);
        return;
    }
    wr_le_u16(r + 0xAu, 8u);
    {
        uint32_t base = rd_le_u16(r + 8u);
        uint32_t f = (uint32_t)(rd_le_u16(r + 4u) + 1u);

        if (f >= base + level_anim_frames()) f = base;
        wr_le_u16(r + 4u, (uint16_t)f);
    }
}

/** 0xCBFE (behaviour 3): a 4-tick sequence player over 0x6568, +0xC the cursor;
 * the end of a run sets +8 = 2 and clears the map cell at +0xE. */
static void _obj_walk_seq(uint8_t *r)
{
    uint32_t a, cur, i;

    if (rd_le_u16(r + 8u) != 1u) return;
    a = rd_le_u16(r + 0xAu);
    if (a != 0u) {
        wr_le_u16(r + 0xAu, a - 1u);
        return;
    }
    wr_le_u16(r + 0xAu, 4u);
    cur = rd_le_u16(r + 0xCu);
    wr_le_u16(r + 0xCu, cur + 1u);
    i = cur;
    wr_le_u16(r + 4u, i < sizeof _obj_seq6568 ? _obj_seq6568[i] : 0u);
    if (i + 1u < sizeof _obj_seq6568 && _obj_seq6568[i + 1u] == 0u) {
        uint8_t *map = (uint8_t *)level_map(); /* the writable [0x1FA0] copy */

        wr_le_u16(r + 8u, 2u);
        if (map && rd_le_u16(r + 0xEu) < 640u) map[rd_le_u16(r + 0xEu)] = 0u;
    }
}

/** 0xCC39 (behaviour 2): the wanderer, on even frames.  The state lives in +8
 * (0 = walking right, 1 = left, 2 = arrived); +0x10 is a 4-tick divider. */
static void _obj_walk_wander(uint8_t *r)
{
    uint32_t ax, bx, cx, dx;

    if (_obj_frame & 1u) return;
    ax = rd_le_u16(r + 0u);
    bx = rd_le_u16(r + 4u);
    cx = rd_le_u16(r + 8u);
    dx = rd_le_u16(r + 0x10u);
    switch (cx) {
    case 0: /* 0xCCC0 */
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
    case 1: /* 0xCCE3 */
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
    default: /* 0xCD06 */ bx = 0x8Au; break;
    }
    wr_le_u16(r + 0u, (uint16_t)ax);
    wr_le_u16(r + 4u, (uint16_t)bx);
    wr_le_u16(r + 0x10u, (uint16_t)dx);
    wr_le_u16(r + 8u, (uint16_t)cx);
    if (cx != 0u) return;
    /* 1000:CC65: the (x-3, y-8) match against the eight fixed slots. */
    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *slot = _obj_array + i * OBJ_STRIDE;
        uint32_t type;

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

/* 0xCD0A (behaviour 6, the torch) and 0xCD2E (behaviour 7, the rope/log).
 * 0xCD0A bumps the frame 0x9C..0x9E when `[0x4103] & 3 == 0`, `+8 == 0xFFFF`
 * and `+0xA == 0`, then **falls into 0xCD2E**; 0xCD2E gates on `+8`'s bit 15
 * and dispatches through `[0x6586 + 2*[+0xA]]` - all three slots are 0xCD4A,
 * behaviour 7's move scan. */
static void _obj_walk_rope(uint8_t *r);

/** @brief The torch walk tail. */
static void _obj_walk_torch(uint8_t *r)
{
    if ((_obj_frame & 3u) == 0u && rd_le_u16(r + 8u) == 0xFFFFu &&
        rd_le_u16(r + 0xAu) == 0u) {
        uint32_t f = (uint32_t)(rd_le_u16(r + 4u) + 1u);

        if (f >= 0x9Fu) f = 0x9Cu;
        wr_le_u16(r + 4u, (uint16_t)f);
    }
    _obj_walk_rope(r); /* 1000:CD2C -> 1000:CD2E */
}

/** --- 1000:E039 object_collision_scan: the movers' map-aware move --------
 *
 * Behaviour 5/7's steppers call this after choosing a frame; it is a **move**,
 * not a draw.  `tab` points at the frame's 4-byte group: pair A (dx,dy) at
 * +0/+1, pair B at +2/+3.  `selC`/`selB` are `[0x423C]`/`[0x4238]`, the
 * frame-change deltas `1000:CE88` computes (zero from `1000:CD4A`).
 */
static void _obj_sum_step(const uint8_t *tab, const uint8_t *r, int16_t *ax,
                          int16_t *bx)
{ /* 1000:E295 sum_three_fields */
    *ax = (int16_t)(uint16_t)(tab[0] + rd_le_u16(r + 0u) + rd_le_u16(r + 0xCu));
    *bx = (int16_t)(uint16_t)(tab[1] + rd_le_u16(r + 2u) + rd_le_u16(r + 0xEu));
}

/** 1000:E2B5 collision_land_probe: the landing row for the projected point
 * (AX, BX).  Returns 0 when the caller must take 1000:E194. */
static int32_t _obj_land_probe(int16_t ax, int16_t bx, int16_t *cxOut)
{
    uint16_t a = (uint16_t)ax, b = (uint16_t)bx;
    uint32_t si = (a >> 4) + 2u * (b & 0xFFF0u), sub = a & 0xFu;
    int32_t page;
    int16_t cx;

    if (bx < 0) return 0; /* 1000:E2B8 */
    cx = (int16_t)(0x140 - b);
    if (cx < 0) return 0; /* 1000:E2C4 */
    cx = (int16_t)(b & 0xFFF0u); /* 1000:E2E3: CX = the row base */
    page = (int32_t)(int8_t)(uint8_t)level_page_byte(
        _obj_map_buffer_byte((int32_t)si), sub);
    if (page < 0) { /* 1000:E33A */
        cx = (int16_t)(cx + 0x10);
        page = (int32_t)(int8_t)(uint8_t)level_page_byte(
            _obj_map_buffer_byte((int32_t)(si + 0x20u)), sub);
        if (page < 0) return 0;
    } else if (page == 0) { /* 1000:E304 */
        uint32_t above = _obj_map_buffer_byte((int32_t)(si - 0x20u));

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
    cx = (int16_t)(cx + page); /* 1000:E354 */
    *cxOut = cx;
    return cx >= 0;
}

/* 1000:E27A: hide the record and clear its class byte. */
/** @brief The move scan's kill tail: hide the record and clear its class. */
static void _obj_move_kill(uint8_t *r, uint32_t idx)
{
    r[6] = 0xFFu;
    if (idx >= 9u && idx - 8u < sizeof _obj_slot_class)
        _obj_slot_class[idx - 8u] = 0u;
}

/** @brief The move scan's settle tail (1000:E0D8): apply the step. */
static void _obj_move_settle(uint8_t *r, int16_t cx)
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

/** @brief The move scan's equal-distance landing (1000:E159). */
static void _obj_move_equal(uint8_t *r)
{
    int16_t fi = (int16_t)(rd_le_u16(r + 4u) - 0x58u);
    int32_t step = -1;

    if ((uint16_t)fi < sizeof _obj_land_step)
        step = _obj_land_step[(uint16_t)fi];
    if (step >= 0) {
        wr_le_u16(r + 2u, (uint16_t)((int16_t)rd_le_u16(r + 2u) + step));
        wr_le_u16(r + 4u,
                  (uint16_t)(rd_le_u16(r + 4u) + (fi < 8 ? 1u : 0xFFFFu)));
    }
    wr_le_u16(r + 0xEu, 0u); /* 1000:E17D */
    wr_le_u16(r + 0xAu, 0u);
    wr_le_u16(r + 0xCu, 0u);
}

/** @brief The move scan's impact tail (1000:E194): bounce or step. */
static void _obj_move_impact(uint8_t *r, const uint8_t *tab, uint32_t idx)
{
    int16_t ax, bx;

    /* 1000:E194: the projected point is inside the terrain (or the landing
     * probe failed).  Probe the tile above and the cell's item word. */
    if (rd_le_u16(r + 0xAu) == 0u) {
        uint32_t si, sub, word, hi, lo, k;

        _obj_sum_step(tab, r, &ax, &bx);
        sub = (uint16_t)ax & 0xFu;
        si = ((uint16_t)ax >> 4) + 2u * ((uint16_t)bx & 0xFFF0u);
        if (_obj_map_buffer_byte((int32_t)(si - 0x20u)) >= 0xF0u) {
            _obj_move_kill(r, idx);
            return;
        }
        word = level_item_word(_obj_map_buffer_byte((int32_t)si),
                               (uint16_t)bx & 0xFu);
        hi = (rd_le_u16(r + 0xCu) & 0x8000u) ? 0xFFFFu : 0u;
        lo = hi ? 0u : 0xFFFFu;
        for (k = 0; k < sub; k++) {
            uint32_t carry = hi & 1u;

            hi >>= 1;
            lo = (lo >> 1) | (carry << 15);
        }
        if ((word & lo) != 0u) {
            int16_t vx = (int16_t)rd_le_u16(r + 0xCu), nvx;

            wr_le_u16(r + 0xEu, 0u); /* 1000:E213 */
            if (vx > 0) nvx = vx > 4 ? (int16_t)-(vx - 4) : 0;
            else if (vx < 0) nvx = vx < -4 ? (int16_t)(-vx - 4) : 0;
            else nvx = 0;
            wr_le_u16(r + 0xCu, (uint16_t)nvx); /* 1000:E231 */
            wr_le_u16(r + 0xAu,
                      (uint16_t)((int16_t)rd_le_u16(r + 2u) < 0x60 ? 1 : 2));
            if ((int16_t)rd_le_u16(r + 0xEu) < 3)
                wr_le_u16(r + 0xEu, 3u); /* 1000:E24C */
        }
    }
    { /* 1000:E252: the vertical step, incremented and clamped to 0xF */
        int16_t step = (int16_t)rd_le_u16(r + 0xEu);
        int16_t next = (int16_t)(step + 1);

        if (next >= 0 && next > 0xF) next = 0xF;
        wr_le_u16(r + 0xEu, (uint16_t)next);
        wr_le_u16(r + 2u, (uint16_t)((int16_t)rd_le_u16(r + 2u) + step));
        wr_le_u16(r + 0u, (uint16_t)((int16_t)rd_le_u16(r + 0u) +
                                     (int16_t)rd_le_u16(r + 0xCu)));
    }
}

/** @brief Pair B's projection (1000:E102), gated by the pair sign and frames. */
static void _obj_move_pair_b(uint8_t *r, const uint8_t *tab, int16_t selB,
                             uint32_t idx)
{
    int16_t ax, bx, cx, d;

    if (((uint16_t)tab[2] | ((uint16_t)tab[3] << 8)) & 0x8000u) {
        _obj_move_impact(r, tab, idx);
        return;
    }
    if (!((uint16_t)(1u << ((rd_le_u16(r + 4u) - 0x58u) & 0x1Fu)) &
          OBJ_PAIRB_FRAMES)) {
        _obj_move_impact(r, tab, idx);
        return;
    }
    _obj_sum_step(tab + 2, r, &ax, &bx);
    if (!_obj_land_probe(ax, bx, &cx)) {
        _obj_move_impact(r, tab, idx);
        return;
    }
    d = (int16_t)(cx - bx);
    if (d == 0) {
        _obj_move_equal(r);
        return;
    }
    if (d > 0) {
        _obj_move_impact(r, tab, idx);
        return;
    }
    cx = (int16_t)(-d);
    {
        int16_t t = (int16_t)(selB + cx);

        if (t >= 0 && t > (int16_t)rd_le_u16(r + 0xEu)) {
            _obj_move_impact(r, tab, idx);
            return;
        }
    }
    _obj_move_settle(r, cx);
}

/** @brief The landing scan for one move step (1000:E045..E27A). */
static void _obj_move_scan(uint8_t *r, const uint8_t *tab, int16_t selC,
                           int16_t selB)
{
    uint32_t idx = (uint32_t)((r - _obj_array) / OBJ_STRIDE);
    int16_t ax, bx, cx, d;

    _obj_sum_step(tab, r, &ax, &bx);
    if (ax < 0 || ax >= 0x200 || bx >= 0x150) { /* 1000:E045..E05A */
        _obj_move_kill(r, idx);
        return;
    }
    if (rd_le_u16(r + 0xEu) & 0x8000u) { /* 1000:E05D */
        _obj_move_impact(r, tab, idx);
        return;
    }
    if (!_obj_land_probe(ax, bx, &cx)) { /* 1000:E068 */
        _obj_move_impact(r, tab, idx);
        return;
    }
    d = (int16_t)(cx - bx);
    if (d == 0) cx = 0;
    else if (d < 0) cx = (int16_t)(-d);
    else {
        _obj_move_pair_b(r, tab, selB, idx);
        return;
    }
    { /* 1000:E07B */
        int16_t t = (int16_t)(selC + cx);

        if (t >= 0 && t > (int16_t)rd_le_u16(r + 0xEu)) {
            _obj_move_pair_b(r, tab, selB, idx);
            return;
        }
    }
    if (rd_le_u16(r + 0xEu) != 0u) _obj_driver_call(5); /* 1000:E097 */
    { /* 1000:E0A7: the settled frame, and y by its pair-A dy change */
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
    _obj_move_settle(r, cx);
}


/** 1000:CE88 compute_object_selectors: the pair-A/pair-B `dy` changes between
 * the old and the new frame, stored into [0x423C]/[0x4238]. */
static void _obj_selectors(uint16_t oldFrame, uint16_t newFrame, int16_t *selC,
                           int16_t *selB)
{
    int32_t o = (int16_t)(oldFrame - 0x58u) * 4;
    int32_t n = (int16_t)(newFrame - 0x58u) * 4;

    *selC = (int16_t)((int8_t)_obj_mover_tab[o + 1] -
                      (int8_t)_obj_mover_tab[n + 1]);
    *selB = (int16_t)((int8_t)_obj_mover_tab[o + 3] -
                      (int8_t)_obj_mover_tab[n + 3]);
}

/** 1000:CD2E dispatch_handler_table_6586 + 1000:CD4A
 * reset_frame_state_and_scan_collisions: behaviour 7's move.  The table holds
 * 0xCD4A three times, so the `+0xA` state only selects the same body. */
static void _obj_walk_rope(uint8_t *r)
{
    uint32_t f;
    const uint8_t *tab;

    if (!(rd_le_u16(r + 8u) & 0x8000u)) return; /* 1000:CD33 */
    switch (rd_le_u16(r + 0xAu)) { /* 0x6586 holds CD4A x3 */
    case 0u:
    case 1u:
    case 2u: break;
    default: return; /* D-101: past the three-entry table */
    }
    if (_obj_frame & 1u) return; /* 1000:CD4A: even frames */
    f = rd_le_u16(r + 4u);
    if (f < 0x96u || f > 0xA5u) return; /* D-101: past the 0x658C table */
    tab = _obj_rope_tab + 4u * (f - 0x96u);
    _obj_move_scan(r, tab, 0, 0);
}

/** 1000:CD71 (behaviour 5): the mover's collision passes ----------------
 *
 * The handler first runs the stepper (`[0x65CC + 2*[+0xA]]`, 0xCDC7/0xCE38/
 * 0xCE3D) - which moves the record through 0xE039 - then projects the record
 * again by the 0x6612 (dx,dy) pair and hands the projected AX/BX to three
 * passes.  The passes write only when they hit.  0x58D1 (record-vs-entity,
 * testing the entity's REX sprite pixels) is `_obj_mover_pass_entity` below;
 * 0x5A2F and 0x5B3E are `_obj_mover_pass_records` / `_obj_mover_pass_placed`.
 */
static void _obj_mover_pass_records(uint8_t *r, int32_t ax, int32_t bx)
{
    for (uint32_t i = 0; i <= _obj_live_count; i++) {
        uint8_t *t = _obj_array + i * OBJ_STRIDE;
        uint8_t *beh = _obj_slot_class + (i - 8u);
        int32_t dx, dy;

        if (i < OBJ_LIVE0 + 1u) continue;
        if (*beh != 8u) continue; /* 1000:5A41 compares the whole byte */
        dx = (int16_t)rd_le_u16(t + 0u) - ax;
        if (!(dx < 0 && dx + 0x10 >= 0)) continue; /* 1000:5A55/0x5A5D */
        dy = (int16_t)rd_le_u16(t + 2u) - bx;
        if (!(dy < 0 && dy + 0x1Au >= 0)) continue; /* 1000:5A66/0x5A6E */
        if (rd_le_u16(r + 0xCu) != 0u || (rd_le_u16(r + 0xEu) & 0x8000u) ||
            rd_le_u16(r + 0xEu) > 1u) {
            uint8_t cls;

            wr_le_u16(t + 0u, (uint16_t)(rd_le_u16(t + 0u) - 8u));
            wr_le_u16(t + 4u, 0x0028u);
            wr_le_u16(t + 8u, 0x0003u);
            wr_le_u16(t + 0xAu, 6u);
            cls = t[7];
            if (cls) {
                _obj_class_byte_set(cls, 0u); /* 0x3E5F + cls - 1 */
                if (cls >= 1u && OBJ_LIVE0 + cls <= _obj_live_count)
                    _obj_array[(OBJ_LIVE0 + cls) * OBJ_STRIDE + 6] = 0xFFu;
            }
            *beh = 0u;
            r[6] = 0xFFu;
            _obj_driver_call(0x0Cu);
            return;
        }
        if (t[7] == 0u) {
            /* 1000:5A9D..0x5AD6: clear the ACTING record's +6/+8/+0xA/+0xC,
             * OR 0x80 into the ACTING record's class byte and link the
             * candidate to it by that class ordinal; the scan then moves on
             * to the next record (0x5B31). */
            uint32_t ord = (uint32_t)((r - _obj_array) / OBJ_STRIDE) - 8u;

            wr_le_u16(r + 6u, 0u);
            wr_le_u16(r + 8u, 0u);
            wr_le_u16(r + 0xAu, 0u);
            wr_le_u16(r + 0xCu, 0u);
            _obj_slot_class[ord] |= 0x80u;
            t[7] = (uint8_t)ord;
        }
    }
}

/** 0x58D1: the record-vs-entity pass.  Walks the entity array, keeps the
 * entities whose handler is < 2, and tests the entity's REX sprite pixel at
 * the record's projected offset (the 0xE0/0x150/0x2+4a tables via
 * entity_frame_info). */
static void _obj_mover_pass_entity(uint8_t *r, int32_t ax, int32_t bx)
{
    uint32_t n = entity_count();

    if (rd_le_u16(r + 0xCu) == 0u && rd_le_u16(r + 0xEu) == 0u) return;
    for (uint32_t i = 0; i < n; i++) {
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
            r[6] = 0xFFu; /* 1000:59F2: SI = record+6 */
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

/** @brief The mover pass over the placed records. */
static void _obj_mover_pass_placed(uint8_t *r, int32_t ax, int32_t bx)
{
    uint32_t i;

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
        if (!(dx < 0 && dx + 0x10 >= 0)) continue; /* 1000:5B77/0x5B7F */
        dy = (int16_t)rd_le_u16(slot + 2u) - bx;
        if (!(dy - 2 < 0 && dy + 0x0C >= 0)) continue; /* 1000:5B89/0x5B91 */
        type = rd_le_u16(slot + 8u);
        if (type < 16u && _obj_type_overlaps[type]) continue;
        (void)object_stack_scan_dissolve(i); /* 1000:F453, AX = 5 */
        _obj_slot_class[(uint32_t)((r - _obj_array) / OBJ_STRIDE) - 8u] = 0u;
        r[6] = 0xFFu;
        cls = slot[7];
        if (cls) {
            _obj_class_byte_set(cls, 0u); /* 0x3E5F + cls - 1 */
            if (cls >= 1u && OBJ_LIVE0 + cls <= _obj_live_count)
                _obj_array[(OBJ_LIVE0 + cls) * OBJ_STRIDE + 6] = 0xFFu;
        }
        wr_le_u16(slot + 0u, (uint16_t)(rd_le_u16(slot + 0u) - 8u));
        wr_le_u16(slot + 4u, 0x0028u);
        wr_le_u16(slot + 8u, 0x0016u);
        wr_le_u16(slot + 0xAu, 5u);
        _obj_driver_call(4);
        return;
    }
}

/** 0xCDC7 / 0xCE38 / 0xCE3D: the frame/direction stepper (odd frames only).
 * It picks the frame, derives the pair-A/pair-B `dy` deltas through 0xCE88 and
 * then runs the move scan 0xE039 with the frame's `0x6612` group. */
static void _obj_mover_stepper(uint8_t *r, uint32_t mode)
{
    uint16_t oldFrame, newFrame;
    int16_t selC, selB;
    const uint8_t *tab;

    if (!(_obj_frame & 1u)) return;
    oldFrame = rd_le_u16(r + 4u);
    newFrame = oldFrame;
    if (mode != 0u) { /* 0xCE38 / 0xCE3D -> 0xCE40 */
        int32_t b = (int16_t)((mode == 1u ? 1 : -1) + oldFrame);

        if (b >= 0x68) b = 0x58;
        if (b <= 0x57) b = 0x67;
        newFrame = (uint16_t)b;
        wr_le_u16(r + 4u, newFrame);
        { /* 0xCE63: the +0xC step toward zero */
            int32_t c = (int16_t)rd_le_u16(r + 0xCu);

            if (c != 0) c += (c < 0) ? 1 : -1;
            wr_le_u16(r + 0xCu, (uint16_t)c);
        }
    } else { /* 0xCDC7: the 8x8 direction table */
        int32_t bx = (int16_t)rd_le_u16(r + 0xCu),
                cx = (int16_t)rd_le_u16(r + 0xEu);

        if (bx == 0) {
            if (cx != 0) newFrame = (uint16_t)(cx > 0 ? 0x60 : 0x58);
        } else if (cx == 0) {
            newFrame = (uint16_t)(bx > 0 ? 0x5C : 0x64);
        } else { /* 1000:CDF4: idx = 8*f([+0xE]) + f([+0xC]) */
            uint32_t idx =
                (uint32_t)(((cx + 0x10) >> 2) * 8 + ((bx + 0x10) >> 2));

            if (idx < sizeof _obj_mover_dir) newFrame = _obj_mover_dir[idx];
        }
        wr_le_u16(r + 4u, newFrame);
    }
    _obj_selectors(oldFrame, newFrame, &selC, &selB); /* 0xCE88 */
    if (newFrame < 0x58u || newFrame > 0x67u) return; /* D-101 */
    tab = _obj_mover_tab + 4u * (newFrame - 0x58u);
    _obj_move_scan(r, tab, selC, selB); /* 0xCE34 / 0xCE84 */
}

/** 1000:CD71: the behaviour-5 handler. */
static void _obj_walk_mover(uint8_t *r)
{
    uint32_t f, mode;

    if (!(rd_le_u16(r + 8u) & 0x8000u)) return;
    mode = rd_le_u16(r + 0xAu);
    if (mode <= 2u) _obj_mover_stepper(r, mode);
    f = rd_le_u16(r + 4u);
    if (f < 0x58u || f > 0x67u) return;
    {
        uint32_t ix = (f - 0x58u) * 4u;
        int32_t dx = (int8_t)_obj_mover_tab[ix];
        int32_t dy = (int8_t)_obj_mover_tab[ix + 1u];
        int32_t ax = (int16_t)rd_le_u16(r + 0u) + dx;
        int32_t bx = (int16_t)rd_le_u16(r + 2u) + dy;

        _obj_mover_pass_entity(r, ax, bx);
        _obj_mover_pass_records(r, ax, bx);
        _obj_mover_pass_placed(r, ax, bx);
    }
}

/** --- 0xC57E (behaviour 8): the 7-state stack machine -------------------
 *
 * The record's +0xA selects one of seven states through 0x654E.  States 0/1
 * walk (vx +-1) with a 0xC62C terrain/0x1FD6 probe and stop on a blocked
 * probe or item word (0xC6AD toggles the direction and restores the frame
 * 0xE0/0xD4); 2/3 play the 0xC615 animation (frames 0x116..0x11A); after
 * either, 0xC6B8 checks the eight fixed slots (frame 0xDC/0xE8, +8 = 2,
 * +0xA = 4/5); 4/5 land through 0xC71E; 6 is the exit (frame 0x2E ends it).
 */
static void _obj_stack8_fail(uint8_t *r, uint32_t frame)
{
    r[0xAu] ^= 1u;
    wr_le_u16(r + 4u, (uint16_t)frame);
}

/** @brief The stack-8 slot pass. */
static void _obj_stack8_slots(uint8_t *r)
{
    uint32_t i;

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

/** @brief The stack-8 mover. */
static void _obj_stack8_move(uint8_t *r, int32_t vx, int32_t vy,
                             uint32_t stopFrame, uint32_t endFrame)
{
    const uint8_t *map = level_map();
    int32_t land, bx, ax, recY;
    uint32_t tile, word, rowFine;

    {
        uint32_t probeCell;

        land = _obj_land_y((int32_t)(int16_t)rd_le_u16(r + 0u) + vy + 8, 0x18,
                           &probeCell);
        (void)probeCell;
    }
    if (land < 0) {
        _obj_stack8_fail(r, stopFrame);
        return;
    }
    bx = land - 0x18;
    ax = (int32_t)(int16_t)rd_le_u16(r + 0u) + vx;
    rowFine = (uint32_t)(bx + 0xC) & 0xFu;
    {
        uint32_t ci = ((uint32_t)(bx + 0xC) >> 4) * 32u + ((uint32_t)ax >> 4);

        tile = (map && ci < 640u) ? map[ci] : 0xFFu;
    }
    word = level_item_word(tile, rowFine);
    if (word != 0u) {
        _obj_stack8_fail(r, stopFrame);
        return;
    }
    recY = (int32_t)(int16_t)rd_le_u16(r + 2u) - bx;
    if (recY < 0 && bx + 8 < 0) {
        _obj_stack8_fail(r, stopFrame);
        return;
    }
    wr_le_u16(r + 0u, (uint16_t)(rd_le_u16(r + 0u) + (uint16_t)vy));
    wr_le_u16(r + 2u, (uint16_t)bx);
    {
        uint32_t s = rd_le_u16(r + 8u);

        if (s == 0u) {
            uint32_t f;

            wr_le_u16(r + 8u, 3u);
            f = (uint32_t)(rd_le_u16(r + 4u) + 1u);
            if (f == endFrame) f -= 8u;
            wr_le_u16(r + 4u, (uint16_t)f);
        } else {
            wr_le_u16(r + 8u, (uint16_t)(s - 1u));
        }
    }
}

/** @brief The stack-8 animation tail. */
static void _obj_stack8_anim(uint8_t *r, uint32_t endFrame)
{
    uint32_t s = rd_le_u16(r + 8u);

    if (s != 0u) {
        wr_le_u16(r + 8u, s - 1u);
        return;
    }
    wr_le_u16(r + 8u, 6u);
    {
        uint32_t f = (uint32_t)(rd_le_u16(r + 4u) + 1u);

        wr_le_u16(r + 4u, (uint16_t)f);
        if (f == endFrame) wr_le_u16(r + 4u, (uint16_t)(f - 4u));
    }
}

/** @brief The stack-8 landing. */
static void _obj_stack8_land(uint8_t *r, uint32_t bx, int32_t ax, int32_t bx2,
                             uint32_t idx)
{
    uint32_t s = rd_le_u16(r + 8u);

    s = (uint32_t)(s - 1u);
    wr_le_u16(r + 8u, (uint16_t)s);
    if ((int16_t)s >= 0) return;
    wr_le_u16(r + 8u, 2u);
    wr_le_u16(r + 4u, (uint16_t)(rd_le_u16(r + 4u) + 1u));
    if (bx == rd_le_u16(r + 4u)) {
        uint32_t cls = r[7];

        r[7] = 0u;
        if (cls) _obj_class_byte_set(cls, _obj_class_byte_at(cls) & 0x3Fu);
        if (cls >= 1u && OBJ_LIVE0 + cls <= _obj_live_count) {
            uint8_t *t = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE;

            wr_le_u16(t + 8u, 0xFFFFu);
            wr_le_u16(t + 0xAu, 0u);
            wr_le_u16(t + 0xCu, (uint16_t)ax);
            wr_le_u16(t + 0xEu, (uint16_t)bx2);
        }
        _obj_driver_call(3);
    }
    if (bx + 3u == rd_le_u16(r + 4u)) {
        wr_le_u16(r + 4u, (uint16_t)(rd_le_u16(r + 4u) - 0xCu));
        wr_le_u16(r + 0xAu, (uint16_t)(rd_le_u16(r + 0xAu) - 4u));
    }
    (void)idx;
}

/** @brief The stack walk tail. */
static void _obj_walk_stack(uint8_t *r, uint32_t idx)
{
    switch (rd_le_u16(r + 0xAu)) {
    case 0u:
        _obj_stack8_move(r, 0x11, 1, 0xE0u, 0xDCu);
        _obj_stack8_slots(r);
        break;
    case 1u:
        _obj_stack8_move(r, -1, -1, 0xD4u, 0xE8u);
        _obj_stack8_slots(r);
        break;
    case 2u:
        _obj_stack8_anim(r, 0x11Au);
        _obj_stack8_slots(r);
        break;
    case 3u:
        _obj_stack8_anim(r, 0x11Eu);
        _obj_stack8_slots(r);
        break;
    case 4u: _obj_stack8_land(r, 0xDDu, 0xA, -3, idx); break;
    case 5u: _obj_stack8_land(r, 0xE9u, -0xA, -3, idx); break;
    default: { /* state 6, the exit */
        uint32_t s = rd_le_u16(r + 8u);

        s = (uint32_t)(s - 1u);
        wr_le_u16(r + 8u, (uint16_t)s);
        if ((int16_t)s >= 0) break;
        wr_le_u16(r + 8u, 3u);
        {
            uint32_t f = (uint32_t)(rd_le_u16(r + 4u) + 1u);

            wr_le_u16(r + 4u, (uint16_t)f);
            if (f == 0x2Eu) {
                _obj_slot_class[idx - 8u] = 0u;
                r[6] = 0xFFu;
            }
        }
        break;
    }
    }
}

/* [0x4238] / [0x423C]: the scratch position `advance_position_snap_row`
 * accumulates, and [0x4244]-1: the acting record's class byte. */
static int16_t _obj_scan_x, _obj_scan_y;
static uint8_t *_obj_act_class;

static int32_t _obj_bg_mark(const uint8_t *actor);

/** @brief Cull or return the projectile velocity. */
static int32_t _obj_proj_cull(int32_t vx, int32_t x)
{ /* 1000:D1E3 cull_entity_offscreen */
    int32_t off;

    if (vx < 0) {
        off = (-vx) >> 3;
        if (x - off < 0) return 0;
    } else {
        off = vx >> 3;
        if (x + off > 0x1EF) return 0;
    }
    return vx;
}

/* 1000:D41B advance_position_snap_row: BX/CX are deltas added to the scratch
 * position [0x4238]/[0x423C]; the routine snaps the row, applies the tile's
 * page byte and the water/drown rule.  On success AX = [0x423C]; on failure
 * -1 (the caller's SF).
 *
 * The water arm (0xD4D4) and the off-page `>= 0x160` arm (0xD5D2) end
 * `POP DI; POP BP; ADD SP,2; RET`.  Both callers leave their own return
 * address on top of the stack (0xCEC3 pushes nothing before these calls and
 * 0xD34E pops its four registers first), so the `ADD SP,2` skips d41b's
 * return address and RET returns one level up: the rest of the immediate
 * caller is abandoned.  It is not a corrupt return and the stack
 * balance is the normal one.  -2 signals that early-out to the callers. */
/** @brief 1000:D584: off the page; from 0x160 the target takes the crash state. */
static int32_t _obj_proj_offpage(uint8_t *r)
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
            _obj_driver_call(4);
        }
        _obj_scan_y = -1;
        return -2; /* 0xD5D2: return one level up */
    }
    _obj_scan_y = -1;
    return -1;
}

/** @brief 1000:D41B: advance the scratch position and snap it to the row. */
static int32_t _obj_proj_advance(uint8_t *r, int16_t bx, int16_t cx)
{
    uint32_t idx, sub;
    int32_t page;
    uint8_t tile;

    _obj_scan_y = (int16_t)(0x10 + cx);
    _obj_scan_x = (int16_t)(_obj_scan_x + bx);
    if ((uint16_t)_obj_scan_y >= 0x140u) /* 1000:D42E */
        return _obj_proj_offpage(r);
    _obj_scan_y &= (int16_t)0xFFF0;
    sub = (uint16_t)_obj_scan_x & 0xFu;
    idx = 2u * (uint16_t)_obj_scan_y + ((uint16_t)_obj_scan_x >> 4);
    tile = (uint8_t)_obj_map_buffer_byte((int32_t)(idx - 0x20u));
    if (tile >= 0xF0u && tile <= 0xF2u) { /* 1000:D46F: the water branch */
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
        _obj_driver_call(6);
        return -2; /* 0xD4D4: return one level up */
    }
    page = (int32_t)(int8_t)(uint8_t)level_page_byte(
        _obj_map_buffer_byte((int32_t)idx), sub); /* 1000:D4DA */
    if (page < 0) { /* 1000:D50F */
        if ((uint16_t)_obj_scan_y >= 0x130u) return _obj_proj_offpage(r);
        page = (int32_t)(int8_t)(uint8_t)level_page_byte(
            _obj_map_buffer_byte((int32_t)(idx + 0x20u)), sub);
        if (page < 0) { /* 1000:D535 */
            _obj_scan_y = -1;
            return -1;
        }
        _obj_scan_y =
            (int16_t)(_obj_scan_y + page); /* 1000:D4FC: no row step */
        return _obj_scan_y;
    }
    if (page == 0) { /* 1000:D540: the row above decides */
        uint32_t above = _obj_map_buffer_byte((int32_t)(idx - 0x20u));

        page = (int32_t)(int8_t)(uint8_t)level_page_byte(above, sub);
        if (page < 0) { /* 1000:D565: re-read the cell, then one row up */
            page = (int32_t)(int8_t)(uint8_t)level_page_byte(
                _obj_map_buffer_byte((int32_t)idx), sub);
            _obj_scan_y = (int16_t)(_obj_scan_y - 0x10 + page);
            return _obj_scan_y;
        }
        _obj_scan_y = (int16_t)(_obj_scan_y - 0x10); /* 1000:D55E */
    }
    _obj_scan_y = (int16_t)(_obj_scan_y - 0x10 + page); /* 1000:D4F7 */
    return _obj_scan_y;
}

/** 1000:D3E3 bounce_value_4230: reflect the velocity and lose 0x20. */
static void _obj_proj_bounce(int16_t *vx, int16_t *vy)
{
    int16_t v = (int16_t)(-*vx);

    if (v >= 0) {
        v = (int16_t)(v - 0x20);
        if (v < 0) v = 0;
    } else {
        v = (int16_t)(v + 0x20);
        if (v < 0) v = 0;
    }
    *vx = v;
    if (*vy != 0 && (uint16_t)*vy < 0x10u) *vy = 0x10;
}

/** 1000:D34E: the projectile's cell/item probe and terrain bounce.  Despite the
 * historical name it draws nothing. */
static void _obj_proj_burst(uint8_t *r, int16_t bx, int16_t cx, int16_t *vx,
                            int16_t *vy)
{
    _obj_scan_x = (int16_t)(_obj_scan_x + bx);
    _obj_scan_y = (int16_t)(_obj_scan_y + cx);
    {
        int16_t sx = _obj_scan_x, sy = _obj_scan_y;
        uint32_t si = 2u * ((uint16_t)sy & 0xFFF0u) + ((uint16_t)sx >> 4);
        uint32_t sub = (uint16_t)sx & 0xFu;
        uint32_t word = level_item_word(_obj_map_buffer_byte((int32_t)si),
                                        (uint16_t)sy & 0xFu);
        uint32_t hi = (*vx < 0) ? 0xFFFFu : 0u;
        uint32_t lo = hi ? 0u : 0xFFFFu, k;

        for (k = 0; k < sub; k++) {
            uint32_t carry = hi & 1u;

            hi >>= 1;
            lo = (lo >> 1) | (carry << 15);
        }
        if ((word & lo) != 0u) { /* 1000:D3B3 */
            _obj_proj_bounce(vx, vy);
            return;
        }
    }
    _obj_scan_x = (*vx < 0) ? 0 : 0x10; /* 1000:D3B9 */
    if (_obj_proj_advance(r, bx, cx) < 0) return;
    {
        int32_t d = (int16_t)(cx - _obj_scan_y); /* 1000:D3D2 */

        if (d < 0) return;
        if (d - 0xC < 0) return;
    }
    _obj_proj_bounce(vx, vy);
}

/** 1000:D31A shift_bit_into_delayed_byte: returns the caller's ZF (1 = the
 * counter path ran). */
static int32_t _obj_hit_delay(uint8_t *r, uint16_t *s38, uint16_t *s3c)
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

/** The five ops of `202F:0x666B` (1000:D228/D2AE/D2C0/D2DA/D2F4): the hit
 * record's frame/state animation.  `r` is the projectile (its +7 is the state
 * byte), `t` the hit record. */
static void _obj_hit_dispatch(uint8_t *r, uint8_t *t, int16_t frame, int16_t vx,
                              int16_t vy)
{
    uint16_t s38 = (uint16_t)((r[7] & 7u) * 4u); /* [0x4238] */
    uint16_t s3c = (uint16_t)(r[7] >> 3); /* [0x423C] */
    uint32_t op = (uint32_t)(r[7] & 7u);

    if (op == 0u) { /* 1000:D228 hit_op_frame_default */
        s38 = 0x41u;
        if (vy == 0) s38 = (uint16_t)(frame + 4);
    } else if (op == 1u) { /* 1000:D2AE hit_op_bit_test_42 */
        if (!_obj_hit_delay(r, &s38, &s3c)) {
            r[7] = 0u;
            return;
        }
        s38 = 0x42u;
    } else if (op == 2u) { /* 1000:D2C0 hit_op_bit_test_4d */
        if (_obj_hit_delay(r, &s38, &s3c)) return;
        wr_le_u16(t + 4u, (uint16_t)(rd_le_u16(t + 4u) + 1u));
        if (rd_le_u16(t + 4u) == 0x4Du) {
            r[7] = 0u;
            wr_le_u16(t + 4u, 0x43u);
            return;
        }
        r[7] = 0x3Au;
        return;
    } else if (op == 3u) { /* 1000:D2DA hit_op_bit_test_50 */
        if (_obj_hit_delay(r, &s38, &s3c)) return;
        wr_le_u16(t + 4u, (uint16_t)(rd_le_u16(t + 4u) + 1u));
        if (rd_le_u16(t + 4u) == 0x50u) {
            r[7] = 0u;
            wr_le_u16(t + 4u, 0x3Cu);
            return;
        }
        r[7] = 0x3Bu;
        return;
    } else if (op == 4u) { /* 1000:D2F4 hit_op_terminate_50 */
        if (vx == 0) {
            r[7] = 0u;
            wr_le_u16(t + 4u, 0x50u);
            return;
        }
        if (_obj_hit_delay(r, &s38, &s3c)) return;
        r[7] = 0x1Cu;
        wr_le_u16(t + 4u, (uint16_t)(rd_le_u16(t + 4u) + 1u));
        if (rd_le_u16(t + 4u) == 0x50u) wr_le_u16(t + 4u, 0x4Au);
        return;
    } else {
        return; /* D-101: past the five-word table */
    }
    /* 1000:D23F: the common tail */
    if (vx == 0) { /* 1000:D294 */
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
    if ((int16_t)s3c < 0) return; /* 1000:D261 */
    if (s38 >= 0x16u || s3c >= 0x16u) return; /* D-101: the 0x667F table */
    {
        uint8_t a = _obj_hit_frame[s38], b = _obj_hit_frame[s3c];

        if (a == b) return; /* 1000:D278 */
        if (a != 0u) {
            r[7] = 0x3Au;
            wr_le_u16(t + 4u, 0x4Au);
        } else {
            r[7] = 0x3Bu;
            wr_le_u16(t + 4u, 0x4Du);
        }
    }
}

/* 0xCEC3 entity_update_from_map: the behaviour-4 projectile ------------------
 *
 * Phase 1 (vy == 0) rolls along the terrain: the tile at (x+8, y+0x10) gives a
 * signed slope ([0x1FD4]) that is added to vx with a +-1 step ([0x6695]); the
 * fuse +0xC is recomputed as 8 - (|vx|>>2) when it expires and the frame cycles
 * 0x38..0x3B.  Phase 2 (vy != 0) is the fall: the signed curve at 0x6652
 * (indexed by vy, clamped by the byte compare at 0x19) is added to y, and the
 * landing test resolves the target record through +0xE (the 0x3E24 pointer
 * table), writing its 0x28/0/0x16/5 state and dispatching the 0x666B op. */
/** @brief 0xCEC3: the behaviour-4 projectile, phase 1 roll and phase 2 fall. */
static void _obj_walk_projectile(uint8_t *r, uint32_t idx)
{
    int16_t x = (int16_t)rd_le_u16(r + 0u);
    int16_t y = (int16_t)rd_le_u16(r + 2u);
    int16_t frame = (int16_t)rd_le_u16(r + 4u);
    int16_t vx, vy, cxreg;

    (void)idx;
    if (r[6] & 0x40u) return; /* 1000:CED6 */
    vx = (int16_t)rd_le_u16(r + 8u);
    vy = (int16_t)rd_le_u16(r + 0xAu);
    if (vy == 0) { /* phase 1 */
        int16_t sx = (int16_t)(x + 8);
        int16_t sy = (int16_t)((y + 0x10) & ~0xF);
        uint32_t ci = 2u * (uint16_t)sy + ((uint16_t)sx >> 4);
        int16_t slope = (int16_t)level_slope_byte(
            _obj_map_buffer_byte((int32_t)ci), (uint16_t)sx & 0xFu);
        int16_t step = 1;

        vx = (int16_t)(vx + slope); /* 1000:CF64 */
        if (vx != 0) {
            if (vx >= 0) step = -1; /* 1000:CF6C */
            vx = (int16_t)(vx + step);
            if (vx != 0) {
                int16_t fuse = (int16_t)(rd_le_u16(r + 0xCu) - 1u);

                wr_le_u16(r + 0xCu, (uint16_t)fuse); /* 1000:CF79 */
                if (fuse < 0) {
                    int16_t sign = (vx < 0) ? -1 : 1;
                    int16_t a = (int16_t)(vx < 0 ? -vx : vx);

                    a = (int16_t)(-(a >> 2) + 8);
                    wr_le_u16(r + 0xCu, (uint16_t)a);
                    frame = (int16_t)(frame + sign); /* 1000:CFAD */
                    if ((uint16_t)frame <= 0x37u) frame = 0x3B;
                    else if ((uint16_t)frame >= 0x3Cu) frame = 0x38;
                }
            }
        }
        vx = (int16_t)_obj_proj_cull(vx, x); /* 1000:CFC3 */
        _obj_scan_x = 8;
        {
            int32_t rc = _obj_proj_advance(r, x, y);

            if (rc == -2) return; /* 0xD4D4/0xD5D2: leave 1000:CEC3 */
            if (rc < 0) { /* 1000:D030: fall into phase 2 */
                vy = 0x10;
                wr_le_u16(r + 0x10u, (uint16_t)y);
            } else {
                cxreg = _obj_scan_y; /* 1000:CFD1 */
                _obj_scan_x = 0;
                _obj_scan_y = 0xC;
                if (vx >= 0) _obj_scan_x = 0x10;
                _obj_proj_burst(r, x, cxreg, &vx, &vy); /* 1000:CFEF */
                { /* 1000:CFF2 */
                    int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

                    if (tgt >= 0) {
                        _obj_foot_x = (uint16_t)(x + 8);
                        _obj_foot_y = (uint16_t)(cxreg + 0x10);
                        if (_obj_bg_mark(r) == 0) {
                            _obj_settled[tgt][0] = (uint16_t)x;
                            _obj_settled[tgt][1] = (uint16_t)(cxreg - 8);
                        }
                    }
                }
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
    vx = (int16_t)_obj_proj_cull(vx, x); /* 1000:D039 */
    _obj_scan_x = 4;
    if ((uint8_t)vy < 0x19u)
        _obj_scan_x = _obj_proj_curve[(uint8_t)vy]; /* 1000:D049 */
    {
        int16_t curve = (int16_t)(int8_t)(uint8_t)_obj_scan_x;

        y = (int16_t)(y + curve); /* 1000:D065 */
        cxreg = y;
        if (curve >= 0) { /* 1000:D069 */
            int32_t tail = 135;

            _obj_scan_x = 8;
            {
                int32_t rc = _obj_proj_advance(r, x, y);

                if (rc == -2) return; /* 0xD4D4/0xD5D2: leave 1000:CEC3 */
                if (rc >= 0 && _obj_scan_y <= y) {
                    int16_t prev = (int16_t)rd_le_u16(r + 0x10u);

                    _obj_scan_x = prev;
                    if (prev >= 0) {
                        _obj_scan_x = (int16_t)(_obj_scan_x - _obj_scan_y);
                        if ((int16_t)_obj_scan_x >= 0 &&
                            (uint16_t)_obj_scan_x > 0x10u)
                            tail = 135;
                    }
                    if (tail != 135) {
                        _obj_scan_x = (int16_t)-_obj_scan_x;
                        if ((int16_t)_obj_scan_x < 0 ||
                            (uint16_t)_obj_scan_x < 0x40u) {
                            tail = 195;
                        } else {
                            int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

                            _obj_scan_x = (int16_t)tgt;
                            if (tgt < 0) {
                                tail = 195;
                            } else { /* 1000:D0B5: the hit */
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
                                tail = 127;
                            }
                        }
                    }
                }
            }
            if (tail == 195) { /* 1000:D11B */
                vy = -1;
                _obj_scan_x = 5;
                tail = 127;
            }
            if (tail == 127) { /* 1000:D127 */
                cxreg = _obj_scan_y;
                r[7] = 0x99u;
                _obj_driver_call((uint32_t)(uint16_t)_obj_scan_x);
            }
            /* 1000:D135 */
            vy = (int16_t)(vy + 1);
            _obj_scan_y = 0x10;
        } else { /* 1000:D141 */
            vy = (int16_t)(vy + 1);
            _obj_scan_y = 0xC;
        }
    }
    {
        _obj_scan_x = (vx < 0) ? 0 : 8; /* 1000:D14B */
        _obj_proj_burst(r, x, cxreg, &vx, &vy); /* 1000:D15F */
    }
    { /* 1000:D162: the hit record's position and the op dispatch */
        int32_t tgt = (int16_t)rd_le_u16(r + 0xEu);

        if (tgt >= 0) {
            uint8_t *t = _obj_array + (uint32_t)tgt * OBJ_STRIDE;

            wr_le_u16(t + 0u, (uint16_t)(x > 8 ? x - 8 : 0));
            wr_le_u16(t + 2u, (uint16_t)(cxreg - 0x10));
            _obj_hit_dispatch(r, t, frame, vx, vy);
        }
    }
    /* 1000:D1C0: the writeback */
    wr_le_u16(r + 0xAu, (uint16_t)vy);
    wr_le_u16(r + 8u, (uint16_t)vx);
    wr_le_u16(r + 4u, (uint16_t)frame);
    wr_le_u16(r + 2u, (uint16_t)cxreg);
    wr_le_u16(r + 0u, (uint16_t)x);
}


/** 0xC444 (behaviour 9): the marker use.  0xC541 first animates the map cell
 * the record points at (+8) every 4th frame; then the tile-aligned record
 * position is matched against the eight fixed slots, and a free slot hands
 * the record over (x -= 8, frame 0x28, +8 = 0x16, +0xA = 5, effect 4). */
static void _obj_walk_marker(uint8_t *r)
{
    const uint8_t *map = level_map();
    uint32_t i;

    if ((_obj_frame & 3u) == 0u) { /* 1000:C541 */
        uint32_t f = rd_le_u16(r + 4u);

        if (_obj_marker_mode) {
            if (f == 0x26u) {
                wr_le_u16(r + 4u, f + 1u);
                return;
            }
            r[6] |= 0x80u;
            if (map && rd_le_u16(r + 8u) < 640u)
                ((uint8_t *)map)[rd_le_u16(r + 8u)] = 0u;
            return;
        }
        if (r[6] & 0x80u) {
            r[6] = 0u;
            return;
        }
        if (f == 0x26u) return;
        wr_le_u16(r + 4u, f - 1u);
        if (map && rd_le_u16(r + 8u) < 640u)
            ((uint8_t *)map)[rd_le_u16(r + 8u)] = 0xBBu;
        return;
    }
    if (r[6] & 0x80u) return;
    {
        uint32_t mx = (uint32_t)(rd_le_u16(r + 0u) + 8u) & ~0xFu;
        uint32_t my = (uint32_t)(rd_le_u16(r + 2u) + 8u) & ~0xFu;

        for (i = 0; i < OBJ_STATIC; i++) {
            uint8_t *slot = _obj_array + i * OBJ_STRIDE;
            uint32_t sx, sy, type;

            sx = (uint32_t)(rd_le_u16(slot + 0u) + 8u) & ~0xFu;
            if (rd_le_u16(slot + 8u) == 0x0Fu)
                sx = (uint32_t)(rd_le_u16(slot + 0u) + 0x10u) & ~0xFu;
            sy = (uint32_t)(rd_le_u16(slot + 2u) + 0x0Cu) & ~0xFu;
            if (sx != mx || sy != my) continue;
            if (slot[6] & 0x80u) continue;
            type = rd_le_u16(slot + 8u);
            if (type < 16u && _obj_type_overlaps[type]) continue;
            object_stack_scan_dissolve(i); /* 1000:F453, DX = 5 */
            wr_le_u16(r + 0u, (uint16_t)(rd_le_u16(r + 0u) - 8u));
            wr_le_u16(r + 4u, 0x0028u);
            wr_le_u16(r + 8u, 0x0016u);
            wr_le_u16(r + 0xAu, 5u);
            _obj_driver_call(4); /* 1000:83B7, AX = 4 */
            {
                uint32_t gx = (uint32_t)(_obj_settled[i][0] + 8u) & ~0xFu;
                uint32_t gy = (uint32_t)(_obj_settled[i][1] + 0x0Cu) & ~0xFu;

                if (gx == mx && gy == my) _obj_settled[i][0] += 0x10u;
            }
            return;
        }
    }
}

/** @brief The placed-object update list. */
void object_update_list(void)
{
    for (uint32_t i = OBJ_LIVE0 + 1u; i <= _obj_live_count; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint32_t beh = (uint32_t)(_obj_slot_class[i - 8u] & 0x3Fu);

        _obj_act_class = &_obj_slot_class[i - 8u]; /* [0x4244] - 1 */
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

/** --- object_tick: one level frame for the object machine -------------- */
void object_tick(const uint8_t *levelPal)
{
    _obj_frame++;
    object_animator_tick();
    object_claim_tick(levelPal);
    _obj_bg_tick();
}

/* update_world's two tails (1000:80CC/0x80CF): the pending-move animator and
 * the bg-log settle.  level_tick runs them only inside its [0xC] gate. */
static uint32_t _obj_world_ticks;
/** @brief One world tick. */
void object_world_tick(void)
{
    _obj_world_ticks++;
    object_animator_tick();
    _obj_bg_tick();
}
/** @brief Accessor: the world tick counter. */
uint32_t object_world_ticks_probe(void) { return _obj_world_ticks; }

/** The claim's own frame: [0x4103] advances on every page flip, and the claim
 * sequence runs while update_world is frozen (D-163). */
void object_claim_frame(const uint8_t *levelPal)
{
    _obj_frame++;
    object_claim_tick(levelPal);
}

/** @brief Accessor: the acting record's index. */
uint32_t object_action_entity(void) { return _obj_entity; }

/** 1000:3344..336C (level_fade_in): slot 8 (`0x3F05`) takes frame 0x17, type 0x1A,
 * +0xA = 0x1006 and +0xC = 0xFF.  `scan_object_rows` starts its walk there and
 * counts `[0x3E73]-8+1` records, so this slot is drawn.
 * Slot 8 is the active-human ARROW: 1000:D64F (object_marker_tick, called from
 * update_world) positions and bobs it every frame, and the selector arms it in
 * 1000:4188 (_obj_marker_arm), so the x/y the array init leaves at 0xFFFF are
 * replaced on the first level frame. */
void object_level_start_record(void)
{
    uint8_t *r = object_slot(8u);

    if (!r) return;
    wr_le_u16(r + 4u, 0x0017u);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, 0x001Au);
    wr_le_u16(r + 0xAu, 0x1006u);
    /* The next instruction of level_fade_in, `MOV byte ptr [0xc], 0xff` at
     * 1000:336C, is the GLOBAL update_world gate (level.c's worldUpdates) -
     * not this record's +0xC, which stays 0 as the asm leaves it. */
}

/** 1000:4188 (the selector's tail): the selection arms the active-human marker,
 * record 8, on the new acting record.  The original also writes
 * [0x6DEA] = 0x3E8 (the one-shot command timer). */
static void _obj_marker_arm(void)
{
    uint8_t *m = object_slot(8u);
    uint8_t *r = _obj_array + _obj_entity * OBJ_STRIDE;

    if (!m) return;
    wr_le_u16(m + 0u, rd_le_u16(r + 0u));
    wr_le_u16(m + 2u, (uint16_t)(rd_le_u16(r + 2u) - 0x1Au));
    wr_le_u16(m + 6u, 0u);
    wr_le_u16(m + 8u, 0x001Au);
    wr_le_u16(m + 0xAu, 0x0605u);
    _obj_cmd_timer = 0x03E8u;
}

/** 1000:D64F (update_world's 1000:80C9): record 8 follows the acting record;
 * its type word is subtracted from y and steps 0x1A,0x18,0x16,0x14,0x12 (two
 * every third frame - the bob); a 32-px acting sprite shifts x by +8; and the
 * word at +0xA is the lifetime - when its high byte underflows the marker is
 * disabled (+6 = 0x80) and scan_object_rows stops drawing it. */
void object_marker_tick(void)
{
    uint8_t *m = object_slot(8u);
    uint8_t *r = _obj_array + _obj_entity * OBJ_STRIDE;
    uint32_t type, frame, mode, ax;

    if (!m) return;
    if (m[6] & 0x80u) return; /* 1000:D655 */
    wr_le_u16(m + 0u, rd_le_u16(r + 0u)); /* 1000:D65B */
    type = rd_le_u16(m + 8u);
    wr_le_u16(m + 2u, (uint16_t)(rd_le_u16(r + 2u) - type)); /* 1000:D65F */
    frame = rd_le_u16(r + 4u);
    mode = frame < OBJ_FRAME_SLOTS ? _obj_sprites[frame].mode : 0u;
    wr_le_u16(m + 0u, (uint16_t)(rd_le_u16(m + 0u) +
                                 ((int32_t)mode - 1) * 8)); /* 1000:D66B */
    m[0xAu] = (uint8_t)(m[0xAu] - 1u); /* 1000:D68F */
    if (!(m[0xAu] & 0x80u)) { /* 1000:D693 JNS */
        wr_le_u16(m + 8u, type);
        return;
    }
    m[0xAu] = 2u; /* 1000:D695 */
    ax = type - 2u; /* 1000:D699 */
    if (ax >= 0x12u) { /* 1000:D69F JNC */
        wr_le_u16(m + 8u, ax);
        return;
    }
    ax = 0x001Au; /* 1000:D6A1 */
    m[0xBu] = (uint8_t)(m[0xBu] - 1u); /* 1000:D6A4 */
    if (m[0xBu] & 0x80u) m[6] = 0x80u; /* 1000:D6A8 */
    wr_le_u16(m + 8u, ax); /* 1000:D6AE */
}

/** @brief Accessor: set the acting record. */
void object_set_action_entity(uint32_t i)
{ _obj_entity = i < OBJ_STATIC ? i : _obj_entity; }
/** @brief Accessor: the action program index. */
uint32_t object_action_program(void) { return _obj_program_index; }
/** @brief Accessor: the action fault flag. */
uint32_t object_action_fault(void) { return _obj_fault; }

/** `NULL` detaches the halt (the headless tests exercise op 7 on purpose). */
void object_attach_osys(const OSystem *osys) { _obj_osys = osys; }
/** The program at 0x5B37 + its own pointer offset, `{count, cursor, icon...}`.
 * Those first two bytes are also the status bar's action-row header: the bar
 * draws this group and SPACE steps byte 1, which is the cursor the script
 * dispatcher reads (1000:3947 writes it, 1000:3987 reads prog[cursor+1]).
 * (The six runs overlap in the image, but every write lands outside the other
 * groups' visible bytes 0..5 - checked against 0x5B2B/0x5B37.) */
const uint8_t *object_program_header(uint32_t group)
{ return (group >= 1u && group <= 6u) ? _obj_programs[group - 1u] : NULL; }

/** @brief Accessor: the action program header. */
const uint8_t *object_action_header(void)
{ return object_program_header(_obj_program_index); }

/** 1000:3933's tail (0x3941-0x394D): advance the selected cell with the wrap to
 * 1 past the count.  It is the program's cursor byte, so the next ACTION press
 * runs the op the bar's highlight shows. */
void object_step_action_cell(void)
{
    uint8_t *p = _obj_programs[(_obj_program_index - 1u) % 6u];

    if (!p[0]) return;
    p[1] = (uint8_t)(p[1] + 1u);
    if (p[1] > p[0]) p[1] = 1u;
}

/** [0x5B56] (1-based): the shipped writers only produce 1..6, so this exists
 * for tests and for a future caller that restores a program from a save. */
void object_action_set_program(uint32_t index)
{ _obj_set_program_index(index); }
/** [0x1988]: the program cursor byte (the pending action 5 writes it, 0x382D). */
void object_action_set_cursor(uint32_t cursor)
{
    uint8_t *p = _obj_programs[(_obj_program_index - 1u) % 6u];
    _obj_cursor = cursor;
    p[1] = (uint8_t)cursor;
}
/** @brief Accessor: the pending event. */
uint32_t object_action_pending(void) { return _obj_pending; }

/** 0x3A26 (op 1): register the object in the 0x6F22 table and turn it into type 8. */
static void _obj_op_register(uint8_t *actor)
{
    if (rd_le_u16(actor + 8u) > 2u) return;
    for (uint32_t i = 0; i < 8u; i++) {
        uint16_t *s = _obj_register[i];
        if (!(s[0] & 0x8000u)) continue; /* testw $0x8000 -> free  */
        s[0] = rd_le_u16(actor + 0u);
        s[1] = rd_le_u16(actor + 2u);
        ((uint8_t *)s)[5] = (uint8_t)_obj_entity;
        wr_le_u16(actor + 4u, 0x0020u);
        wr_le_u16(actor + 8u, 0x0008u);
        return;
    }
}

/** 0x39B5 (op 5): the "pick up / change state" frame step. */
static void _obj_op_set_frame(uint8_t *actor)
{
    uint32_t t = rd_le_u16(actor + 8u);
    if (t >= 2u) return;
    wr_le_u16(actor + 8u, (uint16_t)(t + 0x1Fu));
    wr_le_u16(actor + 4u, (uint16_t)(0x010Eu + 4u * t));
}

/** 0x9D38 (direction 1): the found live record is taken by the actor. */
static int32_t _obj_take_found(uint8_t *actor, uint8_t *found, uint8_t *slot,
                               uint32_t ordinal)
{
    if (rd_le_u16(found + 8u) != 0xFFFFu) return 0;
    if (rd_le_u16(found + 0xAu) != 0u) return 0;
    actor[7] = (uint8_t)ordinal; /* [SI+3] = DL */
    *slot |= 0x80u; /* ORB $0x80, [BP] */
    wr_le_u16(found + 8u, (uint16_t)_obj_entity); /* [DI+4] = [0x3E22] */
    return 1;
}

/** 0x9CF0 (direction 0): the "give / hold" variant. */
static int32_t _obj_give_found(uint8_t *actor, uint8_t *found, uint8_t *slot,
                               uint32_t ordinal)
{
    if (found[6] & 0x40u) return 0;
    if (!(rd_le_u16(found + 0xEu) & 0x8000u)) return 0;
    wr_le_u16(found + 0xEu, (uint16_t)_obj_entity);
    found[7] = 0u;
    *slot |= 0x80u;
    wr_le_u16(actor + 4u, 0u); /* the actor's own record:  */
    actor[7] = (uint8_t)ordinal;
    wr_le_u16(actor + 8u, 0x000Fu);
    wr_le_u16(actor + 0xAu, 0u);
    wr_le_u16(actor + 0xEu,
              (uint16_t)((uintptr_t)found - (uintptr_t)_obj_array + 4u));
    return 1;
}

/** 1000:9D94 (the class path's behaviour-4 handler): the carried record is put
 * down and the acting record becomes a normal record again at its position. */
static void _obj_class_drop(uint8_t *actor, uint8_t *found, uint8_t *slot)
{
    uint32_t ax = 0, bx = 0;

    actor[7] = 0u; /* 1000:9D94 */
    *slot &= 0x7Fu;
    wr_le_u16(found + 0xEu, 0xFFFFu);
    found[6] &= 0x7Fu;
    if (rd_le_u16(found + 8u) & 0x8000u) { /* 1000:9DAA */
        ax = 1u;
        bx = 8u;
    }
    wr_le_u16(actor + 0u, rd_le_u16(found + 0u)); /* 1000:9DB7 */
    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(found + 2u) - 8u));
    wr_le_u16(actor + 4u, (uint16_t)bx);
    wr_le_u16(actor + 6u, 0u);
    wr_le_u16(actor + 8u, (uint16_t)ax);
}

/** 1000:9DD7 (behaviours 5..7): the carried record is thrown / placed at the
 * acting record's feet and snapped to the terrain.  The two scratch stores the
 * original makes first ([0x4238] = -6 / 0x16, [0x423C] = 0x10) are dead: every
 * reader of those words writes them itself (1000:650B, 1000:A783). */
static void _obj_class_throw(uint8_t *actor, uint8_t *found, uint8_t *slot)
{
    uint32_t type = rd_le_u16(actor + 8u);
    uint32_t beh = (uint32_t)(*slot & 0x3Fu);
    int32_t landed;
    uint32_t cell;

    actor[7] = 0u; /* 1000:9DF1 */
    *slot &= 0x7Fu;
    wr_le_u16(
        found + 0u,
        (uint16_t)(rd_le_u16(actor + 0u) +
                   (type < 0x22u ? (int16_t)_obj_throw_dx_word[type] : 0)));
    wr_le_u16(found + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 8u));
    wr_le_u16(found + 4u, beh < 8u ? _obj_drop_frame[beh] : 0u);
    wr_le_u16(found + 6u, 0u);
    wr_le_u16(found + 8u, 0xFFFFu);
    wr_le_u16(found + 0xAu, 0u);
    wr_le_u16(found + 0xCu, 0u);
    wr_le_u16(found + 0xEu, 0u);
    landed = _obj_step_probe_at(found, 8, 0x10, &cell); /* 1000:9E43 -> E389 */
    if (landed >= 0) wr_le_u16(found + 2u, (uint16_t)(landed - 0xF));
}

/** 1000:9D5D (the class path): the acting record's `+7` is set, so it carries
 * the record whose ordinal that byte holds - `DI = 0x3F17 + 0x12*(class-1)`
 * (the first live record is 0x3F17) and `BP = 0x3E5F + class - 1`.  The
 * carried record's class slot picks the handler through 0x5B97/0x5BA0; the
 * tail then always writes [0x1979] = 5, [0x1988] = 1 and [0x5B56] = 1, i.e.
 * the bar returns to the walk group. */
static void _obj_op_class_path(uint8_t *actor)
{
    uint32_t cls = actor[7];
    uint32_t live =
        _obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0 : 0u;
    uint8_t *found, *slot;
    uint32_t beh, idx;

    /* the original indexes with the byte unguarded; past the live records it
     * would read the image, which cannot happen - only the take / give paths
     * write it, always with a valid ordinal. */
    if (cls < 1u || cls > live) return;
    found = _obj_array + (OBJ_LIVE0 + cls) * OBJ_STRIDE; /* 1000:9D5D */
    slot = _obj_slot_class + cls; /* 1000:9D60 */
    beh = (uint32_t)(*slot & 0x3Fu);
    idx = beh < 8u ? _obj_beh_class_idx[beh] : 0xFFu; /* 1000:9D76 */
    if (idx & 0x80u) return; /* 1000:9D7C */
    if (idx == 0u) _obj_class_drop(actor, found, slot);
    else _obj_class_throw(actor, found, slot);
    _obj_pending = 5u; /* 1000:9D84 */
    _obj_cursor = 1u; /* 1000:9D89 */
    _obj_set_program_index(1u); /* 1000:9D8E */
}

/** 0x9C1B (ops 2/6): the collision pass over the live records. */
static void _obj_op_collide(uint8_t *actor)
{
    uint32_t type = rd_le_u16(actor + 8u);
    int32_t ax, bx;
    uint32_t live;

    if (!(type == 0x0Fu || type <= 2u)) return;
    if (actor[7]) { /* 1000:9C37 -> the class path */
        _obj_op_class_path(actor);
        return;
    }
    ax = (int16_t)rd_le_u16(actor + 0u);
    bx = (int16_t)rd_le_u16(actor + 2u);
    live = _obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0 : 0u;

    for (uint32_t k = 0; k < live; k++) {
        uint32_t ordinal = k + 1u; /* DX */
        /* DI starts at 0x3F17 = index 9 = OBJ_LIVE0 + 1 (OBJ_LIVE0 is the
         * count's base, 8). */
        uint8_t *found = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        uint8_t *slot = _obj_slot_class + ordinal; /* BP = 0x3E5F + k */
        int32_t dx, dy;
        uint32_t beh, dir, prog;

        if (*slot & 0x80u) continue;
        if (found[6] & 0x80u) continue;
        beh = (uint32_t)(*slot & 0x3Fu);
        /* 0x5B81 is 8 bytes; behaviours 8/9 (the 0xB7 stack record and the
         * 0xBB marker) read 0xFF/0xFF there, i.e. "skip" - see the table
         * comment.  10..63 read the tables that follow in the image and
         * never occur. */
        dir = beh < 8u ? _obj_beh_dir[beh] : 0xFFu;
        if (dir & 0x80u) continue;
        dx = (int16_t)rd_le_u16(found + 0u) - ax;
        if (dx < 0) dx = -dx; /* SUB/NEG, then < 0x10 */
        if (dx >= 0x10) continue;
        dy = (int16_t)rd_le_u16(found + 2u) - bx; /* 0 <= dy < 0x18 */
        if (dy < 0 || dy >= 0x18) continue;
        if (dir == 0u) {
            if (!_obj_give_found(actor, found, slot, ordinal)) continue;
        } else if (!_obj_take_found(actor, found, slot, ordinal)) continue;
        /* the tail at 1000:9CD9: [0x5B56] = [0x423C] = 0x5B8A[behaviour],
         * which is the ACTION program and the bar's action-row group; a
         * negative id leaves all three writes undone (1000:9CDF).
         * 0x5B8A[8]/[9] = 0xFF/0xF0 in the image, so the bound is the same
         * "negative id" return. */
        prog = beh < 8u ? _obj_beh_collide_prog[beh] : 0xFFu;
        if (prog & 0x80u) return;
        _obj_pending = 5u; /* [0x1979] = 5 */
        _obj_cursor = 1u; /* [0x1988] = 1 */
        _obj_set_program_index(prog); /* [0x5B56] */
        return;
    }
}

/** 0x4147 (resolve_object_sprite): [0x3E22] := index, program from the class. */
static void _obj_resolve_sprite(uint32_t index)
{
    uint8_t *cur = _obj_array + _obj_entity * OBJ_STRIDE;
    uint8_t *r = _obj_array + index * OBJ_STRIDE;
    uint32_t prog = 0u;

    if (rd_le_u16(cur + 8u) == 2u)
        wr_le_u16(cur + 4u, 0x0010u); /* the previous record */
    _obj_entity = index;
    if (r[7]) {
        /* 0x1C76 is 8 bytes (the "\humans\" path string follows it); its
         * [8] = 0x5C gives program 0x5D, out of 1..6 like the bound's 0xFF. */
        uint32_t b = _obj_class_byte_at(r[7]) & 0x3Fu;

        prog = b < 8u ? _obj_beh_program[b] : 0xFFu;
    }
    if (index == (uint32_t)_obj_player) prog = 4u;
    _obj_set_program_index(prog + 1u);
    _obj_pending = 1u;
    _obj_marker_arm(); /* 1000:4188 */
}

/** 1000:38C3 (resolve_object_sprite_guarded): the key list's F1..F8 handler.
 * AX is the scancode and the record is `scancode - 0x3B`; a slot with `+6`
 * bit 7 set or type 0x1D is skipped, otherwise 1000:4147 resolves it.
 * Returns 1 when a record was selected. */
int32_t object_select_by_index(uint32_t index)
{
    uint8_t *r;

    if (index >= OBJ_STATIC) return 0;
    r = _obj_array + index * OBJ_STRIDE;
    if ((r[6] & 0x80u) || rd_le_u16(r + 8u) == 0x1Du) return 0; /* 1000:38CE */
    _obj_resolve_sprite(index); /* 1000:38DA */
    return 1;
}

/** 1000:3933 (the key list's id 8, SPACE).  1000:392D runs the home-state
 * conversion on the acting record (get_object_ptr + 0xEDB7) and the handler
 * returns when it converted; otherwise it plays driver effect 0 and steps the
 * action row's selected cell (1000:3941 -> 0x98F8 + the 0x5B2B header's second
 * byte, wrapping to 1 past the cell count) before the bar redraw. */
int32_t object_space_action(void)
{
    if (_obj_cmd_home_state(_obj_actor())) return 1; /* 1000:392D -> 0xEDB7 */
    _obj_driver_call(0); /* 1000:393A */
    statusbar_cycle_action(); /* 1000:3941 */
    return 1;
}

/** 0x410A (resolve_next_eligible_object_sprite): cycle the 8 static records. */
int32_t object_cycle_entity(int32_t step)
{
    int32_t at = (int32_t)_obj_entity;

    for (uint32_t n = 0; n < OBJ_STATIC; n++) {
        int32_t idx = at + (step < 0 ? -1 : 1);
        uint8_t *r;

        if (idx < 0) idx = (int32_t)OBJ_STATIC - 1;
        if (idx >= (int32_t)OBJ_STATIC) idx = 0;
        r = _obj_array + idx * OBJ_STRIDE;
        if (!(r[6] & 0x80u) && rd_le_u16(r + 8u) != 0x1Du) {
            _obj_resolve_sprite((uint32_t)idx);
            return idx;
        }
        at = idx;
    }
    return -1;
}

/** test_class_against_6eb6 (0xF2CF): ZF set when the acting record's behaviour
 * has this state.  The callers are `CALL`/`JNZ`. */
static int32_t _obj_state_is(uint8_t *actor, uint32_t state)
{
    uint32_t b = actor[7u];
    if (!b) return 0;
    return (_obj_beh_state[_obj_class_byte_at(b) & 0x3Fu] == state);
}

/** 0xF989 (object_record_walk): the live record with slot 3 (behaviour 3) that
 * sits left of `x + step` and above `y + 0xC`, i.e. -0x10 <= dx < 0 and
 * -0x18 <= dy < 0.  Returns the slot index or -1. */
static int32_t _obj_record_walk(uint8_t *actor, uint32_t step)
{
    int32_t cx = (int16_t)rd_le_u16(actor + 2u) + 0x0C;
    int32_t bx = (int16_t)rd_le_u16(actor + 0u) + (int32_t)step;
    uint32_t live =
        _obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0 : 0u;

    for (uint32_t k = 0; k < live; k++) {
        uint8_t *r = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        int32_t dx, dy;

        if (_obj_slot_class[1u + k] != 3u) continue; /* 1000:F99F, whole byte */
        if (r[6] & 0x80u) continue;
        if (rd_le_u16(r + 8u) == 2u) continue;
        dx = (int16_t)rd_le_u16(r + 0u) - bx;
        if (!(dx < 0 && dx + 0x10 >= 0)) continue;
        dy = (int16_t)rd_le_u16(r + 2u) - cx;
        if (!(dy < 0 && dy + 0x18 >= 0)) continue;
        return (int32_t)(OBJ_LIVE0 + 1u + k); /* BP = the record           */
    }
    return -1;
}

/** 0xEFA1 / 0xEFB9 (op 4): set the record's fixed state, gated on the
 * behaviour's state byte being 1 (behaviour 5, the spear). */
static void _obj_op_set_state(uint8_t *actor, uint32_t type)
{
    if (!_obj_state_is(actor, 1u)) return;
    if (type == 0u) {
        wr_le_u16(actor + 4u, 0x00BBu); /* 1000:EFA9 */
        wr_le_u16(actor + 8u, 0x0017u);
    } else {
        wr_le_u16(actor + 4u, 0x00BEu); /* 1000:EFC1 */
        wr_le_u16(actor + 8u, 0x0018u);
    }
    _obj_lat = 0; /* 1000:EFB3 */
}

/** @brief Find the record's overlap target. */
static void _obj_find_overlap(uint8_t *actor)
{
    int32_t ax = (int16_t)rd_le_u16(actor + 0u);
    int32_t bx = (int16_t)rd_le_u16(actor + 2u);

    (void)object_stack_scan_dissolve((uint32_t)(actor - _obj_array) /
                                     OBJ_STRIDE);

    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint8_t *found;
        uint32_t type;
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
        if (type < 16u && _obj_type_overlaps[type]) continue;
        /* 1000:F453 scan_groups_for_record (DX = 5) -> 1000:FA13: the group
         * whose member list holds this record is dissolved */
        wr_le_u16(found + 4u, 0x0103u);
        wr_le_u16(found + 8u, 0x001Eu);
        wr_le_u16(found + 0xAu, 0x0006u);
        wr_le_u16(found + 0xCu, 0x0000u);
        wr_le_u16(found + 0xEu, 0x000Au);
    }
}

/** 0xEF0C / 0xEF39 (op 9, non-player): mark the walked record as state 1,
 * gated on the behaviour's state byte being 3 (behaviour 6), then run
 * object_find_overlap (0xF2E0). */
static void _obj_op_mark_walk(uint8_t *actor, uint32_t step)
{
    int32_t slot;

    if (!_obj_state_is(actor, 3u)) return;
    slot = _obj_record_walk(actor, step);
    if (slot < 0) return;
    {
        uint8_t *r = _obj_array + (uint32_t)slot * OBJ_STRIDE;
        if (rd_le_u16(r + 8u) == 1u) return;
        wr_le_u16(r + 8u, 0x0001u);
        wr_le_u16(r + 0xAu, 0x0003u);
        wr_le_u16(r + 0xCu, 0x0000u);
    }
    _obj_find_overlap(actor);
}

/** 1000:E3F6's input normalisation: swap bits 2 and 3, drop bits 4..6, then kill
 * each contradictory pair (0x3 / 0xC) the way the original does. */
static uint8_t _obj_normalise_input(uint8_t in)
{
    uint8_t v =
        (uint8_t)((in & 0x83u) | ((in & 0x08u) >> 1) | ((in & 0x04u) << 1));

    if ((v & 0x03u) == 0x03u) v &= 0x8Du;
    if ((v & 0x0Cu) == 0x0Cu) v &= 0x8Bu;
    return v;
}

/** 1000:E3F6 dispatch_actor_command: the acting record's command for one input
 * word.  The original calls the returned handler, or the row's tail handler when
 * it is 0 and the handler did not change the record's type; the handlers are the
 * next objective, so only the selection (and the one-shot counter's two writes)
 * is ported.  One global counter, not one per command: [0x6DEA]. */
ObjCommand object_command_select(uint8_t *rec, uint8_t input)
{
    ObjCommand out;
    uint8_t v = _obj_normalise_input(input);
    uint8_t cmd = _obj_cmd_key[v & 0x0Fu];
    const uint8_t *row;
    uint32_t type = rd_le_u16(rec + 8u); /* [SI+8] */
    uint32_t idx;

    out.handler = 0u;
    out.tail = 0u;
    out.hook = 0u;
    out.command = 0u;
    if (type >= 35u) return out; /* no row: the original reads 0x6DEA */
    row = _obj_cmd_rows[type] + ((v & 0x80u) ? 0x10u : 0u);
    _obj_row_ofs = (uint32_t)(row - &_obj_cmd_rows[0][0]); /* DI at 1000:E467 */
    out.tail = rd_le_u16(row + 0xCu);
    out.hook = row[0xAu];
    if (cmd & 0x80u) return out; /* bit 7: this input has no command */
    {
        /* 1000:E47C..0xE49F: AX starts as 0x4B0 (the arm for a non-one-shot
         * command); a bit-6 command replaces it with the [0x6DEA] timer and,
         * when the row's first byte is non-zero, decrements it - reaching 0
         * turns the record into a human (type 2).  A zero timer is *stored as
         * zero*: that is what lets the type-2 idle stream (0xE7C0, gated on
         * [0x6DEA] == 0) finally run. */
        uint16_t n = 0x4B0u;

        if (cmd & 0x40u) {
            n = _obj_cmd_timer;
            if (_obj_cmd_timer && row[0]) {
                n--;
                if (!n) wr_le_u16(rec + 8u, 0x0002u);
            }
        }
        _obj_cmd_timer = n;
    }
    cmd &= 0x3Fu;
    out.command = cmd;
    /* 1000:E4A7 `MOV BL,[BX+DI+1]` with CL = CL & 0xBF: row[1 + cmd], with no
     * upper bound.  The reachable commands are 0..8 (DS:0x6908 gives 0x48 for
     * "no direction", i.e. 8 after the 0x40 clear; the rest carry bit 7), and
     * 1 + 8 = 9 stays inside the row's 16-byte half. */
    idx = cmd <= 8u ? row[1u + cmd] : 0u;
    if (idx >= 1u && idx <= 57u) out.handler = _obj_cmd_handlers[idx - 1u];
    return out;
}

/** The original's `1 << (7 - n)` on a 16-bit register: for n > 7 the count is
 * negative, and an 8086 SHL by a count >= 16 shifts every bit out, so the bit is
 * 0.  Records 0..7 therefore own 0x80..0x01 and 8..14 own nothing. */
static uint32_t _obj_index_bit(uint32_t idx)
{ return idx <= 7u ? (1u << (7u - idx)) : 0u; }

/** 1000:F3CA: clear the record's bit from both masks. */
static void _obj_stack_unmark(uint8_t *actor)
{
    uint8_t keep =
        (uint8_t)~_obj_index_bit((uint32_t)(actor - _obj_array) / OBJ_STRIDE);

    _obj_stack_mask[0] &= keep;
    _obj_stack_mask[1] &= keep;
}

/** 1000:F38F test_bits_64ad_64c1: mark the record as being in a stack, in the
 * mask that the list byte below it (`slot[-1]`) is marked in; when that byte is
 * a terminator or unmarked, clear both instead. */
static void _obj_stack_mark(uint8_t *actor, const uint8_t *slot)
{
    uint32_t below = slot[-1];

    if (!(below & 0x80u)) {
        uint32_t bit = _obj_index_bit(below);

        for (uint32_t m = 0; m < 2u; m++) {
            if (_obj_stack_mask[m] & bit) {
                _obj_stack_mask[m] |= (uint8_t)_obj_index_bit(
                    (uint32_t)(actor - _obj_array) / OBJ_STRIDE);
                return;
            }
        }
    }
    _obj_stack_unmark(actor);
}

/** 1000:FD91: the walk frame step - `y--`, every 5th frame advance the frame by
 * +/-1 (`+0xC` is the direction) and reverse at the `lo`/`hi` bounds.  Both
 * leaves take this on a `y` mismatch. */
static void _obj_frame_step(uint8_t *actor, uint32_t lo, uint32_t hi)
{
    uint32_t f;

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
    wr_le_u16(actor + 4u, (uint16_t)f);
}

/** 1000:E697 (a fall-through inside 1000:E609, the walk-up state): push the
 * acting record onto the `0x6F22` group it stands under.  The original reaches
 * this with AX = actor.x and BX = actor.y + 0x18, after the `0x6F92` span scan
 * has failed; the port takes the record and derives the same two values.
 * Returns 1 when a group took it (frame 0x25, type 7, `+0xE` = its slot). */
int32_t object_stack_push(uint8_t *actor)
{
    uint16_t ax = rd_le_u16(actor + 0u);
    uint16_t bx = (uint16_t)(rd_le_u16(actor + 2u) + 0x18u);

    for (uint32_t i = 0; i < 8u; i++) { /* `CX = 7` + `JNS`: 8 */
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint16_t ex, ey, dx;
        uint8_t *di;
        int32_t de;

        ex = rd_le_u16(e + 0u);
        if (ex & 0x8000u) continue; /* free */
        de = (int32_t)(int16_t)ex - (int32_t)(int16_t)ax;
        if (de < 0) de = -de;
        if (de >= 8) continue; /* |entry.x - actor.x| < 8 */
        ey = rd_le_u16(e + 2u);
        if (bx >= ey) return 0; /* `JNC 0xE6FA`: not under it */
        dx = ey;
        di = e + 5; /* the owner byte */
        while (1) {
            dx -= 0x10u; /* 16 px per member */
            if (di[0] & 0x80u) {
                di++;
                break;
            } /* the terminator */
            di++;
        }
        dx += 0x18u;
        if (bx < dx) return 0; /* `JC 0xE6FA` */
        di--; /* the terminator's slot */
        *di = (uint8_t)_obj_entity; /* `[0x3E22]` into the list */
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
        _obj_stack_mark(actor, di); /* 1000:F38F */
        return 1;
    }
    return 0;
}

/** 1000:FDED object_script_select (the pop): find the acting record in a
 * `0x6F22` list and, if it is the topmost member, remove it.  Returns the
 * original's AX: -1 = not present or not the top, 0 = the record below is within
 * 8 px of `ax` (the caller's x) so the list is left alone, 1 = removed.
 * `*below` = the record the original leaves in DI (NULL when the entry was
 * freed). */
int32_t object_stack_pop(uint16_t ax, uint8_t **below, uint8_t **entry)
{
    *below = NULL;
    if (entry) *entry = NULL;
    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di = e;
        int32_t bl;

        if (entry) *entry = e; /* 1000:FDF7: DX = the entry */

        if (rd_le_u16(di + 0u) & 0x8000u) continue; /* free */
        di += 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)_obj_entity) di++;
        if (di[0] & 0x80u) continue; /* not in this entry */
        di++; /* past the match */
        if (!(di[0] & 0x80u)) return -1; /* a member above it */
        bl = (int8_t)di[-2]; /* the list byte below */
        if (bl >= 0) {
            uint8_t *nb = object_slot((uint32_t)bl);
            int32_t dx =
                nb ? (int32_t)(int16_t)rd_le_u16(nb + 0u) - (int32_t)(int16_t)ax
                   : 8;

            if (dx < 0) dx = -dx;
            if (nb && dx < 8) return 0; /* already aligned: no removal */
        }
        di[-1] = 0xFFu; /* terminate at the match */
        bl = (int8_t)di[-2];
        if (bl < 0) { /* nothing below: free the entry */
            wr_le_u16(di - 6 + 0u, 0xFFFFu);
            wr_le_u16(di - 6 + 2u, 0xFFFFu);
            return 1;
        }
        *below = object_slot((uint32_t)bl);
        return 1;
    }
    return -1;
}

/** 1000:E58B (the tail of 1000:E4F8, the step-down state): grab the group the
 * actor stands at - within 8 px in x and 4 px in y - and append it to the
 * member list (frame 0x23, type 6).  With no group matching, register the
 * actor's position in the last free `0x6F22` entry (frame 0x20, type 8).
 * Returns 1 = grabbed, 0 = registered, -1 = nothing (no group, no free entry:
 * the original would write through a stale `[0x423C]`, the port refuses). */
int32_t object_stack_grab(uint8_t *actor)
{
    uint16_t ax = rd_le_u16(actor + 0u);
    uint16_t bx = rd_le_u16(actor + 2u);
    uint8_t *lastFree = NULL;

    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint16_t ex = rd_le_u16(e + 0u), ey;
        uint8_t *di;
        int32_t d;

        if (ex & 0x8000u) {
            lastFree = e + 2;
            continue;
        } /* `[0x423C] = DI` */
        d = (int32_t)(int16_t)ex - (int32_t)(int16_t)ax;
        if (d < 0) d = -d;
        if (d >= 8) continue; /* |entry.x - actor.x| < 8 */
        ey = rd_le_u16(e + 2u);
        d = (int32_t)(int16_t)ey - (int32_t)(int16_t)bx;
        if (d < 0) d = -d;
        if (d >= 4) continue; /* |entry.y - actor.y| < 4 */
        di = e + 5;
        while (!(di[0] & 0x80u)) di++; /* to the terminator */
        *di = (uint8_t)_obj_entity;
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) - 1u));
        wr_le_u16(actor + 0u, ex);
        wr_le_u16(actor + 4u, 0x0023u);
        wr_le_u16(actor + 8u, 0x0006u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        _obj_stack_mark(actor, di); /* 1000:F38F */
        return 1;
    }
    if (!lastFree) return -1;
    wr_le_u16(lastFree + 0u, bx); /* entry + 2: y */
    lastFree[3] = (uint8_t)_obj_entity; /* entry + 5: the owner */
    wr_le_u16(lastFree - 2 + 0u, ax); /* entry + 0: x */
    wr_le_u16(actor + 4u, 0x0020u);
    wr_le_u16(actor + 8u, 0x0008u);
    return 0;
}

/** 1000:E860 (handler 10): step off the group.  Finds the entry with the actor's
 * x, the actor in its list, and on `entry.y - 0x18 == actor.y` moves the actor
 * down 8 (frame 0x20, type 8) and the list byte below it likewise (frame 0x21).
 * Returns 1 = acted, 0 = not in a group, -1 = the `y` mismatch branch (1000:E8D4
 * -> 1000:FD91, the walk frame step). */
int32_t object_stack_step_off(uint8_t *actor)
{
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);
    uint16_t ax = rd_le_u16(actor + 0u);

    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;
        uint16_t ey;

        if (rd_le_u16(e + 0u) != ax) continue;
        ey = rd_le_u16(e + 2u);
        di = e + 6; /* the first member */
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) {
            ey -= 0x10u; /* 1000:E896: one level up */
            di++;
        }
        if (di[0] & 0x80u) continue;
        ey -= 0x18u; /* 1000:E89B */
        if (ey != rd_le_u16(actor + 2u)) {
            _obj_frame_step(actor, 0x23u, 0x25u); /* 1000:E8D4 */
            return -1;
        }
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 8u));
        wr_le_u16(actor + 4u, 0x0020u);
        wr_le_u16(actor + 8u, 0x0008u);
        if (!(di[-1] & 0x80u)) {
            uint8_t *nb = object_slot(di[-1]);

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

/** 1000:E8DD (handler 11): leave the group.  Same walk, and on `entry.y ==
 * actor.y` the actor becomes a standing human (frame 0x10, type 2) and the list
 * is cut at the slot the push stored in `actor+0xE`.
 * Returns 1 = left, 0 = not in a group, -1 = the `y` mismatch branch (1000:E934
 * -> 1000:FD91). */
int32_t object_stack_leave(uint8_t *actor)
{
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);
    uint16_t ax = rd_le_u16(actor + 0u);

    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;

        if (rd_le_u16(e + 0u) != ax) continue;
        di = e + 6;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        if (rd_le_u16(e + 2u) != rd_le_u16(actor + 2u)) {
            /* 1000:E934: the *down* stepper (y += 1, frames 0x23..0x25) - the
             * member climbs down the column 1 px a step.  (The step-off's
             * 1000:E8D4 is the one that uses the up stepper.) */
            _obj_frame_step_down(actor, 0x23u, 0x25u);
            return -1;
        }
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
        {
            uint32_t off = rd_le_u16(actor + 0xEu);

            if (off < sizeof _obj_register) /* 1000:E922 */
                ((uint8_t *)_obj_register)[off] = 0xFFu;
        }
        return 1;
    }
    return 0;
}

/** 1000:E99D (index 15): stand back on the group.  The topmost member takes the
 * entry's x, `y -= 7`, frame 0x25 and type 7 - and stores its slot in `+0xE` -
 * while the list byte below it moves up 8 (frame 0x20).  Like the pop, its
 * "below" lookup reads `match-1` literally, so for the owner itself that byte is
 * the entry's `+4` state. */
int32_t object_stack_stand(uint8_t *actor)
{
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);

    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;
        int32_t bl;

        if (rd_le_u16(e + 0u) & 0x8000u) continue;
        di = e + 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        di++;
        if (!(di[0] & 0x80u)) return 0; /* a member above it */
        di--; /* the match */
        wr_le_u16(actor + 0xEu,
                  (uint16_t)((uintptr_t)di - (uintptr_t)_obj_register));
        di--;
        bl = (int8_t)di[0];
        if (bl < 0) return 0;
        {
            uint8_t *nb = object_slot((uint32_t)bl);

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

/** 1000:E963 (handler 14): snap onto the group's x.  Only the topmost member
 * (the byte after it is a terminator) moves: `actor.x = entry.x`, frame 0x20. */
int32_t object_stack_snap(uint8_t *actor)
{
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);

    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        uint8_t *di;

        if (rd_le_u16(e + 0u) & 0x8000u) continue;
        di = e + 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        di++;
        if (!(di[0] & 0x80u)) return 0; /* a member above it */
        wr_le_u16(actor + 0u, rd_le_u16(e + 0u));
        wr_le_u16(actor + 4u, 0x0020u);
        return 1;
    }
    return 0;
}

/** 1000:E361 map_get, the law shared with _obj_map_cell: the map cell at
 * (x, y) is `map[32*(y>>4) + (x>>4)]`; `*sub` returns `x & 0xF`, the 0x92A0
 * sub-column the step test indexes.  Out of the 640-byte map the original reads
 * the neighbouring buffer, the port returns 0xFF. */
static uint32_t _obj_map_at_abs(int32_t x, int32_t y, uint32_t *sub)
{
    const uint8_t *map = level_map();
    uint32_t cell;

    if (sub)
        *sub = (uint32_t)x & 0xFu; /* the fine x, when the caller wants it */
    if (!map) return 0xFFu;
    cell = 32u * (((uint32_t)y & 0xFFF0u) >> 4) + ((uint32_t)x >> 4);
    return cell < 640u ? map[cell] : 0xFFu;
}

/** 1000:E389 map_cell_lookup as a point: (x = AX, y = BX) -> the landed y, with
 * -1 for "blocked".  The cell at the point and its 0x92A0 byte decide the y: a
 * flat cell keeps y, a slope adds its low bits, a solid cell takes the slope of
 * the row below (blocked = -1 when that is solid too) or, for a zero byte, the
 * row above.  The original's third argument is the item table in `BP`; the port
 * reads the same table through `level_page_byte` (see re_map.md, "the handler's
 * BP", for what the dispatcher leaves in BP). */
static int32_t _obj_land_y(int32_t x, int32_t y, uint32_t *cellOut)
{
    /* 1000:E361 does AND BX,0xFFF0 on the probe y, so every return below is
     * relative to the row start, not the raw probe line. */
    int32_t row = y & ~0xF;
    uint32_t sub, id = _obj_map_at_abs(x, row, &sub);
    uint32_t b = level_page_byte(id, sub);

    *cellOut = id;
    if (!(b & 0x80u)) {
        if (b != 0u) return row + (int32_t)(int8_t)b;
        uint32_t above =
            _obj_map_at_abs(x, row - 0x10, &sub); /* 1000:E3BA, DI unmoved */
        b = level_page_byte(above, sub);
        if (b & 0x80u) {
            *cellOut = id; /* 1000:E3CA: DI is still the point's cell */
            return row; /* AX = 0 */
        }
        *cellOut = above;
        return row - 0x10 + (int32_t)(int8_t)b;
    }
    {
        uint32_t below =
            _obj_map_at_abs(x, row + 0x10, &sub); /* 1000:E3E7, DI unmoved */
        b = level_page_byte(below, sub);
        if (b & 0x80u) {
            *cellOut = id; /* 1000:E3F1: DI is still the point's cell */
            return -1;
        }
        *cellOut = below;
    }
    return row + 0x10 + (int32_t)(int8_t)b;
}

/** @brief Test seam: the landing probe. */
int32_t object_land_y_for_probe(int32_t x, int32_t y, uint32_t *cellOut)
{ return _obj_land_y(x, y, cellOut); }

/** 1000:E389 from the actor: the cell ahead (x + 8, y + 0x18). */
static int32_t _obj_step_probe_at(const uint8_t *actor, int32_t dx, int32_t dy,
                                  uint32_t *cellOut)
{
    return _obj_land_y((int32_t)(int16_t)rd_le_u16(actor + 0u) + dx,
                       (int32_t)(int16_t)rd_le_u16(actor + 2u) + dy, cellOut);
}

/** @brief Probe the step below the actor. */
static int32_t _obj_step_probe(const uint8_t *actor, uint32_t *cellOut)
{ return _obj_step_probe_at(actor, 8, 0x18, cellOut); }

/** 1000:F25F: whether the walk can land - the step probe's landing must not
 * rise more than 8 px above the feet (a step-up blocks), and the item record's
 * bit for the sub-cell 0x10 below the actor's y must be clear.  0 = free,
 * 0xFFFF = blocked.  `1000:F25F` loads AX = [0x4238] for the probe, i.e. it is
 * `x + 2` walking left and `x + 0xE` walking right - not the selector's fixed
 * `x + 8` (`1000:F1D7`). */
static uint32_t _obj_walk_test(const uint8_t *actor)
{
    uint32_t sub, id, bit, cell;
    int32_t x, y, d;
    int32_t bx = _obj_step_probe_at(actor, (int32_t)(int16_t)_obj_sel_frame,
                                    0x18, &cell);

    if (bx >= 0) {
        d = bx - 0x18 - (int32_t)(int16_t)rd_le_u16(actor + 2u);
        if (d < 0 && d + 8 < 0) return 0xFFFFu;
    }
    x = (int32_t)(int16_t)rd_le_u16(actor + 0u) +
        (int32_t)(int16_t)_obj_sel_frame;
    y = (int32_t)(int16_t)rd_le_u16(actor + 2u) + 0x10;
    id = _obj_map_at_abs(x, y, &sub);
    bit = 1u << (0xFu - ((uint32_t)x & 0xFu));
    return (level_item_word(id, (uint32_t)((uint32_t)y & 0xFu)) & bit) ? 0xFFFFu
                                                                       : 0u;
}

/** 1000:E6FB (left) / 1000:E764 (right): the walk step.  Sets the result
 * frame/type to 2/0x10 or 0xE/0x10, asks 1000:F25F, then moves x by one and
 * advances the walk frame (8..0xF left, 0..7 right) every fifth frame. */
static void _obj_walk(uint8_t *actor, int32_t left)
{
    uint32_t lo = left ? 8u : 0u, hi = left ? 0x10u : 8u, f;

    _obj_sel_frame = left ? 2u : 0xEu;
    _obj_sel_type = 0x10u;
    if (_obj_walk_test(actor) != 0u) return; /* blocked: no step */
    wr_le_u16(actor + 0u,
              (uint16_t)(rd_le_u16(actor + 0u) + (left ? 0xFFFFu : 1u)));
    wr_le_u16(actor + 0xAu, (uint16_t)(rd_le_u16(actor + 0xAu) - 1u));
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 0xAu, 0x0004u);
    f = (uint32_t)((rd_le_u16(actor + 4u) + 1u) & 0xFFFFu);
    if (f >= hi) f = lo;
    wr_le_u16(actor + 4u, (uint16_t)f);
}

/** The dispatcher's `DI` at the handler/hook call (1000:E467): the type row's
 * selected half, `DS:0x698A + 32*type [+ 0x10]`.  The hooks receive it as an
 * argument register and read it as their "rect" (`1000:F423`), so the port keeps
 * the same byte offset into its copy of the rows.
 *
 * The earlier model read a `0x6F92` span here.  That was wrong: `1000:E467` sets
 * DI to the row and `1000:E4DD` calls `1000:D8BD` with DI untouched, so
 * `1000:F3ED`/`1000:F423` see the **row**, not a span.  The spans at `0x6F92`
 * are read by `1000:E4F8`/`0xE609` (the step verbs) and written by `1000:FC81`,
 * which is why the confusion was possible - see re_history.md. */
static uint32_t _obj_row_word(uint32_t off)
{
    const uint8_t *f = &_obj_cmd_rows[0][0];
    uint32_t i = _obj_row_ofs + off;

    if (i + 1u >= sizeof _obj_cmd_rows) return 0u;
    return rd_le_u16(f + i);
}

/** 1000:F423: is (BX, CX) inside the window the row half at `off` describes?  A
 * match ORs `bit` into `off+0x13` and returns 1 (the caller's `JS`: the settle is
 * refused).  The window is `x` in `([row+0] - 0x2A, [row+0] - 0x10]` and `y` in
 * `([row+2] - 9, [row+2] - 5]`, with `row+0x10`'s bit 15 as the "free" marker.
 *
 * `row+0x10` is the *other* half of the type row, so the free marker and the
 * bit byte it ORs live in that half - which is what makes the gate's two calls
 * (`off = 0` and `off = 0x14`) read overlapping windows.  On the shipped rows
 * the y word is 0x0505 / 0x0600 / ... i.e. far above the 320-row map, so no
 * window ever matches and the store never fires; a test asserts exactly that
 * over every type and both halves. */
static int32_t _obj_row_rect_at(uint32_t off, int32_t bx, int32_t cx,
                                uint8_t bit)
{
    int32_t d;

    if (_obj_row_word(off + 0x10u) & 0x8000u) return 0; /* 1000:F425 */
    d = (int16_t)_obj_row_word(off) - 0x10 - bx; /* 1000:F42C */
    if (d >= 0) return 0;
    d += 0x1A; /* 1000:F433 */
    if (d < 0) return 0;
    d = (int16_t)_obj_row_word(off + 2u) - cx; /* 1000:F438 */
    if (d < 0) return 0;
    d -= 5;
    if (d < 0 || d > 4) return 0;
    if (bit) {
        uint32_t i = _obj_row_ofs + off + 0x13u;

        if (i < sizeof _obj_cmd_rows) _obj_cmd_rows[i / 32u][i % 32u] |= bit;
    }
    return 1;
}

/** The gate's two windows, for the test that shows they never match. */
int32_t object_row_window_matches(uint32_t type, uint32_t half, int32_t x,
                                  int32_t y)
{
    uint32_t save = _obj_row_ofs;
    int32_t r;

    if (type >= 35u) return 0;
    _obj_row_ofs = 32u * type + (half ? 0x10u : 0u);
    r = _obj_row_rect_at(0u, x, y, 0u) | _obj_row_rect_at(0x14u, x, y, 0u);
    _obj_row_ofs = save;
    return r;
}

/** 1000:F3ED: clear the stack bits, then - unless the actor's type is marked at
 * 0x6EBE - let the two row windows claim it.  Returns 1 when the settle may run
 * (the original's ZF). */
static int32_t _obj_settle_gate(uint8_t *actor)
{
    uint32_t t = rd_le_u16(actor + 8u);
    uint8_t bit = (uint8_t)(1u << (7u - (t & 7u)));
    int32_t bx = (int16_t)rd_le_u16(actor + 0u),
            cx = (int16_t)rd_le_u16(actor + 2u);

    _obj_stack_unmark(actor); /* 1000:F3CA */
    if (_obj_type_gate[(t >> 3) & 0x1Fu] & bit) return 1; /* 1000:F450 */
    if (_obj_row_rect_at(0u, bx, cx, bit)) return 0; /* 1000:F417 */
    if (_obj_row_rect_at(0x14u, bx, cx, bit)) return 0; /* 1000:F41F */
    return 1;
}

/** 1000:DF2C: the footing landed inside one of the level's 0xA9 sprites?  Marks
 * that sprite's own buffer and returns -1 (the settle then skips its cache). */
static int32_t _obj_bg_mark(const uint8_t *actor)
{
    uint32_t sub, cell = _obj_map_at_abs((int32_t)_obj_foot_x,
                                         (int32_t)_obj_foot_y, &sub);
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);
    const uint8_t *tbl;
    uint32_t bl;

    if (cell < 0xC2u || cell > 0xCFu) return 0; /* 1000:DF77 */
    for (uint32_t i = 0; i < _obj_bg_count; i++) {
        uint32_t rx = _obj_bg[i].x, ry = _obj_bg[i].y;
        int32_t d = (int32_t)_obj_foot_x - (int32_t)((rx + 0x10u) & 0xFFF0u);

        if ((_obj_foot_y & 0xFFF0u) != ((ry + 0x10u) & 0xFFF0u)) continue;
        if (d < 0 || d >= 0x20) continue;
        if (idx < 8u) {
            if (rd_le_u16(actor + 8u) > 1u) return -1; /* 1000:E001 */
            tbl = _obj_foot_b;
        } else {
            int32_t side = (int16_t)_obj_side;
            if (side == 0) return -1; /* 1000:DFF8 */
            tbl = side < 0 ? _obj_foot_b : _obj_foot_a;
        }
        bl = tbl[(uint32_t)(_obj_foot_x - rx) & 0x1Fu];
        if (bl && _obj_bg[i].dst) _obj_bg[i].dst[bl] = 0xFFu;
        return -1;
    }
    return 0;
}

/* 1000:D8BD: the terrain settle the type rows' +0xA hook runs.  Snaps the actor
 * to the slope under its feet, lands it (type 3) on a drop of more than 8 px,
 * and caches the settled position at 0x6847.  `1000:D8C3` keeps the unaligned
 * feet line in CX for the distance test and masks BX to the row base
 * (`AND BX,0xFFF0`, 1000:D8D0); the page lookup and the step use the masked
 * value. */
/** @brief 1000:D9B2: the settle's landing state (type 3, hold 1, frame 4). */
static void _obj_cmd_settle_landed(uint8_t *actor)
{
    wr_le_u16(actor + 8u, 0x0003u);
    wr_le_u16(actor + 0xAu, 0x0001u);
    wr_le_u16(actor + 0xCu, 0x0004u);
    wr_le_u16(actor + 0xEu, 0x0000u);
}

/** @brief The terrain settle the type rows' +0xA hook runs. */
static void _obj_cmd_settle(uint8_t *actor)
{
    int32_t x = (int16_t)rd_le_u16(actor + 0u);
    int32_t y = (int16_t)rd_le_u16(actor + 2u);
    int32_t ax = x + 8, feet = y + 0x18, line = feet & ~0xF, bx, d;
    uint32_t sub, cell, b;

    if (!_obj_settle_gate(actor)) return;
    cell = _obj_map_at_abs(ax, line, &sub);
    if (cell == level_special_tile(0u) || cell == level_special_tile(1u))
        return;
    b = level_page_byte(cell, sub);
    _obj_side = 0;
    if (b == 0u) { /* 1000:D91D */
        b = level_page_byte(_obj_map_at_abs(ax, line - 0x10, &sub), sub);
        if (!(b & 0x80u)) {
            _obj_side = 0xFFE0;
            line -= 0x10;
        } else {
            b = 0u;
        }
    } else if (b & 0x80u) { /* 1000:D945 */
        line += 0x10;
        _obj_side = 0x20;
        b = level_page_byte(_obj_map_at_abs(ax, line, &sub), sub);
        if (b & 0x80u) { /* 1000:D9B2 */
            _obj_cmd_settle_landed(actor);
            return;
        }
    }
    bx = line + (int32_t)(int8_t)b; /* 1000:D967 */
    d = feet - bx; /* 1000:D96A: CX */
    if (d < 0) {
        d = -d;
        if (d > 8) {
            _obj_cmd_settle_landed(actor);
            return;
        }
    }
    wr_le_u16(actor + 2u, (uint16_t)(bx - 0x18));
    _obj_foot_x = (uint16_t)(rd_le_u16(actor + 0u) + 8u);
    _obj_foot_y = (uint16_t)(rd_le_u16(actor + 2u) + 0x18u);
    if (_obj_bg_mark(actor) != 0) return; /* 1000:D986 */
    _obj_settled[(uint32_t)((actor - _obj_array) / OBJ_STRIDE)][0] =
        rd_le_u16(actor + 0u);
    _obj_settled[(uint32_t)((actor - _obj_array) / OBJ_STRIDE)][1] =
        rd_le_u16(actor + 2u);
}

/** @brief Copy one four-byte shade group into its 16-byte row. */
static void _shade_row(const ObjSprite *sp, uint32_t pass, uint32_t row,
                       uint8_t *out, uint32_t *s)
{
    for (uint32_t j = 0; j < 4u; j++) {
        if ((uint32_t)(sp->off + *s) < _obj_bank_size)
            out[row * 16u + pass + 4u * j] = _obj_bank[sp->off + *s];
        (*s)++;
    }
}

/** 1000:3E73's shade table: the recipe sprite's bytes as a 16x16 table.  Rows
 * 0..ybias-1 stay zero, then `rows` rows copy four bytes per pass; pass p fills
 * `row*16 + p + 4*j`, so the four passes fill every byte of each row. */
static void _obj_build_shade(uint32_t frame, uint8_t *out)
{
    const ObjSprite *sp;
    uint32_t s = 0;

    memset(out, 0, 256u);
    if (frame >= OBJ_FRAME_SLOTS) return;
    sp = &_obj_sprites[frame];
    for (uint32_t pass = 0; pass < 4u; pass++)
        for (uint32_t row = 0; row < 16u; row++) {
            if (row < sp->ybias || row >= sp->ybias + sp->rows) continue;
            _shade_row(sp, pass, row, out, &s);
        }
}

/** 1000:E72A (index 4) and 1000:E790 (index 6): the two idle handlers.  Both
 * keep the frame in its cycle (8..0x10 / below 8 and 0x10) with a 4-frame
 * countdown in `+0xA`; the first returns to frame 0x10/type 2 and then 8/1, the
 * second to 0x10/type 2 and then 0/0. */
static void _obj_cmd_idle_a(uint8_t *actor)
{
    uint32_t f = rd_le_u16(actor + 4u);

    if (f < 8u || f > 0x10u) { /* 1000:E739: stand up */
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
        wr_le_u16(actor + 0xAu, 0x0004u);
        return;
    }
    wr_le_u16(actor + 0xAu,
              (uint16_t)(rd_le_u16(actor + 0xAu) - 1u)); /* 1000:E74F */
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 4u, 0x0008u);
    wr_le_u16(actor + 8u, 0x0001u);
    wr_le_u16(actor + 0xAu, 0x0004u);
}

/** @brief Type 18 command 6: the second idle. */
static void _obj_cmd_idle_b(uint8_t *actor)
{
    uint32_t f = rd_le_u16(actor + 4u);

    if (f >= 8u && f != 0x10u) { /* 1000:E79D: stand up */
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
        wr_le_u16(actor + 0xAu, 0x0004u);
        return;
    }
    wr_le_u16(actor + 0xAu,
              (uint16_t)(rd_le_u16(actor + 0xAu) - 1u)); /* 1000:E7AD */
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 4u, 0x0000u);
    wr_le_u16(actor + 8u, 0x0000u);
    wr_le_u16(actor + 0xAu, 0x0004u);
}
/** @brief Accessor: the ACTION latch. */
int32_t object_action_latched(void) { return _obj_action_latch != 0; }
/** @brief Accessor: set the ACTION latch. */
void object_set_action_latched(int32_t on)
{ _obj_action_latch = on ? 0xFFu : 0u; }

/** 1000:83B7 driver_call: INT 60h command 5 with CX = n, gated by [0x5E5B]
 * (sound enabled) and [0x5AFE] (a driver installed).  The port's audio surface
 * has no sfx entry until M6, so the request is recorded and nothing plays;
 * `object_last_sfx` exposes it so a test can pin which handler asked for which
 * effect.  `n = 0xFF` is the "none" sentinel (1000:83C1). */
static void _obj_driver_call(uint32_t n) { _obj_sfx = n; }
/** @brief Test seam: call the sound driver. */
void object_driver_call(uint32_t n) { _obj_driver_call(n); }
/** @brief Accessor: the last sound effect. */
uint32_t object_last_sfx(void) { return _obj_sfx; }

/** 1000:EA33: the acting entity's bit in the two stack masks, 0x64AD and
 * 0x64C1, the same `1 << (7 - entity)` the stack verbs use. */
static int32_t _obj_stack_masked(void)
{
    return ((_obj_stack_mask[0] | _obj_stack_mask[1]) &
            _obj_index_bit(_obj_entity)) != 0;
}

/** 1000:F9E4 object_advance_frame: the once-per-frame gate.  The first call in a
 * frame stores the frame number at `+0xA` and resets the 0x19-frame countdown
 * (`+0xE`); the repeats tick it down and, when it expires, reset it.  Returns 1
 * while the record must wait, 0 on the 25-frame tick (the original's ZF: every
 * path EXCEPT the expiry ends in `XOR AX,AX`). */
static int32_t _obj_frame_cadence(uint8_t *actor)
{
    uint32_t ax = _obj_frame;

    if (ax == rd_le_u16(actor + 0xAu)) { /* 1000:F9FD */
        uint16_t e =
            (uint16_t)(rd_le_u16(actor + 0xEu) - 1u); /* 1000:FA01 SUB */

        wr_le_u16(actor + 0xAu, (uint16_t)(ax + 1u));
        wr_le_u16(actor + 0xEu, e);
        if ((int16_t)e < 0) {
            wr_le_u16(actor + 0xEu, 0x0019u); /* 1000:FA0A */
            return 0;
        }
        return 1;
    }
    wr_le_u16(actor + 0xAu, (uint16_t)(ax + 1u)); /* 1000:F9EC */
    wr_le_u16(actor + 0xEu, 0x0019u);
    wr_le_u16(actor + 0x10u, 0u);
    return 1;
}

/** 1000:F23A: place the record at ([0x4230], [0x4234]) when the landing probe at
 * ([0x4230], 0x18) comes back 0..7 px below that row.  The probe is
 * `1000:E389`'s; its `BP` argument is the item table - see re_map.md, "the
 * handler's BP", for the open question about what the dispatcher leaves there. */
static void _obj_place_at(uint8_t *actor)
{
    uint32_t cell;
    int32_t bx = _obj_land_y((int32_t)(int16_t)_obj_side, 0x18, &cell);
    int32_t d;

    if (bx < 0) return; /* 1000:F243 */
    d = bx - 0x18 - (int32_t)(int16_t)rd_le_u16(actor + 2u);
    if (d < 0) return; /* 1000:F24B */
    if (d - 8 >= 0) return; /* 1000:F250 */
    wr_le_u16(actor + 4u, _obj_sel_xoff); /* 1000:F255 */
    wr_le_u16(actor + 8u, _obj_sel_frame);
}

/** 1000:FD7B: the span's +0x12 flag is 0xFFFF when d equals the entry's
 * frame, 0 below it and 1 above it. */
uint16_t object_span_flag_for_probe(int32_t d, uint32_t frame)
{
    if (d == (int32_t)frame) return 0xFFFFu;
    return d < (int32_t)frame ? 0u : 1u;
}

/** 1000:FC81: the point scan.  Runs only on even frames (on odd ones it just
 * clears the ACTION latch).  From (xoff + record.x, record.y + 0x10) it walks
 * the map down a row at a time until a cell's page byte is a surface (bit 7
 * clear); the surface's height gives the distance `d` (clamped to 0x5F) to the
 * floor below.  It then fills one of the two 0x1C-byte span entries at 0x6F92 -
 * the one owned by the acting entity, else the first whose word 0 still has bit
 * 15 set - with the point, the distance and a direction word (1 when the found
 * extent is above the requested y), and copies `[0x423C]` into the record's
 * type.  An entry whose +6 is 0 copies its own point into +0xA/+0xC. */
static void _obj_scan_point(uint8_t *actor, uint32_t xoff)
{
    int32_t x = (int32_t)(int16_t)rd_le_u16(actor + 0u) + (int32_t)xoff;
    int32_t y = (int32_t)(int16_t)rd_le_u16(actor + 2u) + 0x10;
    int32_t top = y, d;
    uint32_t b;
    uint8_t *span = NULL;
    int32_t fallback = -1;

    if (_obj_frame & 1u) { /* 1000:FC81 */
        object_set_action_latched(0);
        return;
    }
    while (1) { /* 1000:FCBA */
        uint32_t idx = 2u * ((uint32_t)y & 0xFFF0u) + ((uint32_t)x >> 4);

        b = level_page_byte(_obj_map_buffer_byte(idx), (uint32_t)x & 0xFu);
        if (!(b & 0x80u)) { /* 1000:FCE0: a surface */
            y += (int32_t)(int8_t)b;
            break;
        }
        y += 0x10;
        if ((uint32_t)y >= 0x140u) {
            y = 0x140;
            break;
        } /* 1000:FCC8 */
    }
    if (y > 0x140) y = 0x140; /* 1000:FCE9 */
    d = y - 1 - top; /* 1000:FCF2 */
    if (d <= 0) return;
    if (d > 0x5F) d = 0x5F;
    _obj_sel_xoff = 0; /* 1000:FD0E */
    _obj_sel_frame = 0;
    _obj_side = 1; /* [0x4230] = the entry count - 1 */
    for (uint32_t i = 0; i < 2u; i++) { /* 1000:FD20 */
        uint8_t *e = (uint8_t *)_obj_span[i];

        if (rd_le_u16(e + 0u) & 0x8000u) fallback = (int32_t)i;
        if (rd_le_u16(e + 0u) == (uint16_t)_obj_entity) {
            _obj_sel_frame = rd_le_u16(e + 6u);
            span = e;
            break;
        }
        _obj_side = (uint16_t)(_obj_side - 1u); /* 1000:FD3B DEC [0x4230] */
    }
    if (!span) { /* 1000:FD41 */
        if (fallback < 0) return;
        span = (uint8_t *)_obj_span[fallback];
    }
    wr_le_u16(span + 0u, (uint16_t)_obj_entity); /* 1000:FD4D */
    wr_le_u16(span + 2u, (uint16_t)x);
    wr_le_u16(actor + 8u, _obj_sel_type); /* 1000:FD52 */
    wr_le_u16(span + 4u, (uint16_t)top);
    wr_le_u16(span + 6u, _obj_sel_frame);
    wr_le_u16(span + 8u, (uint16_t)d);
    if (_obj_sel_frame == 0u) { /* 1000:FD6C */
        wr_le_u16(span + 6u, 0u);
        wr_le_u16(span + 0xAu, rd_le_u16(span + 2u));
        wr_le_u16(span + 0xCu, (uint16_t)top);
        _obj_span_top[((uint16_t *)span - _obj_span[0]) / 14u] = (uint16_t)top;
    }
    wr_le_u16(span + 0x12u, object_span_flag_for_probe(
                                d, (uint32_t)_obj_sel_frame)); /* 1000:FD7B */
}

/** 1000:FAF4: the neighbour pass.  For every appended record whose class slot is
 * 4, that sits within (-0x10, 0] of the probe point on both axes and whose type
 * magnitude is <= 0xA, it reads the item word under the record's feet
 * (x + (delta >= 0 ? 0x10 : 0), y + 8) and, when the fine-x bit is clear, adds
 * the caller's delta to the record's type word.  `delta` is +0xA from 1000:EC99
 * (a step right) and -0xA from 1000:ECA8 (a step left). */
static void _obj_neighbour_pass(uint8_t *actor, uint32_t offX, uint32_t offY,
                                int32_t delta)
{
    int32_t x = (int32_t)(int16_t)(uint16_t)(rd_le_u16(actor + 0u) + offX);
    int32_t y = (int32_t)(int16_t)(uint16_t)(rd_le_u16(actor + 2u) + offY);
    uint32_t n = _obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0 : 0u;

    _obj_aux = (uint16_t)delta; /* 1000:FAF9 */
    _obj_sel_frame = delta >= 0 ? 0x10u : 0u; /* 1000:FAFD */
    _obj_sel_type = 8u; /* 1000:FB03 */
    for (uint32_t k = 0; k < n; k++) { /* 1000:FB29 */
        uint8_t *r = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        int32_t d, ty;

        _obj_side = rd_le_u16(r + 0u);
        _obj_sel_xoff = rd_le_u16(r + 2u);
        if ((_obj_slot_class[1u + k] & 0x3Fu) != 4u) continue; /* 1000:FB39 */
        if (r[6] & 0x40u) continue; /* 1000:FB46 */
        d = (int16_t)(uint16_t)(rd_le_u16(r + 0u) - (uint16_t)x);
        if (d >= 0 || d + 0x10 < 0) continue; /* 1000:FB4F */
        d = (int16_t)(uint16_t)(rd_le_u16(r + 2u) - (uint16_t)y);
        if (d >= 0 || d + 0x10 < 0) continue; /* 1000:FB62 */
        ty = (int16_t)rd_le_u16(r + 8u);
        if (ty < 0) ty = -ty;
        if (ty > 0xA) continue; /* 1000:FB7E */
        {
            int32_t px =
                (int32_t)(int16_t)rd_le_u16(r + 0u) + (delta >= 0 ? 0x10 : 0);
            int32_t py = (int32_t)(int16_t)rd_le_u16(r + 2u) + 8;
            uint32_t sub, tile = _obj_map_at_abs(px, py, &sub);
            uint32_t word = level_item_word(tile, (uint32_t)(py & 0xF));
            uint32_t bit = 1u << (0xFu - ((uint32_t)px & 0xFu));

            if (!(word & bit)) /* 1000:FC17 */
                wr_le_u16(r + 8u, (uint16_t)(rd_le_u16(r + 8u) +
                                             _obj_aux)); /* 1000:FC1D */
        }
    }
}

/** 1000:EE3B: the enter-the-building sequence, run in place of the handler while
 * [0x6F1D] is set.  It releases the latch, folds the record's direction counter
 * (`+0xE`, 0..0x1FF) into a distance and the frame (0x8C..0x93) into one of the
 * eight `0x6E76` four-word vectors - the door's x, its y, the record's new frame
 * and type - offsets the door x by that distance, frees the record's class slot
 * and turns that slot's placed record into a marker (type 0xFFFF, the door's
 * coordinates in its +0xC/+0xE), then arms the ACTION dispatch ([0x5B56] = 1,
 * [0x6916] = 5, [0x6917] = 1) and plays effect 3. */
static void _obj_cmd_enter(uint8_t *actor)
{
    static const int16_t doors[8][4] = {
        /* 0x6E76: dx, y, frame, type */
        { 3, -5, 0, 0 }, { 2, -7, 0, 0 },  { 1, -9, 0, 0 },  { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },  { -3, -5, 8, 1 }, { -2, -7, 8, 1 }, { -1, -9, 8, 1 },
    };
    uint32_t ax, idx, dx;
    uint8_t *rec;

    if (!_obj_lat) { /* 1000:EE42 */
        wr_le_u16(actor + 0xEu, 0u);
        return;
    }
    _obj_lat = 0; /* 1000:EE48 */
    ax = rd_le_u16(actor + 0xEu);
    if (ax >= 0x100u) ax = 0x1FFu - ax; /* 1000:EE55 */
    ax >>= 6;
    dx = (uint32_t)((rd_le_u16(actor + 4u) - 0x8Cu) & 0xFFFFu);
    if (dx >= 8u) return; /* past 0x6E76's eight entries */
    _obj_pt_x = (uint16_t)(doors[dx][0] >= 0 ? doors[dx][0] + (int32_t)ax
                                             : doors[dx][0] - (int32_t)ax);
    _obj_pt_y = (uint16_t)doors[dx][1]; /* 1000:EE8B */
    wr_le_u16(actor + 4u, (uint16_t)doors[dx][2]); /* 1000:EE94 */
    wr_le_u16(actor + 8u, (uint16_t)doors[dx][3]);
    wr_le_u16(actor + 0xAu, 0u);

    idx = actor[7]; /* 1000:EEB8: the 1-based class */
    actor[7] = 0u;
    /* The class byte write is `AND [0x3E5E + idx], 0x3F` (idx 0 reaches
     * 0x3E5E through the `DEC DX` wrap).  The record write converts the class
     * with `DEC DX` / `MUL DL` and does no range test: for a class byte of 0
     * the factor is 0xFFFF, `MUL DL` uses only 0xFF, and the writes land at
     * 0x3F17 + 0x11EE = 0x5105 (+8), i.e. on 0x510D..0x5114. */
    _obj_class_byte_set(idx,
                        _obj_class_byte_at(idx) & 0x3Fu); /* 0x3E5F + idx - 1 */
    if (idx == 0u) { /* the reachable out-of-range case: record base 0x5105 */
        rec = _obj_ds_tail5105;
    } else if (8u + idx < OBJ_SLOTS) {
        rec = _obj_array + (8u + idx) * OBJ_STRIDE; /* 0x3F17 + 0x12*(idx-1) */
    } else {
        rec = NULL; /* malformed class: the bound is D-101 */
    }
    if (rec) {
        wr_le_u16(rec + 8u, 0xFFFFu); /* 1000:EED5 */
        wr_le_u16(rec + 0xAu, 0u);
        wr_le_u16(rec + 0xCu, _obj_pt_x); /* 1000:EEE5 */
        wr_le_u16(rec + 0xEu, _obj_pt_y);
    }
    _obj_set_program_index(1u); /* 1000:EEF2: [0x5B56] = 1 */
    _obj_out_state = 5; /* 1000:EEF7 */
    _obj_out_state_hi = 1; /* 1000:EEFC */
    _obj_driver_call(3);
}
/** @brief Set the 0x6FE6 flag. */
static void _obj_flag6f_e6_set(void) { _obj_flag6f_e6 = 0xFFu; }
/** the flag's reader is unported, so expose the store for the test */
int32_t object_flag6f_e6(void) { return _obj_flag6f_e6 != 0u; }

/** 1000:FDBF: the down-walking twin of 0xFD91 (y++ and the direction bytes are
 * the mirror image). */
static void _obj_frame_step_down(uint8_t *actor, uint32_t lo, uint32_t hi)
{
    uint32_t f;

    wr_le_u16(actor + 2u,
              (uint16_t)(rd_le_u16(actor + 2u) + 1u)); /* 1000:FDBF */
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

/** @brief Accessor: one word of the animation list. */
static uint32_t _obj_anim_list_word(uint32_t off)
{ return off + 1u < sizeof _obj_anim_list ? _obj_anim_list[off / 2u] : 0u; }

/* --- 1000:F5BC: the level-end probe the movers' tails call --------------
 *
 * `BX` names the record (>= 0) or is -1 for the caller; `[0x6F1D]` is cleared
 * first.  The gate is the tribe odometer: while `[0x5B25] | [0x5B26]` is zero
 * the record is **taken** - freed, its class slot cleared, and, when it was the
 * acting record, the first placed record whose type is neither 0x16, 0x1B..0x1E
 * nor 0x21 is resolved through `0x4147`; when none is, the level is lost
 * (`[0x138E] = [0x1809] = 0`, `[0x1979] = 2`, `[0x6916] = 2`).  With the
 * odometer showing a count it is the **goal form** instead: `[0x5B22] = 0xFF`,
 * the position from `[0x4220]`/`[0x4222]` (the settle cache `0x6847` when x is
 * negative), frame 0x2D / type 0x21 / `+0xA` = 5, the acting-record flags, and
 * the 0xE389 probe on the landing.
 */
/** @brief 1000:F682: the level-end probe's goal form. */
static void _obj_level_end_goal(uint8_t *t, uint32_t idx)
{
    statusbar_arm_odometer(); /* [0x5B22] = 0xFF */
    wr_le_u16(t + 2u, _obj_sel_xoff); /* [0x4222] */
    wr_le_u16(t + 0u, _obj_side); /* [0x4220] */
    if ((int16_t)_obj_side < 0) { /* 1000:F692 */
        wr_le_u16(t + 0u, (uint16_t)((_obj_settled[idx][0] + 8u) & 0xFFF0u));
        wr_le_u16(t + 2u, _obj_settled[idx][1]);
    }
    wr_le_u16(t + 0u, (uint16_t)(rd_le_u16(t + 0u) - 8u)); /* 1000:F6B1 */
    wr_le_u16(t + 4u, 0x002Du);
    t[6] = 0u;
    wr_le_u16(t + 8u, 0x0021u);
    wr_le_u16(t + 0xAu, 5u);
    if (idx == _obj_entity && (int32_t)idx != _obj_player && !t[7]) {
        _obj_set_program_index(1u); /* [0x5B56] = 1 */
        _obj_out_state = 5; /* [0x6916] = 0x105 */
        _obj_out_state_hi = 1;
    }
    {
        uint32_t cell;
        int32_t land =
            _obj_land_y((int32_t)(int16_t)rd_le_u16(t + 0u) + 0x10,
                        (int32_t)(int16_t)rd_le_u16(t + 2u) + 0x18, &cell);

        if (land >= 0)
            wr_le_u16(t + 2u, (uint16_t)(land - 0x18)); /* 1000:F6F2 */
    }
}

/** @brief The level-end probe the movers' tails call. */
static void _obj_level_end_probe(uint8_t *actor, int32_t bx)
{
    uint8_t *t;
    uint32_t idx;

    _obj_lat = 0; /* 1000:F5BC */
    if (bx >= 0) {
        t = object_slot((uint32_t)bx);
        idx = (uint32_t)bx;
    } else {
        t = actor;
        idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);
    }
    if (!t) return;
    if (statusbar_tribe_active()) { /* 1000:F5E3 */
        _obj_level_end_goal(t, idx);
        return;
    }
    wr_le_u16(t + 0u, 0xFFFFu); /* 1000:F5EF: taken */
    wr_le_u16(t + 2u, 0xFFFFu);
    t[6] = 0xFFu;
    wr_le_u16(t + 8u, 0x001Bu);
    if (t[7]) { /* 1000:F603 */
        uint32_t cls = t[7];

        _obj_class_byte_set(cls, 0u); /* 0x3E5F + cls - 1 */
        if (8u + cls < OBJ_SLOTS)
            _obj_array[(8u + cls) * OBJ_STRIDE + 6] = 0xFFu;
        t[7] = 0u;
    }
    if (idx != _obj_entity) return; /* 1000:F627 */
    for (uint32_t i = 0; i < 8u; i++) { /* 1000:F630 */
        uint8_t *r = object_slot(i);
        uint32_t ty;

        if (!r || (r[6] & 0x80u)) continue;
        ty = rd_le_u16(r + 8u);
        if (ty == 0x16u) continue;
        if (ty >= 0x1Bu && ty <= 0x1Eu) continue;
        if (ty == 0x21u) continue;
        _obj_resolve_sprite(i); /* 1000:F664 -> 0x4147 */
        return;
    }
    level_set_end(0); /* 1000:F66D */
}

/** @brief Accessor: one byte of a length-checked table. */
static uint32_t _obj_tab_byte(const uint8_t *t, uint32_t n, uint32_t off)
{ return off < n ? t[off] : 0u; }

/** 1000:DBCD (type 28): a 9-frame stepper over 0x6868 (the frame is a table
 * byte, +0xC the cursor); at frame 0xD3 the record climbs one pixel per tick
 * while its y's low nibble is non-zero, and at a row boundary the level-end
 * probe runs with the scratch pair at 0xFFFF. */
static void _obj_tail_type28(uint8_t *actor)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); /* 1000:DBCD */
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 9u);
    if (rd_le_u16(actor + 4u) != 0x00D3u) { /* 1000:DBD7 */
        uint32_t off = rd_le_u16(actor + 0xCu);

        actor[4] =
            (uint8_t)_obj_tab_byte(_obj_tab6868, sizeof _obj_tab6868, off);
        wr_le_u16(actor + 0xCu, (uint16_t)(off + 1u));
        return;
    }
    if (rd_le_u16(actor + 2u) & 0xFu) { /* 1000:DBEC */
        wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 1u));
        return;
    }
    _obj_side = 0xFFFFu; /* [0x4220] */
    _obj_sel_xoff = 0xFFFFu; /* [0x4222] */
    _obj_level_end_probe(actor, -1); /* 1000:DC11 */
}

/** 1000:DE7E (type 30): the 0x687C table walk. */
static void _obj_tail_type30(uint8_t *actor)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); /* 1000:DE7E */
    if ((int16_t)a >= 0) return;
    {
        uint32_t off = rd_le_u16(actor + 0xCu);
        int32_t dx = (int32_t)(int8_t)(uint8_t)_obj_tab_byte(
            _obj_tab687_c, sizeof _obj_tab687_c, off);
        int32_t dy = (int32_t)(int8_t)(uint8_t)_obj_tab_byte(
            _obj_tab687_c, sizeof _obj_tab687_c, off + 1u);
        uint32_t nxt = off + 2u;

        if (_obj_tab_byte(_obj_tab687_c, sizeof _obj_tab687_c, off + 2u) ==
            0u) { /* 1000:DE9F */
            uint32_t e;

            nxt = (uint16_t)(nxt - 4u); /* 1000:DEA5 */
            e = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u);
            wr_le_u16(actor + 0xEu, e); /* 1000:DEA8 */
            if ((int16_t)e < 0 && statusbar_tribe_active()) { /* 1000:DEB4 */
                _obj_side = rd_le_u16(actor + 0u); /* [0x4220] */
                _obj_sel_xoff = rd_le_u16(actor + 2u); /* [0x4222] */
                _obj_level_end_probe(actor, -1); /* 1000:DEC4 */
                return;
            }
        }
        wr_le_u16(actor + 0xCu, nxt); /* 1000:DEC7 */
        wr_le_u16(actor + 0xAu, (uint16_t)(int16_t)dy); /* [SI+0xA] = CX */
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) + (uint16_t)(int16_t)dx));
    }
}

/** 1000:DED1 (type 34): a two-state tail on the record `+0xA` points at.  While
 * the +6 bit 7 is set: a target `+4` of 2 or 3 holds, anything else runs the
 * level-end probe at ([SI+0x10], [target+8] + 0x28).  With the bit clear: a
 * target `+4` of 2 or 3 sets the bit when `[target+0xC] == +0xE`, and anything
 * else takes the home form (frame 0x10 / type 2, `+0xA`/`+0xC` cleared). */
static void _obj_tail_type34(uint8_t *actor)
{
    /* 1000:DED1: +0xA is the ENTITY record pointer, not an object link.  The
     * entity interaction tags the index (0x8000|n); an untagged link is not
     * a carry and leaves the record alone. */
    uint32_t link = rd_le_u16(actor + 0xAu);
    const Entity *e = (link & 0x8000u) ? entity_at(link & 0x7FFFu) : NULL;
    uint32_t v;

    if (!e) return;
    v = e->handler;
    if (v == 2u || v == 3u) { /* 1000:DEDA / 0xDF0A */
        if (!(actor[6] & 0x80u)) {
            if (e->waypoint == rd_le_u16(actor + 0xEu)) actor[6] |= 0x80u;
        }
        return;
    }
    if (actor[6] & 0x80u) { /* 1000:DEE8 */
        _obj_side = rd_le_u16(actor + 0x10u); /* [0x4220] */
        _obj_sel_xoff = (uint16_t)(e->y + 0x28u); /* [0x4222] */
        _obj_level_end_probe(actor, -1);
        return;
    }
    wr_le_u16(actor + 4u, 0x0010u); /* 1000:DF17 */
    wr_le_u16(actor + 8u, 0x0002u);
    wr_le_u16(actor + 0xAu, 0u);
    wr_le_u16(actor + 0xCu, 0u);
}

/** 1000:EF66 (type 22): a 5-frame stepper to frame 0x2E, then the level-end
 * probe with the scratch pair at 0xFFFF. */
static void _obj_tail_type22(uint8_t *actor)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); /* 1000:EF66 */
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 5u);
    wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
    if (rd_le_u16(actor + 4u) != 0x2Eu) return; /* 1000:EF73 */
    _obj_side = 0xFFFFu; /* [0x4220] */
    _obj_sel_xoff = 0xFFFFu; /* [0x4222] */
    _obj_level_end_probe(actor, -1); /* 1000:EF93 */
}

/* --- the rows' tails (the +0xC word of each DS:0x698A row) --------------
 *
 * `1000:E4D1` reads the tail word from the row and `1000:E4E0` calls it after
 * the handler and the `+0xA` hook.  Twenty-seven distinct tails exist; the ones
 * below are the rows' own, each cited to its entry.
 */

/** 1000:D9D0 (shared body of 0xD9C8 / 0xD9CD, types 31 and 32): a 7-frame hold
 * that steps the frame up and wraps four back at the bound (0x112 / 0x116). */
static void _obj_tail_frame_bound(uint8_t *actor, uint32_t bound)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); /* 1000:D9D0 */
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 7u); /* 1000:D9D6 */
    wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
    if (rd_le_u16(actor + 4u) == bound)
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) - 4u)); /* 1000:D9E3 */
}

/* 0xD9C8 / 0xD9CD: `MOV AX,bound; JMP 0xD9D0`. */

/** 1000:DB30: the frame -> type map the landing tails share. */
static void _obj_tail_frame_to_type(uint8_t *actor)
{
    uint32_t t = 2;

    if (rd_le_u16(actor + 4u) < 0x10u) { /* 1000:DB33 */
        t = 1;
        if (rd_le_u16(actor + 4u) < 8u) t = 0;
    }
    wr_le_u16(actor + 8u, t); /* 1000:DB44 */
}

/** 1000:DB15: the hard landing. */
static void _obj_tail_crash(uint8_t *actor)
{
    wr_le_u16(actor + 0u,
              (uint16_t)(rd_le_u16(actor + 0u) - 7u)); /* 1000:DB15 */
    wr_le_u16(actor + 4u, 0x0028u);
    wr_le_u16(actor + 8u, 0x0016u);
    wr_le_u16(actor + 0xAu, 5u);
    _obj_driver_call(4); /* 1000:DB28 */
}

/** @brief 1000:DB05: the type-3 tail's coast (no landing). */
static void _obj_tail_type3_coast(uint8_t *actor, uint32_t dx)
{
    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + dx));
    wr_le_u16(actor + 0xEu, (uint16_t)(rd_le_u16(actor + 0xEu) + dx));
    wr_le_u16(actor + 0xAu,
              (uint16_t)(dx == rd_le_u16(actor + 0xCu) ? dx : dx + 1u));
}

/** @brief 1000:DAB2: the type-3 tail's landing. */
static void _obj_tail_type3_land(uint8_t *actor, uint32_t bx)
{
    uint32_t sp;

    wr_le_u16(actor + 2u, (uint16_t)(bx - 0x18u));
    sp = rd_le_u16(actor + 0xEu);
    if (sp >= 0x30u) {
        _obj_tail_crash(actor);
        return;
    } /* 1000:DABF */
    level_set_offset_word((uint16_t)(sp >> 3)); /* 1000:DAC7 */
    _obj_tail_frame_to_type(actor); /* 1000:DACA */
    _obj_driver_call(5);
}

/** 1000:D9E8 (type 3): the gravity tail.  The gate is the same 0x6EBE/row one
 * the settle uses; then a probe at (x + 8, y + 0x18) with the row above it, the
 * 0xF0..0xF2 platform snap (frame 0xCD / type 0x1C / effect 6), and otherwise
 * the page byte of the feet / one row above / one row below picking a landing:
 * a descent of 0x30 or more crashes (frame 0x28 / type 0x16 / effect 4), a
 * slower one arms [0x2D8E] = speed >> 3, takes the frame->type map and plays
 * effect 5.  `[0x4258]` is the map buffer's end (map + 0x280 = 640 here). */
static void _obj_tail_type3(uint8_t *actor)
{
    const uint8_t *map = level_map();
    uint32_t ax, bx, cx, dx, di, b;
    int32_t sc;

    if (!_obj_settle_gate(actor)) { /* 1000:D9EB */
        _obj_tail_frame_to_type(actor);
        return;
    }
    ax = (uint32_t)(rd_le_u16(actor + 0u) + 8u);
    bx = (uint32_t)(rd_le_u16(actor + 2u) + 0x18u);
    dx = rd_le_u16(actor + 0xAu);
    if (bx >= 0x158u) {
        _obj_tail_crash(actor);
        return;
    } /* 1000:DA02 */
    cx = bx;
    bx &= 0xFFF0u;
    di = 2u * bx + (ax >> 4); /* 1000:DA0C..DA23 */
    ax &= 0xFu; /* 1000:DA1C: AX = the FINE x */
    if (map) {
        b = _obj_map_buffer_byte(di - 0x20u); /* 1000:DA30 */

        if (b >= 0xF0u && b - 0xF0u < 3u) { /* 1000:DA37/DA39 */
            /* 1000:DA3E..0xDA61 writes only y/frame/type/+C/+A - no x store
             * (the x snap belongs to 0xF6F6's branch, not this one).  Writing
             * it teleported a drowning actor to x -4 (D-78). */
            wr_le_u16(actor + 2u, (uint16_t)(bx - 0x1Cu));
            wr_le_u16(actor + 4u, 0x00CDu);
            wr_le_u16(actor + 8u, 0x001Cu);
            wr_le_u16(actor + 0xCu, 0u);
            wr_le_u16(actor + 0xAu, 9u);
            _obj_driver_call(6);
            return;
        }
    }
    if (di >= 0x280u || !map) { /* 1000:DA63 */
        _obj_tail_type3_coast(actor, dx);
        return;
    }
    b = level_page_byte(_obj_map_buffer_byte(di), ax); /* 1000:DA71 */
    if (b & 0x80u) { /* 1000:DA85 */
        di += 0x20u;
        if (di >= 0x280u) { /* 1000:DA8C */
            _obj_tail_type3_coast(actor, dx);
            return;
        }
        b = level_page_byte(_obj_map_buffer_byte(di), ax);
        if (b & 0x80u) { /* 1000:DAA6 */
            _obj_tail_type3_coast(actor, dx);
            return;
        }
        bx += 0x10u; /* 1000:DAA8 */
        sc = (int32_t)(int8_t)(uint8_t)b;
        bx = (uint32_t)((int32_t)bx + sc);
        cx = (uint32_t)((int32_t)cx - (int32_t)bx);
        if ((int32_t)cx < 0) {
            cx = (uint32_t)-(int32_t)cx;
            if (cx > dx) { /* 1000:DB01 */
                _obj_tail_type3_coast(actor, dx);
                return;
            }
        }
        _obj_tail_type3_land(actor, bx); /* 1000:DAB2 */
        return;
    }
    if (b != 0u) { /* 1000:DAD6 */
        sc = (int32_t)(int8_t)(uint8_t)b;
        bx = (uint32_t)((int32_t)bx + sc);
        cx = (uint32_t)((int32_t)cx - (int32_t)bx);
        if ((int32_t)cx < 0) {
            cx = (uint32_t)-(int32_t)cx;
            if (cx > dx) {
                _obj_tail_type3_coast(actor, dx);
                return;
            }
        }
        _obj_tail_type3_land(actor, bx);
        return;
    }
    /* b == 0: look two rows above (1000:DAD8) */
    if (di >= 0x40u) {
        b = level_page_byte(_obj_map_buffer_byte(di - 0x40u), ax);
        _obj_side = (uint16_t)(int16_t)(int8_t)(uint8_t)b; /* [0x4230] */
        if (!(b & 0x80u)) {
            bx -= 0x10u;
            sc = (int32_t)(int16_t)_obj_side;
            bx = (uint32_t)((int32_t)bx + sc);
            cx = (uint32_t)((int32_t)cx - (int32_t)bx);
            if ((int32_t)cx < 0) {
                cx = (uint32_t)-(int32_t)cx;
                if (cx > dx) {
                    _obj_tail_type3_coast(actor, dx);
                    return;
                }
            }
            _obj_tail_type3_land(actor, bx);
            return;
        }
    }
    sc = 0; /* 1000:DA87: AX = 0 */
    bx = (uint32_t)((int32_t)bx + sc);
    cx = (uint32_t)((int32_t)cx - (int32_t)bx);
    if ((int32_t)cx < 0) {
        cx = (uint32_t)-(int32_t)cx;
        if (cx > dx) {
            _obj_tail_type3_coast(actor, dx);
            return;
        }
    }
    _obj_tail_type3_land(actor, bx);
}

/** 1000:DB7A: find the actor's span entry.  Returns the matched entry (even
 * when a gate fails, because the original leaves it in DI for the walk) and
 * sets `yes` when the gates at DB9D/DBA4 also pass.  No match returns NULL
 * (the original's DI is then past the two entries, i.e. the zero word). */
static uint8_t *_obj_span_owner(uint8_t *actor, int32_t *yes)
{
    uint32_t idx = (uint32_t)((actor - _obj_array) / OBJ_STRIDE);
    uint8_t *found = NULL;

    *yes = 0;
    for (uint32_t i = 0; i < 2u; i++) { /* AX = 1: two entries */
        uint8_t *e = (uint8_t *)_obj_span[i];

        if (rd_le_u16(e + 0u) != (uint16_t)(idx & 0x7FFFu))
            continue; /* 1000:DB8D */
        found = e;
        if (!(rd_le_u16(e + 0x12u) & 0x8000u)) return found; /* 1000:DB9D */
        if (rd_le_u16(e + 8u) != 0u) return found; /* 1000:DBA4 */
        *yes = 1;
        return found;
    }
    return found;
}

/** 1000:DBAE: walk the actor's frame toward AX on a 3-frame hold.  The span's
 * height (+6) equal to its fall distance (+8) holds the frame; a NULL span is
 * the original's past-the-table DI, which reads as two zero words. */
static void _obj_tail_walk_to_frame(uint8_t *actor, uint32_t frame,
                                    const uint8_t *span)
{
    uint32_t a;
    uint16_t height = span ? rd_le_u16(span + 6u) : 0u;
    uint16_t dist = span ? rd_le_u16(span + 8u) : 0u;

    if (height == dist) return; /* 1000:DBAE..0xDBCC */
    a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);
    wr_le_u16(actor + 0xAu, a); /* 1000:DBB6 */
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 3u); /* 1000:DBBB */
    if (rd_le_u16(actor + 4u) == frame)
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) + 1u)); /* 1000:DBC9 */
    else
        wr_le_u16(actor + 4u,
                  (uint16_t)(rd_le_u16(actor + 4u) - 1u)); /* 1000:DBC5 */
}

/** 1000:DB48 / 1000:DB61 (types 9 and 10): the span's owner test; on "yes" the
 * matched entry is freed and the acting record is reset (frame/type 0/0 or
 * 8/1), otherwise it walks toward 0xAB / 0xAD. */
static void _obj_tail_span_owner(uint8_t *actor, uint32_t frame, uint32_t f8,
                                 uint32_t bad)
{
    int32_t yes;
    uint8_t *span = _obj_span_owner(actor, &yes);

    if (yes) { /* 1000:DB4B */
        wr_le_u16(span + 0u, 0xFFFFu); /* 1000:DB4D: free the matched entry */
        wr_le_u16(actor + 4u, (uint16_t)frame);
        wr_le_u16(actor + 8u, (uint16_t)f8);
        return;
    }
    _obj_tail_walk_to_frame(actor, bad, span); /* 1000:DB5C -> 0xDBAE */
}

/** 1000:DD93 (type 16): a 10-frame stepper that walks the frame through
 * 0x55..0x57, flipping the direction byte at +0xC and playing effect 8 at 0x55;
 * a `+0xE` countdown then drops it back to frame 0x10 / type 2. */
static void _obj_tail_type16(uint8_t *actor)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a);
    if ((int16_t)a < 0) { /* 1000:DD96 */
        uint32_t f = rd_le_u16(actor + 4u);

        wr_le_u16(actor + 0xAu, 0x0Au); /* 1000:DD98 */
        if (actor[0xCu] != 0u) { /* 1000:DD A0 */
            f--;
            if (f == 0x55u) { /* 1000:DD A7 */
                actor[0xCu] = 0u;
                _obj_driver_call(8);
                f = 0x55u;
            }
        } else {
            f++;
            if (f == 0x57u) actor[0xCu] = 0xFFu; /* 1000:DDC3 */
        }
        wr_le_u16(actor + 4u, f); /* 1000:DD C7 */
    }
    a = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u); /* 1000:DDCA */
    wr_le_u16(actor + 0xEu, a);
    if ((int16_t)a < 0) {
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
    }
}

/** 1000:EB5C / 1000:EB6A (types 13 and 14): one walk step, then a `+0xE`
 * countdown that sets the type to 1 (left) or 0 (right). */
static void _obj_tail_walk_countdown(uint8_t *actor, int32_t left)
{
    if (left) _obj_walk(actor, 1);
    else _obj_walk(actor, 0); /* 1000:EB5C */

    {
        uint32_t a = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u);

        wr_le_u16(actor + 0xEu, a); /* 1000:EB5F */
        if ((int16_t)a < 0)
            wr_le_u16(actor + 8u, left ? 1u : 0u); /* 1000:EB64 / 0xEB72 */
    }
}

/** 1000:E802 / 1000:E831 (types 4 and 5): on a type match, probe the cell under
 * the feet; a field-K special tile is the climb stepper (`1000:E828`/`E857`:
 * lo 0x23, hi 0x25 into 0xFD91/0xFDBF), anything else drops the record to
 * frame 0x10 / type 2.  (The 0x52/0x54 pair belongs to 1000:EAAC's type-11
 * carry stepper, not here.) */
static void _obj_tail_special_tile(uint8_t *actor, uint32_t type, int32_t yoff,
                                   int32_t down)
{
    uint32_t cell, sub;

    if (rd_le_u16(actor + 8u) != type) return; /* 1000:E802 */
    cell =
        _obj_map_at_abs((int32_t)(int16_t)rd_le_u16(actor + 0u) + 8,
                        (int32_t)(int16_t)rd_le_u16(actor + 2u) + yoff, &sub);
    if (cell == level_special_tile(0u) || cell == level_special_tile(1u)) {
        if (down) _obj_frame_step_down(actor, 0x23u, 0x25u); /* 1000:E857 */
        else _obj_frame_step(actor, 0x23u, 0x25u); /* 1000:E828 */
        return;
    }
    wr_le_u16(actor + 4u, 0x0010u); /* 1000:E81D */
    wr_le_u16(actor + 8u, 0x0002u);
}

/** 1000:ECB7 (type 17): step the 0x6E5C frame list by one.  While the 9-beat
 * has not expired (`1000:ECBB JNS 0xECDC`) the routine returns at the bare
 * `RET`; when it has, the frame list advances and the shared "restore home"
 * block runs - the `ADD +0xC` at 0xECD8 falls straight into 0xECDD, so both
 * branches of the expiry reach it: the frame/type come from `+0xE`/`+0x10`,
 * `+0xA`/`+0xC` are cleared and `[0x1F1C]` gets 0x82 to its `+4` and 0 to its
 * `+8`. */
static void _obj_tail_type17(uint8_t *actor)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); /* 1000:ECB7 */
    if ((int16_t)a >= 0) return; /* 1000:ECBB JNS 0xECDC */
    {
        uint32_t off = rd_le_u16(actor + 0xCu);

        wr_le_u16(actor + 0xAu, 9u);
        if (off < 0x1Au)
            wr_le_u16(actor + 4u, (uint16_t)_obj_anim_list_word(off));
        if (!(_obj_anim_list_word(off + 2u) & 0x8000u))
            wr_le_u16(actor + 0xCu, (uint16_t)(off + 2u));
    }
    wr_le_u16(actor + 4u, rd_le_u16(actor + 0xEu)); /* 1000:ECDD */
    wr_le_u16(actor + 8u, rd_le_u16(actor + 0x10u));
    wr_le_u16(actor + 0xAu, 0u);
    wr_le_u16(actor + 0xCu, 0u);
    /* 1000:ECF1 writes [0x1F1C]'s +4 = 0x82 and +8 = 0; that pointer has no
     * ported writer (re_map.md, "the [0x1F1C] record"), so it is not stored. */
}

/** 1000:DDDA (type 29): the "join the tribe" tail.  It compares the *acting*
 * record with this one (0x79F4 returns it in SI): when the actor is within the
 * 0x20 window, the actor is collected - type 2, its class record marked, the
 * two counters bumped ([0x3E20], [0x11FC]) - with effect 0xC; otherwise a slow
 * two-frame animation toward frame 0xFE and then 0x102 on its class record. */
static void _obj_tail_type29(uint8_t *actor)
{
    uint8_t *e = object_slot(object_action_entity());
    int32_t dx, dy;
    uint32_t a;

    if (e && rd_le_u16(e + 8u) <= 2u) { /* 1000:DDE4 */
        dx = (int32_t)(int16_t)rd_le_u16(actor + 0u) -
             (int32_t)(int16_t)rd_le_u16(e + 0u);
        dy = (int32_t)(int16_t)rd_le_u16(actor + 2u) -
             (int32_t)(int16_t)rd_le_u16(e + 2u);
        if (dx < 0 && dx + 0x20 >= 0 && dy < 0 && dy + 0x20 >= 0) {
            uint8_t *cls = _obj_link_record(rd_le_u16(actor + 0xCu));

            wr_le_u16(actor + 8u, 2u); /* 1000:DDFD */
            wr_le_u16(actor + 0xAu, 0u);
            if (cls) cls[6] = 0x80u; /* 1000:DE0A */
            wr_le_u16(actor + 4u, 0x0010u);
            wr_le_u16(actor + 0u, (uint16_t)(rd_le_u16(actor + 0u) + 8u));
            wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(actor + 2u) + 8u));
            _obj_placed_humans++; /* 1000:DE1B: [0x3E20] */
            level_add_member(); /* 1000:DE1F: [0x11FC] */
            _obj_driver_call(0xC);
            return;
        }
    }
    a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u); /* 1000:DE2C */
    wr_le_u16(actor + 0xAu, a);
    if ((int16_t)a >= 0) return;
    if (rd_le_u16(actor + 4u) == 0x00FEu) { /* 1000:DE31 */
        uint8_t *cls;

        wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
        wr_le_u16(actor + 0xAu, 0x32u);
        cls = _obj_link_record(rd_le_u16(actor + 0xCu));
        if (cls) {
            wr_le_u16(cls + 0u, rd_le_u16(actor + 0u));
            wr_le_u16(cls + 2u, rd_le_u16(actor + 2u));
            wr_le_u16(cls + 0u,
                      (uint16_t)(rd_le_u16(cls + 0u) + 0x0Fu)); /* 1000:DE60 */
            wr_le_u16(cls + 2u, (uint16_t)(rd_le_u16(cls + 2u) - 0x0Cu));
            wr_le_u16(cls + 4u, 0x0102u);
            cls[6] = 0u;
        }
        _obj_driver_call(8);
        return;
    }
    wr_le_u16(actor + 4u,
              (uint16_t)(rd_le_u16(actor + 4u) - 1u)); /* 1000:DE38 */
    wr_le_u16(actor + 0xAu, 0xFAu);
    {
        uint8_t *cls = _obj_link_record(rd_le_u16(actor + 0xCu));

        if (cls) cls[6] = 0x80u;
    }
}

/** 1000:EAAC (type 11): the record's +0xE is the 0x6F92 span entry; its word 0
 * is the owner record, resolved through the 0x3E24 pointer table.  Compare the
 * owner's y with ours, then either the 0x52/0x54 stepper or the "home" form
 * (type = the owner's +8 + 4, frame 8 only for type 9, +0xE = 0xA). */
static uint8_t *_obj_span_entry(uint32_t off)
{
    if (off >= sizeof _obj_span) return NULL;
    return (uint8_t *)_obj_span + off;
}

/** @brief Type 11's tail handler. */
static void _obj_tail_type11(uint8_t *actor)
{
    uint8_t *span = _obj_span_entry(rd_le_u16(actor + 0xEu));
    uint8_t *t;

    if (!span) return;
    t = object_slot(rd_le_u16(span + 0u)); /* the span's owner */
    if (!t) return;
    if (rd_le_u16(actor + 2u) > rd_le_u16(t + 2u)) { /* 1000:EAB8 */
        _obj_frame_step(actor, 0x52u, 0x54u);
        return;
    }
    {
        uint32_t f = rd_le_u16(t + 8u);

        wr_le_u16(actor + 8u, (uint16_t)(f + 4u)); /* 1000:EAC9 */
        wr_le_u16(actor + 4u, f == 9u ? 8u : 0u);
        wr_le_u16(actor + 0xEu, 0x000Au); /* 1000:EAE0 */
    }
}

/** 1000:EAE6 (type 12): the same span entry; when the span's end
 * (`span+4` + `span+6`) is at or above the record's y it is home (frame 0x10,
 * type 2, [0x6FE6] = 0xFF), otherwise the map probe under its feet decides
 * between the down-stepper and home. */
static void _obj_tail_type12(uint8_t *actor)
{
    uint8_t *span = _obj_span_entry(rd_le_u16(actor + 0xEu));
    uint32_t sum, ry = rd_le_u16(actor + 2u);

    if (!span) return;
    sum = (uint32_t)(rd_le_u16(span + 4u) + rd_le_u16(span + 6u));
    if (sum > ry) { /* 1000:EAF6: span end below the record */
        uint32_t cell, sub;
        int32_t y = (int32_t)(int16_t)ry + 0x18;
        uint32_t b;

        cell = _obj_map_at_abs((int32_t)(int16_t)rd_le_u16(actor + 0u) + 8, y,
                               &sub);
        b = level_page_byte(cell, sub);
        /* 1000:EB4B: a solid page byte takes the stepper; otherwise the
         * surface line 1000:EB4D is tested against the probe line `y`. */
        if ((b & 0x80u) ||
            (uint16_t)(((uint16_t)y & 0xFFF0u) + b) > (uint16_t)y) {
            _obj_frame_step_down(actor, 0x52u, 0x54u);
            return;
        }
    }
    wr_le_u16(actor + 4u, 0x0010u); /* 1000:EAFB */
    wr_le_u16(actor + 8u, 0x0002u);
    _obj_flag6f_e6_set(); /* 1000:EB05 */
}

/** 1000:F0C9 (type 33): a 5-frame stepper down to frame 0x27, where the record
 * jumps 8 px right and returns to frame 0x10 / type 2; if it is the acting
 * record it also seeds slot 8 (0x3F05) with type 0x1A and [0x6DEA] = 0x3E8. */
static void _obj_tail_type33(uint8_t *actor)
{
    uint32_t a = (uint16_t)(rd_le_u16(actor + 0xAu) - 1u);

    wr_le_u16(actor + 0xAu, a); /* 1000:F0C9 */
    if ((int16_t)a >= 0) return;
    wr_le_u16(actor + 0xAu, 5u);
    wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) - 1u));
    if (rd_le_u16(actor + 4u) != 0x27u) return; /* 1000:F0D6 */
    wr_le_u16(actor + 0u, (uint16_t)(rd_le_u16(actor + 0u) + 8u));
    wr_le_u16(actor + 4u, 0x0010u);
    wr_le_u16(actor + 8u, 0x0002u);
    wr_le_u16(actor + 0xAu, 0u);
    if ((uint32_t)((actor - _obj_array) / OBJ_STRIDE) != _obj_entity)
        return; /* 1000:F0F7 */
    {
        uint8_t *slot8 = object_slot(8u); /* 0x3F05 */

        if (!slot8) return;
        wr_le_u16(slot8 + 0u, rd_le_u16(actor + 0u));
        wr_le_u16(slot8 + 2u, (uint16_t)(rd_le_u16(actor + 2u) - 0x1Au));
        wr_le_u16(slot8 + 6u, 0u);
        wr_le_u16(slot8 + 8u, 0x001Au);
        wr_le_u16(slot8 + 0xAu, 0x0605u);
        _obj_cmd_timer = 0x03E8u; /* 1000:F11C */
    }
}

/** --- the path handlers (indices 24 / 25 / 27: the type-15 rows) ----------
 *
 * These walk a record that `1000:9CF0` (the give/hold direction) creates: a
 * record whose **type is 0x0F** and whose `+0xE` links to another live record
 * (`found - 0x3E75 + 4`, the port's existing encoding).  So the "node" is a
 * placed record: `[+0]` its x, `[+2]` its y, `[+7]` its class byte, `[+8]` the
 * progress word, `[+0x10]` the map-derived value EBD0 writes.
 */
static uint8_t *_obj_link_record(uint32_t off)
{
    uint32_t idx;

    if (off < 4u) return NULL;
    idx = (off - 4u) / OBJ_STRIDE;
    return idx < OBJ_SLOTS ? _obj_array + idx * OBJ_STRIDE : NULL;
}

/** 1000:FC32: nudge the node's progress.  The *acting* record's own `+0xA`
 * accumulates the step (clamped at +/-8; a value about to pass +/-9 enters at
 * 0xFFF8 / 8) and the signed accumulator is added to the node's `+8`, unless the
 * node's `+0xA` word is non-zero (a "busy" gate) or the stored progress already
 * has the same sign and is 0xA away. */
static void _obj_nudge(uint8_t *actor, int32_t step)
{
    uint8_t *node = _obj_link_record(rd_le_u16(actor + 0xEu));
    uint32_t bx;
    int32_t cx;

    if (!node) return;
    if (rd_le_u16(node + 0xAu) != 0u) return; /* 1000:FC3C */
    bx = (uint16_t)(rd_le_u16(actor + 0xAu) + (uint16_t)step);
    if ((int16_t)bx < 0) {
        if (bx == (uint16_t)-9) bx = 0xFFF8u; /* 1000:FC45 */
    } else if (bx == 9u) {
        bx = 8u; /* 1000:FC4F */
    }
    wr_le_u16(actor + 0xAu, bx); /* 1000:FC57 */
    cx = (int16_t)rd_le_u16(node + 8u);
    if ((int16_t)bx < 0) {
        if (cx >= 0) {
            wr_le_u16(node + 8u, (uint16_t)(cx + (int16_t)bx));
            return;
        }
        if (-cx >= 0xA) return; /* 1000:FC69 */
    } else {
        if (cx < 0) {
            wr_le_u16(node + 8u, (uint16_t)(cx + (int16_t)bx));
            return;
        }
        if (cx >= 0xA) return; /* 1000:FC78 */
    }
    wr_le_u16(node + 8u, (uint16_t)(cx + (int16_t)bx)); /* 1000:FC7E */
}

/** 1000:EBA0 / 1000:EB78: step the node's progress by +1 / -1 until it reaches
 * +0xF / -0xF, where the acting record takes frame 0x4A and the node's class
 * byte is parked at 0xC (`& 7` = 4). */
static void _obj_cmd_path_step(uint8_t *actor, int32_t forward)
{
    uint8_t *node = _obj_link_record(rd_le_u16(actor + 0xEu));
    int32_t v;

    if (!node) return;
    if ((node[7] & 7u) == 4u) return; /* 1000:EBA6 / 0xEB7E */
    v = (int16_t)rd_le_u16(node + 8u);
    if (forward) {
        if (v >= 0 || -v < 0xF) {
            _obj_nudge(actor, 1);
            return;
        }
    } else {
        if (v < 0 || v < 0xF) {
            _obj_nudge(actor, -1);
            return;
        }
    }
    wr_le_u16(actor + 4u, 0x004Au); /* 1000:EBBa / 0xEB90 */
    node[7] = 0x0Cu;
}

/** 1000:EBD0: advance the node to the map under it.  It probes
 * (node.x + 8, node.y + 0x10), reads the item byte for the fine x (bit 7 picks
 * the 1/3 step, `word[node+8]`'s sign the other case), scales |progress| >> 3
 * through 0x6E3C (the 0x3B / 0x30 cells add 8 first), and - only while the
 * node's `+0xA` is 0 - stores the result at `+0xA` and the node's y at `+0x10`,
 * with effect 2. */
static void _obj_path_advance(uint8_t *actor)
{
    uint8_t *node = _obj_link_record(rd_le_u16(actor + 0xEu));
    uint32_t x, y, tile, fine, step;
    int32_t ax;

    if (!node) return;
    if (node[7] != 0u) return; /* 1000:EBD3 */
    x = (uint32_t)(rd_le_u16(node + 0u) + 8u);
    y = rd_le_u16(node + 2u);
    fine = x & 0xFu;
    tile = _obj_map_at_abs((int32_t)x, (int32_t)y + 0x10,
                           NULL); /* 1000:EBEF: the map cell */
    step = 3;
    if (level_slope_byte(tile, fine) & 0x80u) { /* 1000:EC27, field A */
        if (!(rd_le_u16(node + 8u) & 0x8000u)) step = 1;
    } else if (rd_le_u16(node + 8u) & 0x8000u) {
        step = 1;
    }
    ax = (int16_t)rd_le_u16(node + 8u); /* 1000:EC43 */
    if (ax < 0) {
        ax = -ax;
        if (tile == 0x3Bu) ax += 8; /* 1000:EC53 */
    } else if (tile == 0x30u) {
        ax += 8; /* 1000:EC5D */
    }
    ax >>= 3;
    step += (uint32_t)(int8_t)_obj_path_scale[(uint32_t)ax & 0x1Fu];
    (void)tile;
    if (rd_le_u16(node + 0xAu) != 0u) return; /* 1000:EC72 */
    wr_le_u16(node + 0xAu, (uint16_t)step); /* 1000:EC78 (DI = node+0xA) */
    wr_le_u16(node + 0x10u, (uint16_t)y); /* 1000:EC7A */
    _obj_driver_call(2); /* 1000:EC7E */
}

/** --- 1000:DC1F type15_tail_scan: the type-15 rows' tail -----------------
 *
 * Every type-15 command's row hands `0xDC1F` to the dispatcher as its tail, so
 * this is what runs after the path handlers.  It takes the record the actor is
 * linked to (`+0xE`, a placed record - the `1000:9CF0` node) as the probe
 * centre and looks for the first *other* placed record within
 * `|dy| <= 8` of `node.y + 0x10` and `|dx| <= 1` of `node.x + 8` whose type is
 * `<= 2`; when the node's progress (`+8`) is zero it skips that scan and goes
 * straight to the group pass instead.
 *
 * A match becomes the **carried form**: frame 0x55, type 0x10, `+0xA`/`+0xC`
 * cleared, `+0xE = 0xF0 + ([0x4103] & 0x3F)` (the animation phase), effect 8.
 * The player's own record takes the **give exit** instead (`0xDD3C`): the
 * actor's class slot is freed and re-framed at that class's placed record
 * (frame 0x103, type 0x1E, `+0xA` = 6, `+0xE` = 0xA).
 *
 * The group pass walks the eight `0x6F22` entries: an active entry within the
 * same window has its first member (`byte[+5]`) resolved to a placed record,
 * which is dissolved through `1000:FA13`; the original preserves that record
 * in BP and writes the carried form into it (`1000:DD2D PUSH DI / CALL 0xFA13
 * / POP BP`), not into the entry.
 */
static void _obj_carry_form(uint8_t *rec, uint32_t phase)
{
    wr_le_u16(rec + 4u, 0x0055u); /* 1000:DC9A */
    wr_le_u16(rec + 8u, 0x0010u);
    wr_le_u16(rec + 0xAu, 0u);
    wr_le_u16(rec + 0xCu, 0u);
    wr_le_u16(rec + 0xEu, (uint16_t)(0xF0u + phase)); /* 1000:DCB0 */
    _obj_driver_call(8); /* 1000:DCBF */
}

/** @brief 1000:DD3C: the type-15 carry give-back tail. */
static void _obj_cmd_type15_give(uint8_t *actor)
{
    uint32_t cls = actor[7];
    uint32_t slot;
    uint8_t *rec;

    actor[7] = 0u;
    if (!cls) return;
    _obj_class_byte_set(cls,
                        _obj_class_byte_at(cls) & 0x7Fu); /* 0x3E5F + cls - 1 */
    slot = 8u + cls; /* 0x3F17 + 0x12*(cls-1) */
    if (slot >= OBJ_SLOTS) return;
    rec = _obj_array + slot * OBJ_STRIDE;
    wr_le_u16(rec + 0xAu, 1u);
    wr_le_u16(rec + 0xEu, 0xFFFFu);
    rec[6] &= 0x7Fu; /* 1000:DD63 */
    wr_le_u16(actor + 0u, rd_le_u16(rec + 0u));
    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(rec + 2u) - 8u));
    wr_le_u16(actor + 4u, 0x0103u);
    wr_le_u16(actor + 8u, 0x001Eu);
    wr_le_u16(actor + 0xAu, 6u);
    wr_le_u16(actor + 0xCu, 0u);
    wr_le_u16(actor + 0xEu, 0x000Au);
}

/** @brief 1000:DC38 (type 15): the carry record's give-back branch. */
static void _obj_cmd_type15_tail(uint8_t *actor)
{
    uint8_t *node = _obj_link_record(rd_le_u16(actor + 0xEu));
    int32_t ax, bx;
    uint32_t phase;

    if (!node) return; /* 1000:DC1F (DI = the node) */
    ax = (int32_t)rd_le_u16(node + 0u) + 8; /* 1000:DC32 */
    bx = (int32_t)rd_le_u16(node + 2u) + 0x10; /* 1000:DC34 */
    phase = _obj_frame & 0x3Fu; /* 1000:DC3D -> [0x4230] */
    if (rd_le_u16(node + 8u) != 0u) { /* 1000:DC22 */
        for (uint32_t i = 0; i < 8u; i++) { /* 1000:DC3A: CX = 7 */
            uint8_t *r = object_slot(i);
            int32_t d;

            if (!r || r == actor) continue; /* 1000:DC4B */
            d = (int32_t)(int16_t)(uint16_t)(rd_le_u16(r + 2u) + 0x18u) - bx;
            if (d < 0) d = -d;
            if (d > 8) continue; /* 1000:DC72 */
            d = (int32_t)(int16_t)rd_le_u16(r + 0u) - ax;
            if (d < 0) d = -d;
            if (d > 1) continue; /* 1000:DC83 */
            if (rd_le_u16(r + 8u) > 2u) continue; /* 1000:DC8A */
            if ((int32_t)i == object_player()) { /* 1000:DC91 */
                _obj_cmd_type15_give(actor);
                return;
            }
            _obj_carry_form(r, phase); /* 1000:DC9A */
            return;
        }
    }
    for (uint32_t g = 0; g < 8u; g++) { /* 1000:DCD0 */
        uint8_t *e = (uint8_t *)_obj_register[g];
        int32_t d;

        if (rd_le_u16(e + 0u) & 0x8000u) continue; /* 1000:DCEC */
        d = (int32_t)(int16_t)(uint16_t)(rd_le_u16(e + 2u) + 0x18u) - bx;
        if (d < 0) d = -d;
        if (d > 8) continue;
        d = (int32_t)(int16_t)(uint16_t)(rd_le_u16(e + 0u) + 8u) - ax;
        if (d < 0) d = -d;
        if (d > 1) continue; /* 1000:DD0F */
        if ((uint32_t)e[5] == (uint32_t)object_player()) { /* 1000:DD27 */
            _obj_cmd_type15_give(actor);
            return;
        }
        {
            uint8_t *placed = object_slot(e[5]);

            if (!placed) continue;
            object_group_dissolve_at(g); /* 1000:DD2E */
            _obj_carry_form(placed, phase); /* 1000:DC9A with BP = the record */
        }
        return;
    }
    return;
}

/** 1000:EA0F / 1000:EA4D: the two "ride" commands.  Both require the record's
 * behaviour state to be 2 (1000:F2CF) and the acting entity not to be in a
 * stack (1000:EA33); they then set a frame (0xAB / 0xAD) with a three-frame
 * hold and hand 1000:FC81 the x offset and the bit argument. */
static void _obj_cmd_ride(uint8_t *actor, uint32_t frame, uint32_t xoff,
                          uint32_t bit)
{
    if (!_obj_state_is(actor, 2u)) return; /* 1000:EA15 */
    if (_obj_stack_masked()) return; /* 1000:EA1A */
    wr_le_u16(actor + 4u, (uint16_t)frame);
    wr_le_u16(actor + 0xAu, 3u);
    _obj_sel_type = bit; /* 1000:EA29 / 1000:EA67 */
    _obj_scan_point(actor, xoff); /* 1000:FC81 */
}

/** 1000:ED4F / 1000:ED69 / 1000:ED83 / 1000:ED9D: the four one-step animation
 * nudges.  Each waits on the 25-frame cadence (1000:F9E4) and then moves the
 * frame one step toward its bound unless it is already there.  With [0x6F1D]
 * set, the handler is the enter-the-building sequence (1000:EE3B) instead. */
static void _obj_cmd_frame_step(uint8_t *actor, uint32_t bound, int32_t up)
{
    uint32_t f;

    if (_obj_lat) {
        _obj_cmd_enter(actor);
        return;
    } /* 1000:ED56 */
    if (_obj_frame_cadence(actor)) return; /* 1000:ED5C */
    f = rd_le_u16(actor + 4u);
    if (f == bound) return;
    wr_le_u16(actor + 4u, (uint16_t)(up ? f + 1u : f - 1u));
}

/** 1000:EDB7 (and its thunks 0xF0C3 / 0xF0C6): put the record into one of its
 * two home states.  Types 0x12/0x17/0x1F become frame 0, type 0; types
 * 0x14/0x18/0x20 become frame 8, type 1; anything else is left alone.  Either
 * way the pending words are cleared, the latch is released and [0x13A4] is set
 * to 0xFF (the ACTION press stays consumed). */
static int32_t _obj_cmd_home_state(uint8_t *actor)
{
    uint32_t t = rd_le_u16(actor + 8u);

    if (t == 0x12u || t == 0x17u || t == 0x1Fu) {
        wr_le_u16(actor + 4u, 0u); /* 1000:EDD9 */
        wr_le_u16(actor + 8u, 0u);
    } else if (t == 0x14u || t == 0x18u || t == 0x20u) {
        wr_le_u16(actor + 4u, 8u); /* 1000:EDE5 */
        wr_le_u16(actor + 8u, 1u);
    } else {
        return 0; /* 1000:EDD8 */
    }
    wr_le_u16(actor + 0xAu, 0u); /* 1000:EDEF */
    wr_le_u16(actor + 0xCu, 0u);
    wr_le_u16(actor + 0xEu, 0u);
    wr_le_u16(actor + 0x10u, 0u);
    _obj_lat = 0; /* 1000:EDFD */
    object_set_action_latched(1); /* 1000:EE01 */
    return 1; /* AX = 0: converted */
}

/** 1000:EE07 (and 0xEFD1 / 0xEFD4, its jump-table aliases): advance the record's
 * direction pointer while the ACTION press is not being consumed.  The first
 * pass latches [0x6F1D] and clears the pointer; `+0xE` then walks 0..0x1FF in
 * steps of four (a 128-entry circle of four-word vectors).  `[0x6916]` becomes 4
 * unless the previous dispatch code was already 4 (the debounce byte 0x197A),
 * and 0x1987 is armed. */
static void _obj_cmd_turn(uint8_t *actor)
{
    if (object_action_latched()) return; /* 1000:EE0C */
    if (!_obj_lat) {
        _obj_lat = 0xFF; /* 1000:EE16 */
        wr_le_u16(actor + 0xEu, 0u);
    }
    wr_le_u16(actor + 0xEu,
              (uint16_t)((rd_le_u16(actor + 0xEu) + 4u) & 0x1FFu));
    if (_obj_debounce != 4u) _obj_out_state = 4; /* 1000:EE30 */
    _obj_charge_arm = 0xFF; /* 1000:EE35 */
}

/** 1000:EFE4: the *leave* sequence - what 1000:EFD7 jumps to when the latch is
 * set (the enter sequence's counterpart, reached from the type-23/24 rows'
 * idle command).  It releases the latch, derives the animation index from the
 * frozen direction counter (`+0xE`, folded at 0x100 and divided by 16), sets a
 * three-frame hold, bumps the type by 2 and the frame by 1, nudges the record
 * 4 px (right and down for frame 0xBB, else left) and runs the fall machine
 * with the (0/0 | 8/1) landing pair.  When the record did not land (`+0x10`
 * still set) the nudged position stays; otherwise the position is restored and
 * the landing pair is written back. */
static void _obj_cmd_leave(uint8_t *actor)
{
    uint32_t cx = rd_le_u16(actor + 0xEu);
    int32_t ax = 4, bx = 2;
    int32_t down = 1;

    _obj_lat = 0; /* 1000:EFE4 */
    if (rd_le_u16(actor + 4u) != 0x00BBu) { /* 1000:EFF5 */
        ax = -4;
        bx = -2;
        down = 0;
    }
    wr_le_u16(actor + 0x10u, (uint16_t)(int16_t)bx); /* 1000:F005 */
    wr_le_u16(actor + 0xEu, 0u);
    if (cx >= 0x100u) cx = 0x1FFu - cx; /* 1000:F00D */
    cx >>= 4; /* 1000:F019 */
    wr_le_u16(actor + 0xCu, (uint16_t)(0xFu - cx)); /* 1000:F021 */
    wr_le_u16(actor + 0xAu, 3u); /* 1000:F029 */
    wr_le_u16(actor + 8u,
              (uint16_t)(rd_le_u16(actor + 8u) + 2u)); /* 1000:F02E */
    wr_le_u16(actor + 4u,
              (uint16_t)(rd_le_u16(actor + 4u) + 1u)); /* 1000:F032 */
    {
        uint16_t sx = rd_le_u16(actor + 0u), sy = rd_le_u16(actor + 2u);

        wr_le_u16(actor + 0u, (uint16_t)(sx + (uint16_t)(int16_t)ax));
        wr_le_u16(actor + 2u, (uint16_t)(sy - 4u)); /* 1000:F03C */
        _obj_driver_call(2); /* 1000:F041 */
        _obj_cmd_fall(actor, down ? 0 : 8, down ? 0 : 1, down ? 0x10u : 0u);
        if (rd_le_u16(actor + 0x10u) == 0u) { /* 1000:F04A */
            wr_le_u16(actor + 0u, sx); /* 1000:F051: restore */
            wr_le_u16(actor + 2u, sy);
            wr_le_u16(actor + 4u, (uint16_t)_obj_side); /* 1000:F056: frame  */
            wr_le_u16(actor + 8u, _obj_sel_xoff); /* 1000:F05C: type   */
        }
    }
}

/** 1000:EFD7: release the record's direction pointer while the latch is clear
 * (otherwise the enter sequence runs). */
static void _obj_cmd_turn_idle(uint8_t *actor)
{
    if (_obj_lat) {
        _obj_cmd_leave(actor);
        return;
    } /* 1000:EFDC -> 0xEFE4 */
    wr_le_u16(actor + 0xEu, 0u);
}

/** 1000:F7C3/F83E: `byte[0x6F0B + (type - 0x19)]` = `byte[0x6EF2 + type]`.
 * Types 0x19/0x1A select bytes 0x6F0B/0x6F0C, which `level_setup_screen`
 * (1000:4E99) overwrites with the level's field-K second word; the rest of the
 * window is the image's static bytes. */
static uint32_t _obj_type_tile_byte(uint32_t type)
{
    if (type == 0x19u) return level_special_tile(2u); /* [0x6F0B] */
    if (type == 0x1Au) return level_special_tile(3u); /* [0x6F0C] */
    return _obj_type_tile[type < 35u ? type : 0u];
}

/** @brief 1000:F8AD: the fall machine's hard landing. */
static void _obj_fall_hard_landing(uint8_t *actor)
{
    wr_le_u16(actor + 0u, (uint16_t)(rd_le_u16(actor + 0u) - 8u));
    wr_le_u16(actor + 4u, 0x0028u);
    wr_le_u16(actor + 8u, 0x0016u);
    wr_le_u16(actor + 0xAu, 5u);
    _obj_driver_call(4);
}

/** @brief 1000:F975: the fall machine's landing at y. */
static void _obj_fall_land_at_y(uint8_t *actor, int32_t bx)
{
    wr_le_u16(actor + 2u, (uint16_t)bx);
    wr_le_u16(actor + 0x10u, 0u);
    if (rd_le_u16(actor + 0xCu) < 0x10u) wr_le_u16(actor + 0xCu, 0x0010u);
}

/** @brief 1000:F8D2: the fall machine's land pair. */
static void _obj_fall_land_pair(uint8_t *actor)
{
    wr_le_u16(actor + 4u, (uint16_t)_obj_side); /* frame <- [0x4230] */
    wr_le_u16(actor + 8u, _obj_sel_xoff); /* type  <- [0x4234] */
    wr_le_u16(actor + 0xAu, 0u);
    _obj_driver_call(5);
}

/** @brief 1000:F966: the fall machine's placement. */
static void _obj_fall_place(uint8_t *actor, int32_t ax, int32_t bx)
{
    wr_le_u16(actor + 0u, (uint16_t)ax);
    wr_le_u16(actor + 2u, (uint16_t)bx);
    if (bx >= 0x140) _obj_fall_hard_landing(actor);
}

/** @brief 1000:F8EC: the fall machine's item-word probe. */
static void _obj_fall_item_word(uint8_t *actor, int32_t ax, int32_t bx)
{
    int32_t px = ax + (int32_t)(int16_t)_obj_sel_frame;
    int32_t py = bx + 0x18;
    uint32_t tile = _obj_map_at_abs(px, py, NULL);
    uint32_t item = level_item_word(tile, (uint32_t)((uint32_t)py & 0xFu));
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
    for (uint32_t i = 0; i < ((uint32_t)px & 0xFu); i++) { /* 1000:F952 */
        uint32_t carry = lo & 1u;

        lo >>= 1;
        hi = (hi >> 1) | (carry << 15);
    }
    mask = hi;
    if ((_obj_sel_type & mask) != 0u) { /* 1000:F964 -> F975 */
        _obj_fall_land_at_y(actor, bx);
        return;
    }
    _obj_fall_place(actor, ax, bx);
}

/** @brief 1000:F8CB: the fall machine. */
static void _obj_fall_machine(uint8_t *actor)
{
    int32_t ax, bx, col, d;
    uint32_t row, fine, page;
    uint32_t probeY, above, below;
    uint32_t anim;
    int32_t vy;

    if (!_obj_settle_gate(actor)) { /* 1000:F6F6 */
        /* 1000:F6FB/F701: frame <- [0x4230] (_obj_side), type <- [0x4234]
         * (_obj_sel_xoff).  The third word [0x4238] is the item probe's x
         * offset, not the type. */
        wr_le_u16(actor + 4u, (uint16_t)_obj_side);
        wr_le_u16(actor + 8u, _obj_sel_xoff);
        wr_le_u16(actor + 0xAu, 0u);
        return;
    }
    if (rd_le_u16(actor + 0xAu) == 1u) /* 1000:F70D DEC / JNZ */
        wr_le_u16(actor + 4u, (uint16_t)(rd_le_u16(actor + 4u) + 1u));
    wr_le_u16(actor + 0xAu, (uint16_t)(rd_le_u16(actor + 0xAu) - 1u));

    anim = rd_le_u16(actor + 0xCu); /* 1000:F715 */
    /* 1000:F71B is `CMP AX,0x18 / JZ`: only the exact 0x18 takes the flat 3.
     * The counter stops there (1000:F727 increments to 0x18 at most), so `>=`
     * is equivalent for every reachable value; a crafted save can store more,
     * where the original reads past the 0x6EF3 table (D-101). */
    if (anim >= 0x18u) {
        vy = 3; /* 1000:F71B */
    } else {
        /* 1000:F720-0xF727: `CBW` sign-extends the table byte, so the
         * first steps of a full charge are NEGATIVE (the leap rises). */
        vy = _obj_fall_step[anim];
        anim++;
    }
    wr_le_u16(actor + 0xCu, (uint16_t)anim); /* 1000:F728 */
    wr_le_u16(actor + 0xEu,
              (uint16_t)(rd_le_u16(actor + 0xEu) + (uint16_t)(int16_t)vy));

    ax = (int32_t)(int16_t)rd_le_u16(actor + 0u) +
         (int32_t)(int16_t)rd_le_u16(actor + 0x10u);
    bx = (int32_t)(int16_t)rd_le_u16(actor + 2u) + vy;
    col = ax + 8; /* 1000:F736/F73C: the probe x (DX = y, CX keeps the row) */
    probeY = (uint32_t)(bx + 0x18); /* 1000:F739 */
    fine = (uint32_t)col & 0xFu; /* 1000:F753 */
    row = probeY & ~0xFu; /* 1000:F758: CX is the row word */

    above = _obj_map_at_abs(col, (int32_t)row - 0x10,
                            NULL); /* 1000:F761: [DI-0x20] */
    if (above >= 0xF0u) { /* 1000:F766 */
        /* 1000:F772: AX is the record.x + [+0x10] captured at 1000:F736, not
         * the probe x in CX (`ADD AX,8` then `AND AX,0xFFF0`). */
        uint32_t gx = (uint32_t)(ax + 8) & ~0xFu;

        if (above != 0xF1u) {
            gx -= 4u; /* 1000:F778 */
            if (above != 0xF2u) gx += 8u;
            wr_le_u16(actor + 0u, (uint16_t)gx); /* 1000:F784 */
        }
        wr_le_u16(actor + 2u, (uint16_t)(row - 0x1Cu)); /* 1000:F786 */
        wr_le_u16(actor + 4u, 0x00CDu);
        wr_le_u16(actor + 8u, 0x001Cu);
        wr_le_u16(actor + 0xCu, 0u);
        wr_le_u16(actor + 0xEu, 0u);
        wr_le_u16(actor + 0xAu, 9u);
        _obj_driver_call(6); /* 1000:F7A6 */
        return;
    }
    if (bx >= 0x128) { /* 1000:F7AE */
        _obj_fall_place(actor, ax, bx);
        return;
    }
    below =
        _obj_map_at_abs(col, (int32_t)probeY, NULL); /* 1000:F7B9: the feet */
    page = level_page_byte(below, fine); /* 1000:F7F3 */
    if (rd_le_u16(actor + 0x10u) != 0u) { /* 1000:F7BB */
        uint32_t want = _obj_type_tile_byte(rd_le_u16(actor + 8u));

        if (below == want) { /* 1000:F7CD */
            /* 1000:F7D7: `[DI+0x20]` is the cell one row below the feet, in
             * the same column, through the same `BP` (0x92A0 + fine). */
            uint32_t b2 = _obj_map_at_abs(col, (int32_t)row + 0x10, NULL);

            if (level_page_byte(b2, fine) == 0u) { /* 1000:F7EE */
                _obj_fall_land_at_y(actor, bx);
                return;
            }
        }
    }
    if (page == 0u) { /* 1000:F80A: the row above decides */
        uint32_t a2 = _obj_map_at_abs(col, (int32_t)row - 0x10, NULL);
        uint32_t want = _obj_type_tile_byte(rd_le_u16(actor + 8u));
        /* 1000:F83A/F84C park these two in [0x422C]/[0x422E] only to compare
         * them; the port compares the cells directly. */
        if (want == a2) { /* 1000:F856 */
            _obj_fall_land_at_y(actor, bx);
            return;
        }
        if (!(level_page_byte(a2, fine) & 0x80u)) {
            row -= 0x10; /* 1000:F86F: the row word, for the snap's `d` */
            page = level_page_byte(a2, fine);
        }
    } else if (page & 0x80u) { /* 1000:F80C JNS */
        row += 0x10; /* 1000:F80E: `ADD CX,0x10` moves the ROW, not the probe
                      * x; the cell read at 1000:F813 is `[DI+0x20]` - the
                      * same column, one row down. */
        page = level_page_byte(_obj_map_at_abs(col, (int32_t)row, NULL), fine);
        if (page & 0x80u) { /* 1000:F82F -> F8EC */
            _obj_fall_item_word(actor, ax, bx);
            return;
        }
    }
    /* 1000:F87F: the snap */
    _obj_sel_type = (uint16_t)(int16_t)(int8_t)(uint8_t)page;
    d = (int32_t)(row + (uint32_t)(int16_t)(int8_t)(uint8_t)page) - 0x18 - bx;
    if (d >= 0) { /* 1000:F891 */
        _obj_fall_item_word(actor, ax, bx);
        return;
    }
    d = -d;
    if (d > 3) {
        _obj_fall_item_word(actor, ax, bx);
        return;
    }
    bx -= d; /* 1000:F89A */
    wr_le_u16(actor + 0u, (uint16_t)ax);
    wr_le_u16(actor + 2u, (uint16_t)bx);
    {
        int32_t speed = (int16_t)rd_le_u16(actor + 0xEu);

        if (speed < 0) { /* 1000:F8A6 */
            _obj_fall_land_pair(actor);
            return;
        }
        if (speed >= 0x30) { /* 1000:F8AB */
            _obj_fall_hard_landing(actor);
            return;
        }
        level_set_offset_word((uint16_t)((uint32_t)speed >> 3)); /* 1000:F8CE */
    }
    _obj_fall_land_pair(actor);
}

/** 1000:F067 / 0xF078: the two fall-machine entries.  The first lands with
 * frame 0 / type 0 and a 0x10 x offset, the second with frame 8 / type 1. */
static void _obj_cmd_fall(uint8_t *actor, int32_t x, int32_t y, uint32_t frame)
{
    _obj_side = (uint16_t)(int16_t)x;
    _obj_sel_xoff = (uint16_t)y;
    _obj_sel_frame = frame;
    _obj_fall_machine(actor);
}

/** 1000:F08D / 1000:F0A2: the two landing placements - a frame/type pair and a
 * scratch point, then 1000:F23A. */
static void _obj_cmd_place(uint8_t *actor, int32_t x, int32_t y, uint32_t frame)
{
    _obj_side = (uint16_t)(int16_t)x; /* 1000:F08D */
    _obj_sel_xoff = (uint16_t)y;
    _obj_sel_frame = frame;
    _obj_place_at(actor); /* 1000:F23A */
}

/** 1000:F0B3: the "stand" form - frame 0x10, type 2, a one-frame hold. */
static void _obj_cmd_stand(uint8_t *actor)
{
    wr_le_u16(actor + 4u, 0x0010u);
    wr_le_u16(actor + 8u, 0x0002u);
    wr_le_u16(actor + 0xAu, 0x0001u);
}

/** 1000:E7C0 stream_script_bytes_timed.  The dispatch's one-shot [0x6DEA]
 * gates it; when the hold 0x6DEC expires, fetch a frame/hold pair, set the
 * actor's +4 frame and the next hold, and wrap at 0xFF. */
static void _obj_cmd_stream_timed(uint8_t *actor)
{
    const uint8_t *p;

    if (_obj_cmd_timer != 0u) return;
    if (_obj_stream_count != 0u) {
        _obj_stream_count--;
        return;
    }
    _obj_stream_count = 0u;
    p = _obj_stream_ptr;
    wr_le_u16(actor + 4u, p[0]); /* MOV [SI+4], AX */
    _obj_stream_count = p[1]; /* [0x6DEC] */
    p += 2;
    if (*p == 0xFFu) p = _obj_anim_stream; /* 1000:E7FA */
    _obj_stream_ptr = p; /* [0x6DEE] */
}

/** 1000:EBCA: type 15 command 8 clears the record's +0xA hold. */
static void _obj_cmd_clear_countdown(uint8_t *actor)
{ wr_le_u16(actor + 0xAu, 0u); }

/** Test seams for the two handler bodies (the row selector's one-shot gate
 * keeps 1000:E7C0 from being driven directly in the unit test). */
void object_stream_command(uint8_t *actor) { _obj_cmd_stream_timed(actor); }
/** @brief Test seam: clear the record's hold. */
void object_clear_countdown(uint8_t *actor) { _obj_cmd_clear_countdown(actor); }
/** @brief Test seam: set the command timer. */
void object_set_command_timer(uint32_t v) { _obj_cmd_timer = (uint16_t)v; }

/** @brief Dispatch one command handler address. */
static int32_t _obj_cmd_call(uint8_t *actor, uint32_t addr)
{
    switch (addr) {
    case 0xD9C7u: return 1; /* the common tail: a bare RET */
    case 0xE4F8u: (void)object_step_down(actor); return 1;
    case 0xE609u: (void)object_step_up(actor); return 1;
    case 0xE72Au: _obj_cmd_idle_a(actor); return 1;
    case 0xE790u: _obj_cmd_idle_b(actor); return 1;
    case 0xE860u: (void)object_stack_step_off(actor); return 1;
    case 0xE8DDu: (void)object_stack_leave(actor); return 1;
    case 0xE963u: (void)object_stack_snap(actor); return 1;
    case 0xE99Du: (void)object_stack_stand(actor); return 1;
    case 0xE93Du: (void)object_selector_walk(actor, 0); return 1;
    case 0xE952u: (void)object_selector_walk(actor, 1); return 1;
    /* the rest of the rows' handlers (see the block above) */
    case 0xE6FBu: _obj_walk(actor, 1); return 1;
    case 0xE764u: _obj_walk(actor, 0); return 1;
    case 0xEB78u: _obj_cmd_path_step(actor, 0); return 1;
    case 0xEBA0u: _obj_cmd_path_step(actor, 1); return 1;
    case 0xEBD0u: _obj_path_advance(actor); return 1;
    case 0xDC1Fu:
        _obj_cmd_type15_tail(actor);
        return 1; /* the type-15 rows' tail */
    /* the rows' tails (the +0xC word): 0xD9C7 is the bare RET above */
    case 0xD9C8u: _obj_tail_frame_bound(actor, 0x112u); return 1;
    case 0xD9CDu: _obj_tail_frame_bound(actor, 0x116u); return 1;
    case 0xD9E8u: _obj_tail_type3(actor); return 1;
    case 0xDB48u: _obj_tail_span_owner(actor, 0u, 0u, 0xABu); return 1;
    case 0xDB61u: _obj_tail_span_owner(actor, 8u, 1u, 0xADu); return 1;
    case 0xDD93u: _obj_tail_type16(actor); return 1;
    case 0xDDDAu: _obj_tail_type29(actor); return 1;
    case 0xE802u: _obj_tail_special_tile(actor, 4u, 0x16, 0); return 1;
    case 0xE831u: _obj_tail_special_tile(actor, 5u, 0x18, 1); return 1;
    case 0xEAACu: _obj_tail_type11(actor); return 1;
    case 0xEAE6u: _obj_tail_type12(actor); return 1;
    case 0xEB5Cu: _obj_tail_walk_countdown(actor, 1); return 1;
    case 0xEB6Au: _obj_tail_walk_countdown(actor, 0); return 1;
    case 0xECB7u: _obj_tail_type17(actor); return 1;
    case 0xEF0Au: return 1; /* a bare RET */
    case 0xEF0Bu: return 1; /* a bare RET */
    case 0xF067u: _obj_cmd_fall(actor, 0, 0, 0x10u); return 1;
    case 0xF078u: _obj_cmd_fall(actor, 8, 1, 0u); return 1;
    case 0xF0C9u: _obj_tail_type33(actor); return 1;
    case 0xDBCDu: _obj_tail_type28(actor); return 1;
    case 0xDE7Eu: _obj_tail_type30(actor); return 1;
    case 0xDED1u: _obj_tail_type34(actor); return 1;
    case 0xEF66u: _obj_tail_type22(actor); return 1;
    case 0xEA0Fu: _obj_cmd_ride(actor, 0xABu, 0xCu, 9u); return 1;
    case 0xEA4Du: _obj_cmd_ride(actor, 0xADu, 2u, 0xAu); return 1;
    case 0xEC99u:
        _obj_walk(actor, 0);
        _obj_neighbour_pass(actor, 0xDu, 0xCu, 0xA);
        return 1;
    case 0xECA8u:
        _obj_walk(actor, 1);
        _obj_neighbour_pass(actor, 3u, 0xCu, -0xA);
        return 1;
    case 0xED4Fu: _obj_cmd_frame_step(actor, 0x8Eu, 1); return 1;
    case 0xED69u: _obj_cmd_frame_step(actor, 0x8Cu, 0); return 1;
    case 0xED83u: _obj_cmd_frame_step(actor, 0x91u, 0); return 1;
    case 0xED9Du: _obj_cmd_frame_step(actor, 0x93u, 1); return 1;
    case 0xEDB7u: _obj_cmd_home_state(actor); return 1;
    case 0xEE07u: _obj_cmd_turn(actor); return 1;
    case 0xEE3Bu: _obj_cmd_enter(actor); return 1;
    case 0xEFD1u: _obj_cmd_turn(actor); return 1;
    case 0xEFD4u: _obj_cmd_turn(actor); return 1;
    case 0xEFD7u: _obj_cmd_turn_idle(actor); return 1;
    case 0xF08Du: _obj_cmd_place(actor, -8, 8, 1u); return 1;
    case 0xF0A2u: _obj_cmd_place(actor, 0x18, 0, 0u); return 1;
    case 0xF0B3u: _obj_cmd_stand(actor); return 1;
    case 0xF0C3u: _obj_cmd_home_state(actor); return 1;
    case 0xF0C6u: _obj_cmd_home_state(actor); return 1;
    case 0xE7C0u: _obj_cmd_stream_timed(actor); return 1;
    case 0xEBCAu: _obj_cmd_clear_countdown(actor); return 1;
    case 0xEA0Eu: return 1; /* bare RET */
    default: return 0;
    }
}

/** 1000:E4B0..E4F7: run one command for the acting record.  The type row's
 * handler runs first, and when it changes the record's type the tail is skipped
 * (1000:E4CC); otherwise the row's +0xA hook (1000:D8BD, the terrain settle)
 * and then the tail handler run.
 *
 * This is the ACTING record's path.  Every other static record runs its row
 * from `object_row_pass` (`collision_scan` `1000:D860`, the last call of
 * `update_world`), which the port runs after `object_world_tick`. */
int32_t object_command_run(uint8_t *actor, uint8_t input)
{
    ObjCommand c = object_command_select(actor, input);
    uint32_t before = rd_le_u16(actor + 8u);
    int32_t ran = OBJ_CMD_NONE;
    int32_t skip = 0;

    if (c.handler) {
        if (!_obj_cmd_call(actor, c.handler)) return OBJ_CMD_UNPORTED;
        ran = OBJ_CMD_HANDLER;
        if (rd_le_u16(actor + 8u) != before) skip = 1; /* 1000:E4CC */
    }
    if (!skip) {
        if (c.hook) _obj_cmd_settle(actor); /* 1000:D8BD */
        if (c.tail) {
            if (!_obj_cmd_call(actor, c.tail)) return OBJ_CMD_UNPORTED;
            ran = OBJ_CMD_TAIL;
        }
    }
    if (_obj_out_state != 0u) { /* 1000:E4E4 */
        _obj_debounce =
            (uint8_t)_obj_pending; /* [0x197A] := the old [0x1979] */
        _obj_pending = _obj_out_state;
        _obj_cursor = _obj_out_state_hi;
    }
    return ran;
}

/** 1000:D860 collision_scan: update_world's last call (1000:80D5), the
 * per-frame row pass over the eight static records.  The acting record is
 * skipped (it ran its row through 1000:E4B0 already); every other record whose
 * x is not 0xFFFF runs its type row's +0xA settle hook (1000:D8BD) and then the
 * row's +0xC tail - the *first* half of the row, with no input-bit-7
 * selection.  `CALL word ptr [0x6845]` is unconditional, but every shipped
 * row's tail is a real handler (the common one is the bare RET 0xD9C7). */
void object_row_pass(void)
{
    uint8_t *actor = _obj_actor();

    for (uint32_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        const uint8_t *row;
        uint32_t type, tail;

        if (r == actor) continue; /* 1000:D86D */
        if (rd_le_u16(r + 0u) == 0xFFFFu) continue; /* 1000:D873 */
        type = rd_le_u16(r + 8u);
        if (type >= 35u) continue; /* the original reads past 0x698A */
        row = _obj_cmd_rows[type];
        _obj_row_ofs = 32u * type; /* 1000:D894: DI = the row */
        if (row[0xAu]) _obj_cmd_settle(r); /* 1000:D89F */
        tail = rd_le_u16(row + 0xCu);
        if (tail) (void)_obj_cmd_call(r, tail); /* 1000:D8A8 */
    }
}

/** 1000:E4F8 (handler 1): the **step-down** state.  The cell at the feet
 * (`x + 8`, `y + 0x16`) decides: a special tile (field K -> `[0x6F20]`/`[0x6F21]`)
 * snaps the actor to the tile grid, a `0x6F92` span at that column takes it
 * onto the span, else the grab (1000:E58B) runs.  Returns 1 tile rule,
 * 2 span, 3 grabbed, 4 registered, -1 nothing. */
int32_t object_step_down(uint8_t *actor)
{
    int32_t x = (int32_t)(int16_t)rd_le_u16(actor + 0u);
    int32_t y = (int32_t)(int16_t)rd_le_u16(actor + 2u);
    uint32_t sub, cell = _obj_map_at_abs(x + 8, y + 0x16, &sub);

    if (cell == level_special_tile(0u) || cell == level_special_tile(1u)) {
        wr_le_u16(actor + 0u, (uint16_t)((x + 8) & 0xFFF0));
        wr_le_u16(actor + 2u, (uint16_t)(y - 1));
        wr_le_u16(actor + 4u, 0x0023u);
        wr_le_u16(actor + 8u, 0x0004u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        return 1;
    }
    for (uint32_t i = 0; i < 2u; i++) {
        uint16_t *s = _obj_span[i];
        int32_t sx = (int16_t)s[1], sy = (int16_t)s[2], sh = (int16_t)s[3], d;

        if (rd_le_u16((uint8_t *)s + 0u) & 0x8000u) continue;
        d = sx - (x + 8);
        if (d < 0) d = -d;
        if (d >= 8) continue; /* |span.x - (x+8)| < 8 */
        d = sy - y;
        if (d >= 0 && d >= 0x18) continue; /* span.y - y < 0x18 */
        if (sy + sh < y) continue; /* the span covers y */
        wr_le_u16(actor + 2u, (uint16_t)(y - 1));
        wr_le_u16(actor + 0u, (uint16_t)(sx - 8));
        wr_le_u16(actor + 4u, 0x0052u);
        wr_le_u16(actor + 8u, 0x000Bu);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        wr_le_u16(actor + 0xEu,
                  (uint16_t)((uintptr_t)s - (uintptr_t)_obj_span));
        _obj_stack_unmark(actor); /* 1000:F3CA */
        return 2;
    }
    {
        int32_t r = object_stack_grab(actor);

        return r == 1 ? 3 : (r == 0 ? 4 : -1);
    }
}

/** 1000:E609 (handler 2): the **walk-up** state.  Same three-way split, with
 * `x + 8, y + 0x19` as the feet cell; the span test uses `y + 0x18` as the line
 * and a 0x10-px column window, and the fall-through is the push (1000:E697).
 * Returns 1 tile rule, 2 span, 3 pushed, 4 nothing. */
int32_t object_step_up(uint8_t *actor)
{
    int32_t x = (int32_t)(int16_t)rd_le_u16(actor + 0u);
    int32_t y = (int32_t)(int16_t)rd_le_u16(actor + 2u);
    uint32_t sub, cell = _obj_map_at_abs(x + 8, y + 0x19, &sub);

    if (cell == level_special_tile(0u) || cell == level_special_tile(1u)) {
        wr_le_u16(actor + 0u, (uint16_t)((x + 8) & 0xFFF0));
        wr_le_u16(actor + 2u, (uint16_t)(y + 1));
        wr_le_u16(actor + 4u, 0x0025u);
        wr_le_u16(actor + 8u, 0x0005u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        return 1;
    }
    for (uint32_t i = 0; i < 2u; i++) {
        uint16_t *s = _obj_span[i];
        int32_t sx = (int16_t)s[1], sy = (int16_t)s[2], sh = (int16_t)s[3], d;
        int32_t line = y + 0x18;

        if (rd_le_u16((uint8_t *)s + 0u) & 0x8000u) continue;
        d = sx - (x + 8);
        if (d < 0) d = -d;
        if (d >= 0x10) continue; /* |span.x - (x+8)| < 0x10 */
        if (sy >= line) continue;
        if (sy + sh < line) continue;
        wr_le_u16(actor + 2u, (uint16_t)(y + 1));
        wr_le_u16(actor + 0u, (uint16_t)(sx - 8));
        wr_le_u16(actor + 4u, 0x0054u);
        wr_le_u16(actor + 8u, 0x000Cu);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        wr_le_u16(actor + 0xEu,
                  (uint16_t)((uintptr_t)s - (uintptr_t)_obj_span));
        _obj_stack_unmark(actor);
        return 2;
    }
    return object_stack_push(actor) ? 3 : 4;
}

/** 1000:E93D / 1000:E952: the two wrappers that set the selector's constants and
 * jump into it - left: xoff -1, frame 8, type 1; right: xoff 1, frame 0, type 0. */
int32_t object_selector_walk(uint8_t *actor, int32_t right)
{
    return right ? object_selector(actor, 1u, 0u, 0u)
                 : object_selector(actor, 0xFFFFu, 8u, 1u);
}

/** 1000:F17C script_op_select_on_zero: the object selector, and the stack's
 * "walk off" path.  `xoff` = [0x4234], `frame` = [0x4238], `type` = [0x423C] -
 * the two wrappers 1000:E93D (8/1) and 1000:E952 (0/0) set them and jump here.
 * Returns 0 = done, -1 = nothing found (the original's `JS`); the terrain
 * branch (`1000:F1D7`'s probe, `1000:F223`'s special-tile test) is in. */
int32_t object_selector(uint8_t *actor, uint16_t xoff, uint16_t frame,
                        uint16_t type)
{
    uint16_t ax;
    uint8_t *below, *entry;
    int32_t r;

    _obj_sel_xoff = xoff;
    _obj_sel_frame = frame;
    _obj_sel_type = type;
    ax = (uint16_t)(_obj_sel_xoff + rd_le_u16(actor + 0u)); /* 1000:F185 */
    r = object_stack_pop(ax, &below, &entry);
    if (r < 0) return -1; /* 1000:F18D */
    if (r == 0) {
        if (rd_le_u16(actor + 4u) != 0x20u) { /* 1000:F1F2 */
            int32_t d = (int32_t)(int16_t)rd_le_u16(actor + 4u) -
                        (int32_t)(int16_t)_obj_sel_frame;

            if (d < 0 || d >= 8)
                wr_le_u16(actor + 4u, _obj_sel_frame); /* 1000:F20C */
            _obj_walk(actor, _obj_sel_type != 0u); /* 1000:E6FB / 0xE764 */
            return 0;
        }
        wr_le_u16(actor + 4u, _obj_sel_frame); /* 1000:F191 */
        return 0;
    }
    if (below) { /* 1000:F198 */
        wr_le_u16(below + 2u, (uint16_t)(rd_le_u16(below + 2u) - 8u));
        wr_le_u16(below + 4u, 0x0020u);
    }
    wr_le_u16(actor + 4u, frame);
    wr_le_u16(actor + 8u, type);
    /* 1000:F1B2 also writes [0x6FE6] = 0xFF; that byte has no decoded reader. */
    {
        uint32_t bit =
            _obj_index_bit((uint32_t)(actor - _obj_array) / OBJ_STRIDE);

        if ((_obj_stack_mask[0] | _obj_stack_mask[1]) & bit)
            return 0; /* 1000:F1CB */
    }
    /* 1000:F1D7: the step probe's distance test.  A blocked probe, or a step
     * of 10 px or more, goes to 1000:F223; a small step does nothing. */
    {
        uint32_t cell = 0xFFu;
        int32_t bx = _obj_step_probe(actor, &cell), d;

        if (bx >= 0) {
            d = bx - 0x18 - (int32_t)(int16_t)rd_le_u16(actor + 2u);
            if (d < 0) d = -d;
            if (d - 0xA < 0) return 0; /* 1000:F1F1 */
        }
        /* 1000:F223: DI is the cell 1000:E389 ended on; when it is one of the
         * level's two special tiles (field K -> [0x6F20]/[0x6F21]) the actor
         * stays put, otherwise it takes the found entry's x. */
        if (cell == level_special_tile(0u) || cell == level_special_tile(1u))
            return 0;
        if (entry) wr_le_u16(actor + 0u, rd_le_u16(entry + 0u));
        return 0;
    }
}

/** 1000:FADA: the per-member restore inside a dissolve. */
static void _obj_group_restore_member(uint8_t *rec)
{
    wr_le_u16(rec + 4u, 0x0010u); /* frame 0x10 */
    wr_le_u16(rec + 8u, 0x0003u); /* type 3     */
    wr_le_u16(rec + 0xAu, 0x0001u);
    wr_le_u16(rec + 0xCu, 0x0004u);
    wr_le_u16(rec + 0xEu, 0x0000u);
}

/** 1000:FA13 object_group_dissolve: free the entry, then give the owner (n = 0)
 * its standing human form, or each member (n >= 1) the FADA form.  `n` is the
 * member-list length - 1, exactly `(run length from +5) - 2` as the code
 * computes it; the `n >= 2` handler's second `XCHG CX,BX` at FA78 leaves
 * BX = n, so its loop runs `n - 1` times after the first member: n calls in
 * all, one per member.
 *
 * The n >= 1 paths then fall into `1000:FAA4`, which stores `y -= 8`,
 * frame 0x10, type 2 on the last member (it becomes a standing human). */
void object_group_dissolve_at(uint32_t slot)
{
    if (slot < 8u) _obj_group_dissolve((uint8_t *)_obj_register[slot]);
}

/** @brief Clear one group list record. */
static void _obj_group_clear_list(uint8_t *e)
{
    for (uint32_t i = 0; i < 5u; i++) /* 1000:FAC1/FAC4: five words */
        wr_le_u16(e + 4u + 2u * i, 0xFFFFu);
}

/** @brief Dissolve one group. */
static void _obj_group_dissolve(uint8_t *e)
{
    uint8_t *bp;
    uint32_t n;
    uint8_t *last; /* the topmost list byte's record */

    wr_le_u16(e + 0u, 0xFFFFu); /* 1000:FA16 */
    wr_le_u16(e + 2u, 0xFFFFu); /* 1000:FA1C */
    bp = e + 5;
    while (!(bp[0] & 0x80u)) bp++; /* past the terminator */
    n = (uint32_t)(bp - (e + 5)) - 1u; /* list length - 1     */
    if (n == 0u) {
        last = object_slot(bp[-1]); /* the owner byte      */
        if (last) {
            wr_le_u16(last + 4u, 0x0010u); /* frame 0x10          */
            wr_le_u16(last + 8u, 0x0002u); /* type 2, a human     */
        }
        _obj_group_clear_list(e); /* 1000:FABF */
        return;
    }
    last = object_slot(bp[-1]); /* n = 1: the only member */
    if (last) {
        _obj_group_restore_member(last);
        for (uint32_t k = 1u; k < n; k++) { /* the members below it */
            uint8_t *m = object_slot(bp[-1 - (int32_t)k]);
            if (m) _obj_group_restore_member(m);
        }
        /* 1000:FAA4: the last list record becomes a standing human 8 px up. */
        wr_le_u16(last + 2u, (uint16_t)(rd_le_u16(last + 2u) - 8u));
        wr_le_u16(last + 4u, 0x0010u);
        wr_le_u16(last + 8u, 0x0002u);
    }
    _obj_group_clear_list(e); /* 1000:FABF */
}

/** 1000:F453 scan_groups_for_record: the first 0x6F22 entry whose member list
 * (from +5) contains `recIndex` gets dissolved.  Returns 1 when one was. */
int32_t object_stack_scan_dissolve(uint32_t recIndex)
{
    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)_obj_register[i];
        if (e[1] & 0x80u) continue; /* +0 bit 15 set = free */
        for (uint8_t *p = e + 5; !(p[0] & 0x80u); p++)
            if (p[0] == (uint8_t)recIndex) {
                _obj_group_dissolve(e);
                return 1;
            }
    }
    return 0;
}

/** 0xEA71 (op 8): on odd frames reset the span entry of the acting record.
 * On even frames the original also clears [0x13A4]; the port's equivalent is
 * game_main's one-action-per-press latch (poll_input masks the ACTION bit
 * while [0x13A4] is set and clears it on release, 1000:9998). */
static void _obj_op_reset_span(void)
{
    if (!(_obj_frame & 1u)) return;
    for (uint32_t i = 0; i < 2u; i++) {
        uint16_t *e = _obj_span[i];
        if (e[0] != _obj_entity) continue;
        if (e[4] != 0u && e[3] > e[4]) {
            e[4] = 0u;
            e[9] = 0u;
        }
        return;
    }
}

/* The pending-action dispatch ([0x1979] -> the table at 0x197B), one frame
 * later.  Action 5 is event_anim_script_advance (1000:382D): it rewinds the
 * program's cursor byte to [0x1988], then calls blit_sprite_loop (1000:990A),
 * which re-draws the status bar's action row for the program's first two bytes
 * (count, gold flag) — the same bytes as the 0x5B2B group header.  That
 * drawing is already ported in statusbar.c as draw_actions() and runs every
 * tick via statusbar_tick(), so the press needs no extra call here. */
/** 1000:3747 with the acting record's +0xE (the bar drawing lives in
 * statusbar.c, which owns the band and panel4). */
static void _obj_charge_draw(void)
{
    uint8_t *actor = object_slot(object_action_entity());
    uint32_t v;

    if (!actor) return;
    /* The original's charge loop accumulates one stroke per frame (1000:3702
     * -> 0x3747, +0xE stepping by 4); its page is never rebuilt, so the
     * strokes stay.  The port recomposes the page every tick, so it redraws
     * the whole run - the same pixels the original accumulated (D-81). */
    v = (uint32_t)(rd_le_u16(actor + 0xEu) & 0x1FFu);
    statusbar_charge_bar(0u); /* 1000:36F7 */
    statusbar_charge_bar(2u); /* 1000:36FF */
    for (uint32_t x = 4u; x <= v; x += 4u) statusbar_charge_bar(x);
}

/** The pending-event dispatch (`[0x1979]` -> the table at CS:0x197B).  The port
 * only needs the two gameplay codes: 4 is the jump/throw charge loop and 5 is
 * the program-cursor rewind; 1/2/3/6 are the screen-level dispatch control
 * (lock, "return 0", a bare RET, spin-wait), none of which the port's tick
 * drives. */
void object_pending_run(void)
{
    uint32_t code = _obj_pending;

    _obj_pending = 0u;
    /* 1000:3705: any pending code ends the running charge loop (the loop
     * swallows it at 0x373B); its own event-4 request does not. */
    if (_obj_charging && code != 4u) _obj_charging = 0;
    switch (code) {
    case 4u: /* 1000:36DB: enter 0x36DB */ _obj_charging = 1; break;
    case 5u: { /* 1000:382D */
        uint8_t *p = _obj_programs[(_obj_program_index - 1u) % 6u];

        p[1] = (uint8_t)_obj_cursor;
        break;
    }
    default: /* 1000:36C7/36D7/36DA/3843 */ break;
    }
}

/** One frame of the charge loop (0x3702): the world frame has already run, so
 * this redraws the bar at the record's new +0xE, or leaves when the turn did
 * not advance (0x370C) - the loop's other exit, a pending code, is the ACTION
 * press and is consumed by object_action_run. */
int32_t object_charge_active(void) { return _obj_charging; }

/** @brief The charge loop's per-tick step. */
int32_t object_charge_tick(void)
{
    if (!_obj_charging) return 0;
    if (!_obj_charge_arm) { /* 1000:370C */
        _obj_charging = 0; /* the loop leaves; 0x373B */
        return 0;
    }
    _obj_charge_arm = 0u; /* 1000:3713 */
    _obj_charge_draw(); /* 1000:3718-0x3730 */
    return 1;
}

/** script_op_dispatch: one ACTION press. */
void object_action_run(void)
{
    const uint8_t *p = _obj_programs[(_obj_program_index - 1u) % 6u];

    /* 1000:3523/0x3530: pause_toggle returns while a sequence owns the
     * record ([0x6F1D] != 0), so the held frames keep reaching the type row's
     * fire half (the turn) and the charge keeps advancing.  During a charge
     * [0x6F1D] is exactly what blocks this call, so there is no separate
     * "press ends the charge" rule: the release ends it, when the fire
     * column stops arming [0x1987]. */
    if (_obj_lat) return;
    object_set_action_latched(1);

    uint8_t *actor = _obj_array + _obj_entity * OBJ_STRIDE;
    uint32_t op = p[p[1] + 1u];

    switch (op) {
    case OP_REGISTER: _obj_op_register(actor); break;
    case OP_COLLIDE_A:
    case OP_COLLIDE_B: _obj_op_collide(actor); break;
    case OP_SETFRAME: _obj_op_set_frame(actor); break;
    case OP_CLAIM_OR_TYPE: /* 0x39E4: the player claims, others walk */
        if (actor == _obj_array + _obj_player * OBJ_STRIDE)
            (void)object_claim_begin(2u, (int32_t)_obj_entity);
        else _obj_op_mark_walk(actor, rd_le_u16(actor + 8u) ? 0u : 0x10u);
        break;
    case OP_CLAIM3: (void)object_claim_begin(3u, (int32_t)_obj_entity); break;
    case OP_CLAIM0: (void)object_claim_begin(0u, (int32_t)_obj_entity); break;
    case OP_CLAIM1: (void)object_claim_begin(1u, (int32_t)_obj_entity); break;
    case OP_TYPE_A: /* 0x3A05 -> 0xEFA1 / 0xEFB9          */
        if (rd_le_u16(actor + 8u) <= 2u)
            _obj_op_set_state(actor, rd_le_u16(actor + 8u) ? 1u : 0u);
        break;
    case OP_RESET: /* 0x39CC -> 0xEA71                   */
        _obj_op_reset_span();
        break;
    case OP_SELECT: /* 0x3A1A -> 0xECFE                   */
        /* 0xECFE: needs a class, then page_offset_to_ptr (0xF123) on the map
         * cell under the record and on the cell one to the right (type 0) or
         * left; only when both are free does it set type 0x12/frame 0x8C
         * (type 0) or type 0x14/frame 0x91.  The 16-bit `DI += CX` can walk
         * off the map, so both indices are bounds-checked.  The original's
         * single exit (1000:ED49) clears the sequence latch [0x6F1D] on every
         * path and nothing in the body reads it, so the clear is done here. */
        _obj_lat = 0;
        {
            uint32_t t = rd_le_u16(actor + 8u);
            uint32_t step = (t == 0u) ? 1u : 0xFFFFu;
            uint32_t frame = (t == 0u) ? 0x8Cu : 0x91u;
            const uint8_t *map = level_map();
            int32_t col = (int16_t)rd_le_u16(actor + 0u) >> 4;
            int32_t row = (int16_t)rd_le_u16(actor + 2u) >> 4;
            int32_t cell = row * 32 + col;
            int32_t next = cell + (int32_t)(int16_t)step;

            if (!actor[7u] || t > 2u || !map) break;
            if (cell < 0 || cell >= 640) break;
            if (!level_page_free(map[cell])) break;
            if (next < 0 || next >= 640) break;
            if (!level_page_free(map[next])) break;
            wr_le_u16(actor + 8u, (uint16_t)((t == 0u) ? 0x12u : 0x14u));
            wr_le_u16(actor + 4u, (uint16_t)frame);
        }
        break;
    case OP_TYPE_B: /* 0x39CF -> 0xEA0F / 0xEA4D          */
        /* Op 7 is the **ride** command: `1000:39CF` loads BP = 0xEA0F for
         * type 0 and 0xEA4D otherwise and calls it - `cmd_ride_ab` /
         * `cmd_ride_ad`, the same two bodies the type rows reach as command 8
         * (`_obj_cmd_ride` above).  The port still stops here instead of calling
         * them: port_deviations.md D-92.  The earlier reading (that these
         * targets are address fragments inside 0xD5F0) named the wrong page;
         * re_map.md's op-7 section and re_history.md carry the correction.
         * The op is reachable only through a program cursor above 1, which the
         * shipped data never produces: every readable path pins the cursor at
         * 1, and the only unread producer is the 0FE7:0188 save-restore
         * overlay. */
        _obj_fault = OP_TYPE_B;
        if (_obj_osys && _obj_osys->fatalError) {
            _obj_osys->fatalError(
                "HUMANS: ACTION op 7 (1000:39CF) dispatched - it needs a "
                "program cursor above 1, which the shipped data never "
                "produces.  The op's targets are the ride commands "
                "(1000:EA0F/1000:EA4D); the port does not call them yet "
                "(port_deviations.md D-92).");
        }
        break;
    default: break;
    }
}

/* --- SAVE GAME state ranges (re_formats.md §10/§12.3) ------------------ */

/** 0x3E20..0x407E: [0x3E20] placed humans, [0x3E22] entity index, the 29
 * fixed pointers, the 20 class bytes, [0x3E73] live count and the 29 0x12
 * records at 0x3E75.  The pointer table is the image's constant run. */
void object_save_table(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x25Fu);
    wr_le_u16(dst + 0u, _obj_placed_humans);
    wr_le_u16(dst + 2u, _obj_entity);
    for (uint32_t i = 0; i < OBJ_SLOTS; i++)
        wr_le_u16(dst + 4u + 2u * i, (uint16_t)(0x3E75u + 0x12u * i));
    for (uint32_t i = 0; i < 21u; i++) dst[0x3Eu + i] = _obj_slot_class[i];
    wr_le_u16(dst + 0x53u, (uint16_t)_obj_live_count); /* [0x3E73] word */
    memcpy(dst + 0x55u, _obj_array, OBJ_SLOTS * OBJ_STRIDE);
}

/** @brief Load the behaviour table. */
void object_load_table(const uint8_t *src)
{
    if (!src) return;
    _obj_placed_humans = rd_le_u16(src + 0u);
    _obj_entity = rd_le_u16(src + 2u);
    if (_obj_entity >= OBJ_SLOTS) _obj_entity = 0u;
    memset(_obj_slot_class, 0, sizeof _obj_slot_class);
    for (uint32_t i = 0; i < 21u; i++) _obj_slot_class[i] = src[0x3Eu + i];
    _obj_live_count = rd_le_u16(src + 0x53u); /* [0x3E73] word */
    if (_obj_live_count > OBJ_SLOTS) _obj_live_count = OBJ_SLOTS;
    memcpy(_obj_array, src + 0x55u, OBJ_SLOTS * OBJ_STRIDE);
}

/** @brief Save the action programs. */
void object_save_programs(uint8_t *dst)
{
    if (!dst) return;
    memcpy(dst, _obj_program_base, 0x20u);
    for (uint32_t i = 0; i < 6u; i++)
        dst[_obj_program_start[i] + 1u] = _obj_programs[i][1];
    dst[0x1F] = (uint8_t)_obj_program_index;
}

/** @brief Load the action programs. */
void object_load_programs(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_program_region, src, 0x20u);
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t start = _obj_program_start[i];
        uint32_t n = 0x20u - start;

        if (n > 12u) n = 12u;
        memcpy(_obj_programs[i], _obj_program_region + start, n);
        /* n < 12 only for the last program: the save ends at 0x5B56, so its
         * tail is not in the file and the port keeps the static tail. */
    }
    _obj_program_index = src[0x1F];
    if (_obj_program_index < 1u || _obj_program_index > 6u)
        _obj_program_index = 1u;
}

/** 0x649A..0x64C2: the two TER.DAT slots (2 x 0x14). */
void object_save_ter(uint8_t *dst)
{
    if (!dst) return;
    memcpy(dst, _obj_ter_slot[0], 0x14u);
    memcpy(dst + 0x14u, _obj_ter_slot[1], 0x14u);
}

/** @brief Load the terrain table. */
void object_load_ter(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_ter_slot[0], src, 0x14u);
    memcpy(_obj_ter_slot[1], src + 0x14u, 0x14u);
}

/** 0x6F20..0x82EA: the special tiles, the 8 group templates at 0x6F22 and the
 * two 0x1C spans at 0x6F92; the image is zero elsewhere, and so is this. */
void object_save_pool(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x13CAu);
    wr_le_u16(dst + 0u, (uint16_t)level_special_tile(0u)); /* [0x6F20] */
    wr_le_u16(dst + 2u, (uint16_t)level_special_tile(1u)); /* [0x6F21] */
    memcpy(dst + 2u, _obj_register, sizeof _obj_register);
    memcpy(dst + 0x72u, _obj_span, sizeof _obj_span);
}

/** The stripe's pixels live on the page in the original and the page is not part
 * of any save block, so a restore starts the stripe empty: the port puts its
 * seed row at the entry's current top (1000:5D65 then draws nothing until the
 * next +0xC step). */
static void _obj_span_top_reset(void)
{
    for (uint32_t i = 0; i < 2u; i++)
        _obj_span_top[i] = rd_le_u16((uint8_t *)_obj_span[i] + 0xCu);
}

/** @brief Load the shared sprite pool. */
void object_load_pool(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_register, src + 2u, sizeof _obj_register);
    memcpy(_obj_span, src + 0x72u, sizeof _obj_span);
    _obj_span_top_reset();
}

/** 0x6F92..0x6FE6's second copy: the two spans, then zero. */
void object_save_spans(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x54u);
    memcpy(dst, _obj_span, sizeof _obj_span);
}

/** @brief Load the span table. */
void object_load_spans(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_span, src, sizeof _obj_span);
    _obj_span_top_reset();
}

/** 1000:D6B2 phases 1/2: walk the pending-move table, raising a record's
 * +4 tag 0x21 -> 0x22 for 0x28 ticks, then putting it back. */
static void _obj_pending_moves_tick(void)
{
    uint32_t placed = object_placed_humans();

    if (_pend_tag == 0u) {
        if (_pend_div-- > 0) return;
        if (_pend_cursor / 4u < 17u && placed != 0u) {
            uint32_t idx = _pend_index + _pend_off[_pend_cursor / 4u];

            while (idx >= placed) idx -= placed;
            _pend_div = _pend_wait[_pend_cursor / 4u];
            _pend_index = (uint16_t)idx;
            _pend_cursor = (uint16_t)(_pend_cursor + 4u);
            if (_pend_off[_pend_cursor / 4u] == 0xFFFFu) _pend_cursor = 0u;
            {
                uint8_t *r = object_slot(idx);

                if (r && rd_le_u16(r + 4u) == 0x21u) {
                    wr_le_u16(r + 4u, 0x22u);
                    _pend_tag = 1u;
                    _pend_count = 0x28u;
                    _pend_slot = (uint16_t)idx;
                }
            }
        }
    } else if (_pend_count-- == 0u) {
        uint8_t *r = object_slot(_pend_slot);

        if (r && rd_le_u16(r + 4u) == 0x22u) wr_le_u16(r + 4u, 0x21u);
        _pend_tag = 0u;
        _pend_count = 0u;
    }
}

/* --- 1000:D6B2 phase 3: the group-member animator ---------------------- */
/** 0x6F22 entry: word0 = base x (bit 15 free), +4 = direction<<4 | count,
 * +5.. = member list (bit-7 terminated).  Every 6th call the count steps
 * and each member record whose frame is 0x20..0x22 gets
 * x = group.word0 + _obj_anim_x[length-3][count][member]. */
void object_animator_tick(void)
{
    _obj_pending_moves_tick();

    if (_obj_anim_div-- <= 0) _obj_anim_div = 6;

    for (uint32_t gi = 0; gi < 8u; gi++) {
        uint8_t *g = (uint8_t *)_obj_register[gi];
        uint32_t len = 0, limit, c;
        uint8_t dl;

        if (rd_le_u16(g + 0u) & 0x8000u) continue;
        while ((g[5u + len] & 0x80u) == 0) len++;
        if (len < 3u || len > 8u) continue;
        limit = 2u * (len - 1u);
        c = g[4] & 0x0Fu;
        /* 1000:D795: the state byte's high nibble is carried in DL (bits 4..7,
         * the 0x80 sentinel included) and only bit 4 is the direction. */
        dl = (uint8_t)(g[4] & 0xF0u);
        if (_obj_anim_div == 0) {
            if (!(dl & 0x10u)) {
                if (c == 0u) dl ^= 0x10u;
                else c--;
            } else {
                if (c >= limit) {
                    c = limit;
                    dl ^= 0x10u;
                } else c++;
            }
        }
        if (c > limit) c = limit;
        /* 1000:D81D..D850: the member list is walked BACKWARDS from the
         * terminator while the offset run is read forwards, so the last (top)
         * member takes the run's first offset and the owner takes the last -
         * which is 0 in every shipped run.  `_obj_anim_x[..][c][j]` is the run's
         * word j+1, so the list index m takes column `len - 1 - m`. */
        for (uint32_t m = 0; m < len; m++) {
            uint8_t *r = object_slot(g[5u + m]);
            uint32_t frame;

            if (!r) continue;
            frame = rd_le_u16(r + 4u);
            if (frame < 0x20u || frame > 0x22u) continue;
            wr_le_u16(
                r + 0u,
                (uint16_t)(rd_le_u16(g + 0u) +
                           (uint16_t)_obj_anim_x[len - 3u][c][len - 1u - m]));
        }
        g[4] = (uint8_t)(dl | (c & 0x0Fu)); /* 1000:D813 OR DL,CL */
    }
}

/** @brief Accessor: one group record. */
uint8_t *object_group_at(uint32_t i)
{ return i < 8u ? (uint8_t *)_obj_register[i] : NULL; }
/** @brief Reset the animator's state. */
void object_animator_reset(void)
{
    _obj_anim_div = 0;
    _pend_tag = _pend_count = _pend_slot = _pend_index = _pend_cursor = 0u;
    _pend_div = 0;
}
