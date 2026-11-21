#include "object_internal.h"
#include "object_command.h"
#include "object_core.h"
#include "object_units.h"
#include "level/level.h"
#include "osystem.h"
#include "byteorder.h"

/* forward declarations */
static int32_t object_command_selector_walk(uint8_t *actor, int32_t right);
static uint32_t _obj_command_walk_test(const uint8_t *actor);
static void _obj_command_settle_landed(uint8_t *actor);
static void _obj_command_settle(uint8_t *actor);
static void _obj_command_idle_a(uint8_t *actor);
static void _obj_command_idle_b(uint8_t *actor);
static int32_t _obj_command_frame_cadence(uint8_t *actor);
static void _obj_command_neighbour_pass(uint8_t *actor, uint32_t offX,
                                        uint32_t offY, int32_t delta);
static void _obj_command_enter(uint8_t *actor);
static void _obj_command_nudge(uint8_t *actor, int32_t step);
static void _obj_command_path_step(uint8_t *actor, int32_t forward);
static void _obj_command_path_advance(uint8_t *actor);
static void _obj_command_carry_form(uint8_t *rec, uint32_t phase);
static void _obj_command_type15_give(uint8_t *actor);
static void _obj_command_type15_tail(uint8_t *actor);
static void _obj_command_frame_step(uint8_t *actor, uint32_t bound, int32_t up);
static void _obj_command_turn(uint8_t *actor);
static void _obj_command_leave(uint8_t *actor);
static void _obj_command_turn_idle(uint8_t *actor);
static void _obj_command_fall(uint8_t *actor, int32_t x, int32_t y,
                              uint32_t frame);
static void _obj_command_place(uint8_t *actor, int32_t x, int32_t y,
                               uint32_t frame);
static void _obj_command_stand(uint8_t *actor);
static void _obj_command_stream_timed(uint8_t *actor);
static void _obj_command_clear_countdown(uint8_t *actor);
static int32_t _obj_command_call(uint8_t *actor, uint16_t addr);
static void _obj_command_pending_moves_tick(void);

uint8_t _obj_anim_div; // [0x66B1]: the 6-tick divider

/* 0x67F1's pending-move chain: tag, phase-2 countdown, slot, phase-1
 * divider/index/cursor into the 0x67FF (offset, divider) table. */
uint16_t _obj_anim_move_tag, _obj_anim_move_count, _obj_anim_move_slot,
    _obj_anim_move_index, _obj_anim_move_cursor;

uint8_t _obj_anim_move_div;

/* The group animator's offset runs, [length-3][count][member] (1000:D6B2).
 * The original reads an 8-word run; the ninth column the table used to carry
 * was never indexed (member <= len-1 <= 7). */
static const int16_t _obj_anim_x[6][15][8] = {
    {
        /* length 3 */
        { -2, -1, 0, 0, 0, 0, 0, 0 },
        { -1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 4 */
        { -3, -2, -1, 0, 0, 0, 0, 0 },
        { -2, -1, 0, 0, 0, 0, 0, 0 },
        { -1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 0, 0, 0, 0, 0 },
        { 3, 2, 1, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 5 */
        { -4, -3, -2, -1, 0, 0, 0, 0 },
        { -3, -2, -1, 0, 0, 0, 0, 0 },
        { -2, -1, 0, 0, 0, 0, 0, 0 },
        { -1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 0, 0, 0, 0, 0 },
        { 3, 2, 1, 0, 0, 0, 0, 0 },
        { 4, 3, 2, 1, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 6 */
        { -5, -4, -3, -2, -1, 0, 0, 0 },
        { -4, -3, -2, -1, 0, 0, 0, 0 },
        { -3, -2, -1, 0, 0, 0, 0, 0 },
        { -2, -1, 0, 0, 0, 0, 0, 0 },
        { -1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 0, 0, 0, 0, 0 },
        { 3, 2, 1, 0, 0, 0, 0, 0 },
        { 4, 3, 2, 1, 0, 0, 0, 0 },
        { 5, 4, 3, 2, 1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 7 */
        { -6, -5, -4, -3, -2, -1, 0, 0 },
        { -5, -4, -3, -2, -1, 0, 0, 0 },
        { -4, -3, -2, -1, 0, 0, 0, 0 },
        { -3, -2, -1, 0, 0, 0, 0, 0 },
        { -2, -1, 0, 0, 0, 0, 0, 0 },
        { -1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 0, 0, 0, 0, 0 },
        { 3, 2, 1, 0, 0, 0, 0, 0 },
        { 4, 3, 2, 1, 0, 0, 0, 0 },
        { 5, 4, 3, 2, 1, 0, 0, 0 },
        { 6, 5, 4, 3, 2, 1, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        /* length 8 */
        { -7, -6, -5, -4, -3, -2, -1, 0 },
        { -6, -5, -4, -3, -2, -1, 0, 0 },
        { -5, -4, -3, -2, -1, 0, 0, 0 },
        { -4, -3, -2, -1, 0, 0, 0, 0 },
        { -3, -2, -1, 0, 0, 0, 0, 0 },
        { -2, -1, 0, 0, 0, 0, 0, 0 },
        { -1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 0, 0, 0, 0, 0 },
        { 3, 2, 1, 0, 0, 0, 0, 0 },
        { 4, 3, 2, 1, 0, 0, 0, 0 },
        { 5, 4, 3, 2, 1, 0, 0, 0 },
        { 6, 5, 4, 3, 2, 1, 0, 0 },
        { 7, 6, 5, 4, 3, 2, 1, 0 },
    },
};


/* 0x6DF2: the static two-byte (frame, hold) stream the type-2 command-8
 * handler walks; 0xFF 0xFF terminates and wraps. */
const uint8_t _obj_anim_stream[] = {
    0x10, 0xC8, 0x10, 0xC8, 0x12, 0x05, 0x10, 0x14, 0x12, 0x05, 0x10, 0xC8,
    0x10, 0xC8, 0x13, 0x06, 0x14, 0x06, 0x15, 0x06, 0x16, 0x28, 0x15, 0x06,
    0x14, 0x06, 0x13, 0x06, 0x10, 0xC8, 0x12, 0x05, 0x10, 0x0A, 0x12, 0x05,
    0x10, 0x32, 0x13, 0x06, 0x14, 0x06, 0x15, 0x06, 0x16, 0x23, 0x15, 0x06,
    0x14, 0x06, 0x13, 0x06, 0x10, 0xC8, 0x10, 0xC8, 0x12, 0x05, 0x10, 0x14,
    0x12, 0x05, 0x10, 0xC8, 0x10, 0xC8, 0xFF, 0xFF,
};


/* DS:0x6908, DS:0x698A, DS:0x6918 - the command dispatch's three tables
 * (1000:E3F6), copied verbatim from the image (file offset = DS offset + 0x106F0):
 *  - _obj_cmd_key      the input bit (low nibble of [0x5ACE]) -> command byte: bit 7
 *                   = no handler, bit 6 = one-shot, low 6 bits = the command
 *                   number; the original reads the 0x6914 tail word for indexes
 *                   12..15, but `_obj_sequence_normalise_input` clears one of
 *                   each opposing pair, so those indexes are unreachable (D-268);
 *  - _obj_cmd_rows     the per-type rows, 32 bytes each (two 16-byte halves, bit 7
 *                   of the input picks the second), ending exactly at 0x6DEA;
 *  - _obj_cmd_handlers the 57 handler addresses, index n = word at 0x6918+2(n-1).
 * Index 2 is 1000:E609 (its tail holds the human-stack push), 10/11 the leaves,
 * 14 the snap. */
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


static const uint8_t _obj_cmd_key[12] = {
    0x48u, 0x00u, 0x04u, 0xFFu, 0x02u, 0x01u,
    0x03u, 0xFFu, 0x06u, 0x07u, 0x05u, 0x00u,
};


static const uint8_t _obj_cmd_rows[35][32] = {
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


/* DS:0x6E3C - the byte EBD0 adds to its 1/3 step, indexed by the node's
 * progress >> 3 (the values run 3, 2 x 7, 1 x 12, then 0). */
static const uint8_t _obj_path_scale[0x20] = {
    0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};


/* 0x67FF: the pending-move (offset, divider) table; the word after each
 * entry is the next offset, so 0xFFFF ends the run and resets the cursor. */
static const uint16_t _obj_anim_move_off[18] = {
    1, 2, 3, 5, 2, 3, 5, 6, 2, 1, 2, 3, 2, 3, 4, 5, 2, 0xFFFF,
};

static const uint16_t _obj_anim_move_wait[17] = {
    0x28, 0x14, 0x0F, 0x05, 0x0A, 0x14, 0x1E, 0x19, 0x08,
    0x0C, 0x0F, 0x12, 0x32, 0x17, 0x3C, 0x0A, 0x04,
};


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_command_anim_move_off_for_probe(uint32_t i)
{
    return i < sizeof _obj_anim_move_off / sizeof _obj_anim_move_off[0]
               ? _obj_anim_move_off[i]
               : 0u;
}
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_command_anim_move_wait_for_probe(uint32_t i)
{
    return i < sizeof _obj_anim_move_wait / sizeof _obj_anim_move_wait[0]
               ? _obj_anim_move_wait[i]
               : 0u;
}
#endif /* HUMANSRE_TEST_PROBES */


ObjCommand object_command_select(uint8_t *rec, uint8_t input)
{
    ObjCommand out;
    uint8_t v = _obj_sequence_normalise_input(input);
    uint32_t key = v & JOYBITS_DIRS;
    uint8_t cmd = key < sizeof _obj_cmd_key ? _obj_cmd_key[key] : 0x80u;
    const uint8_t *row;
    uint16_t type = rd_le_u16(rec + 8u); // [SI+8]
    uint8_t idx;

    out.handler = 0u;
    out.tail = 0u;
    out.hook = 0u;
    out.command = 0u;
    if (type >= 35u) return out; // no row: the original reads 0x6DEA
    row = _obj_cmd_rows[type] + ((v & JOYBITS_FIRE) ? 0x10u : 0u);
    out.tail = rd_le_u16(row + 0xCu);
    out.hook = row[0xAu];
    if (cmd & 0x80u) return out; // bit 7: this input has no command
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


/** @brief 1000:F25F: whether the walk can land - the step probe's landing must not
 * rise more than 8 px above the feet (a step-up blocks), and the item record's
 * bit for the sub-cell 0x10 below the actor's y must be clear.  0 = free,
 * 0xFFFF = blocked.  `1000:F25F` loads AX = [0x4238] for the probe, i.e. it is
 * `x + 2` walking left and `x + 0xE` walking right - not the selector's fixed
 * `x + 8` (`1000:F1D7`).
 *
 * @param actor the object record
 * @return 0 when the walk can land, 0xFFFF when it is blocked
 */
static uint32_t _obj_command_walk_test(const uint8_t *actor)
{
    uint32_t sub, id, bit, cell;
    int32_t x, y, d;
    int32_t bx = _obj_core_step_probe_at(
        actor, (int32_t)(int16_t)_obj_sel_frame, 0x18, &cell);

    if (bx >= 0) {
        d = bx - 0x18 - (int32_t)(int16_t)rd_le_u16(actor + 2u);
        if (d < 0 && d + 8 < 0) return 0xFFFFu;
    }
    x = (int32_t)(int16_t)rd_le_u16(actor + 0u) +
        (int32_t)(int16_t)_obj_sel_frame;
    y = (int32_t)(int16_t)rd_le_u16(actor + 2u) + 0x10;
    id = _obj_core_map_at_abs(x, y, &sub);
    bit = 1u << (0xFu - ((uint32_t)x & 0xFu));
    return (level_item_word(id, ((uint32_t)y & 0xFu)) & bit) ? 0xFFFFu : 0u;
}


/** @brief 1000:E6FB (left) / 1000:E764 (right): the walk step.  Sets the result
 * frame/type to 2/0x10 or 0xE/0x10, asks 1000:F25F, then moves x by one and
 * advances the walk frame (8..0xF left, 0..7 right) every fifth frame.
 *
 * @param actor the object record
 * @param left the left value
 */
void _obj_command_walk(uint8_t *actor, int32_t left)
{
    int32_t ax = (int32_t)(int16_t)rd_le_u16(actor + 0u);
    uint8_t lo = left ? 8u : 0u, hi = left ? 0x10u : 8u, f;

    if (left ? ax <= 0 : ax + 16 >= (int32_t)OBJ_MAP_W)
        return; // the sides are hard (D-270)
    _obj_sel_frame = left ? 2u : 0xEu;
    _obj_sel_type = 0x10u;
    if (_obj_command_walk_test(actor) != 0u) return; // blocked: no step
    wr_le_u16(actor + 0u,
              (uint16_t)(rd_le_u16(actor + 0u) + (left ? 0xFFFFu : 1u)));
    wr_le_u16(actor + 0xAu, (uint16_t)(rd_le_u16(actor + 0xAu) - 1u));
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 0xAu, 0x0004u);
    f = (uint8_t)(((rd_le_u16(actor + 4u) + 1u) & 0xFFFFu));
    if (f >= hi) f = lo;
    wr_le_u16(actor + 4u, f);
}


/**
 * @brief 1000:D9B2: the settle's landing state (type 3, hold 1, frame 4).
 *
 * @param actor the object record
 */
static void _obj_command_settle_landed(uint8_t *actor)
{
    wr_le_u16(actor + 8u, 0x0003u);
    wr_le_u16(actor + 0xAu, 0x0001u);
    wr_le_u16(actor + 0xCu, 0x0004u);
    wr_le_u16(actor + 0xEu, 0x0000u);
}


/**
 * @brief 1000:D8BD: the terrain settle the type rows' +0xA hook runs.
 *
 * Snaps the actor to the slope under its feet, lands it (type 3) on a drop of
 * more than 8 px, and caches the settled position at 0x6847.  1000:D8C3 keeps
 * the unaligned feet line in CX for the distance test and masks BX to the row
 * base (AND BX,0xFFF0, 1000:D8D0); the page lookup and the step use the masked
 * value.
 *
 * @param actor the object record
 */
static void _obj_command_settle(uint8_t *actor)
{
    int32_t x = (int16_t)rd_le_u16(actor + 0u);
    int32_t y = (int16_t)rd_le_u16(actor + 2u);
    int32_t ax = x + 8, feet = y + 0x18, line = feet & ~0xF, bx, d;
    uint32_t sub, cell, b;

    if (x < 0 || x >= (int32_t)OBJ_MAP_W) return; // the sides are hard (D-270)
    if (!_obj_units_settle_gate(actor)) return;
    cell = _obj_core_map_at_abs(ax, line, &sub);
    if (cell == level_special_tile(0u) || cell == level_special_tile(1u))
        return;
    b = level_page_byte(cell, sub);
    _obj_side = 0;
    if (b == 0u) { // 1000:D91D
        b = level_page_byte(_obj_core_map_at_abs(ax, line - 0x10, &sub), sub);
        if (!(b & 0x80u)) {
            _obj_side = (int16_t)0xFFE0;
            line -= 0x10;
        } else {
            b = 0u;
        }
    } else if (b & 0x80u) { // 1000:D945
        line += 0x10;
        _obj_side = 0x20;
        b = level_page_byte(_obj_core_map_at_abs(ax, line, &sub), sub);
        if (b & 0x80u) { // 1000:D9B2
            _obj_command_settle_landed(actor);
            return;
        }
    }
    bx = line + (int32_t)(int8_t)b; // 1000:D967
    d = feet - bx; // 1000:D96A: CX
    if (d < 0) {
        d = -d;
        if (d > 8) {
            _obj_command_settle_landed(actor);
            return;
        }
    }
    wr_le_u16(actor + 2u, (uint16_t)(bx - 0x18));
    _obj_foot_x = (uint16_t)(rd_le_u16(actor + 0u) + 8u);
    _obj_foot_y = (uint16_t)(rd_le_u16(actor + 2u) + 0x18u);
    if (_obj_spawn_bg_mark(actor) != 0) return; // 1000:D986
    _obj_settled[(uint32_t)((actor - _obj_array) / OBJ_STRIDE)][0] =
        rd_le_u16(actor + 0u);
    _obj_settled[(uint32_t)((actor - _obj_array) / OBJ_STRIDE)][1] =
        rd_le_u16(actor + 2u);
}


/** @brief 1000:E72A (index 4) and 1000:E790 (index 6): the two idle handlers.  Both
 * keep the frame in its cycle (8..0x10 / below 8 and 0x10) with a 4-frame
 * countdown in `+0xA`; the first returns to frame 0x10/type 2 and then 8/1, the
 * second to 0x10/type 2 and then 0/0.
 *
 * @param actor the object record
 */
static void _obj_command_idle_a(uint8_t *actor)
{
    uint16_t f = rd_le_u16(actor + 4u);

    if (f < 8u || f > 0x10u) { // 1000:E739: stand up
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
        wr_le_u16(actor + 0xAu, 0x0004u);
        return;
    }
    wr_le_u16(actor + 0xAu,
              (uint16_t)(rd_le_u16(actor + 0xAu) - 1u)); // 1000:E74F
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 4u, 0x0008u);
    wr_le_u16(actor + 8u, 0x0001u);
    wr_le_u16(actor + 0xAu, 0x0004u);
}


/**
 * @brief Type 18 command 6: the second idle.
 *
 * @param actor the object record
 */
static void _obj_command_idle_b(uint8_t *actor)
{
    uint16_t f = rd_le_u16(actor + 4u);

    if (f >= 8u && f != 0x10u) { // 1000:E79D: stand up
        wr_le_u16(actor + 4u, 0x0010u);
        wr_le_u16(actor + 8u, 0x0002u);
        wr_le_u16(actor + 0xAu, 0x0004u);
        return;
    }
    wr_le_u16(actor + 0xAu,
              (uint16_t)(rd_le_u16(actor + 0xAu) - 1u)); // 1000:E7AD
    if ((int16_t)rd_le_u16(actor + 0xAu) >= 0) return;
    wr_le_u16(actor + 4u, 0x0000u);
    wr_le_u16(actor + 8u, 0x0000u);
    wr_le_u16(actor + 0xAu, 0x0004u);
}


/** @brief 1000:F9E4 object_advance_frame: the once-per-frame gate.  The first call in a
 * frame stores the frame number at `+0xA` and resets the 0x19-frame countdown
 * (`+0xE`); the repeats tick it down and, when it expires, reset it.  Returns 1
 * while the record must wait, 0 on the 25-frame tick (the original's ZF: every
 * path EXCEPT the expiry ends in `XOR AX,AX`).
 *
 * @param actor the object record
 * @return 1 while the record must wait, 0 on the 25-frame tick
 */
static int32_t _obj_command_frame_cadence(uint8_t *actor)
{
    uint16_t ax = _obj_frame;

    if (ax == rd_le_u16(actor + 0xAu)) { // 1000:F9FD
        uint16_t e = (uint16_t)(rd_le_u16(actor + 0xEu) - 1u); // 1000:FA01 SUB

        wr_le_u16(actor + 0xAu, (uint16_t)(ax + 1u));
        wr_le_u16(actor + 0xEu, e);
        if ((int16_t)e < 0) {
            wr_le_u16(actor + 0xEu, 0x0019u); // 1000:FA0A
            return 0;
        }
        return 1;
    }
    wr_le_u16(actor + 0xAu, (uint16_t)(ax + 1u)); // 1000:F9EC
    wr_le_u16(actor + 0xEu, 0x0019u);
    wr_le_u16(actor + 0x10u, 0u);
    return 1;
}


/** @brief 1000:FAF4: the neighbour pass.  For every appended record whose class slot is
 * 4, that sits within (-0x10, 0] of the probe point on both axes and whose type
 * magnitude is <= 0xA, it reads the item word under the record's feet
 * (x + (delta >= 0 ? 0x10 : 0), y + 8) and, when the fine-x bit is clear, adds
 * the caller's delta to the record's type word.  `delta` is +0xA from 1000:EC99
 * (a step right) and -0xA from 1000:ECA8 (a step left).
 *
 * @param actor the object record
 * @param offX the x offset
 * @param offY the y offset
 * @param delta the delta
 */
static void _obj_command_neighbour_pass(uint8_t *actor, uint32_t offX,
                                        uint32_t offY, int32_t delta)
{
    int32_t x = (int32_t)(int16_t)(uint16_t)(rd_le_u16(actor + 0u) + offX);
    int32_t y = (int32_t)(int16_t)(uint16_t)(rd_le_u16(actor + 2u) + offY);
    uint16_t aux = (uint16_t)delta; // 1000:FAF9: [0x4228], local to this pass
    uint8_t n =
        (uint8_t)(_obj_live_count > OBJ_LIVE0 ? _obj_live_count - OBJ_LIVE0
                                              : 0u);

    _obj_sel_frame = delta >= 0 ? 0x10u : 0u; // 1000:FAFD
    _obj_sel_type = 8u; // 1000:FB03
    for (uint16_t k = 0; k < n; k++) { // 1000:FB29
        uint8_t *r = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        int32_t d, ty;

        _obj_side = rd_le_u16(r + 0u);
        _obj_sel_xoff = rd_le_u16(r + 2u);
        if ((_obj_slot_class[1u + k] & 0x3Fu) != 4u) continue; // 1000:FB39
        if (r[6] & 0x40u) continue; // 1000:FB46
        d = (int16_t)(uint16_t)(rd_le_u16(r + 0u) - (uint16_t)x);
        if (d >= 0 || d + 0x10 < 0) continue; // 1000:FB4F
        d = (int16_t)(uint16_t)(rd_le_u16(r + 2u) - (uint16_t)y);
        if (d >= 0 || d + 0x10 < 0) continue; // 1000:FB62
        ty = (int16_t)rd_le_u16(r + 8u);
        if (ty < 0) ty = -ty;
        if (ty > 0xA) continue; // 1000:FB7E
        {
            int32_t px =
                (int32_t)(int16_t)rd_le_u16(r + 0u) + (delta >= 0 ? 0x10 : 0);
            int32_t py = (int32_t)(int16_t)rd_le_u16(r + 2u) + 8;
            uint32_t sub, tile = _obj_core_map_at_abs(px, py, &sub);
            uint16_t word =
                (uint16_t)(level_item_word(tile, (uint32_t)(py & 0xF)));
            uint16_t bit = (uint16_t)(1u << (0xFu - ((uint32_t)px & 0xFu)));

            if (!(word & bit)) // 1000:FC17
                wr_le_u16(r + 8u,
                          (uint16_t)(rd_le_u16(r + 8u) + aux)); // 1000:FC1D
        }
    }
}


/** @brief 1000:EE3B: the enter-the-building sequence, run in place of the handler while
 * [0x6F1D] is set.  It releases the latch, folds the record's direction counter
 * (`+0xE`, 0..0x1FF) into a distance and the frame (0x8C..0x93) into one of the
 * eight `0x6E76` four-word vectors - the door's x, its y, the record's new frame
 * and type - offsets the door x by that distance, frees the record's class slot
 * and turns that slot's placed record into a marker (type 0xFFFF, the door's
 * coordinates in its +0xC/+0xE), then arms the ACTION dispatch ([0x5B56] = 1,
 * [0x6916] = 5, [0x6917] = 1) and plays effect 3.
 *
 * @param actor the object record
 */
static void _obj_command_enter(uint8_t *actor)
{
    static const int16_t doors[8][4] = {
        /* 0x6E76: dx, y, frame, type */
        { 3, -5, 0, 0 }, { 2, -7, 0, 0 },  { 1, -9, 0, 0 },  { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },  { -3, -5, 8, 1 }, { -2, -7, 8, 1 }, { -1, -9, 8, 1 },
    };
    uint32_t ax, idx, dx;
    uint8_t *rec;

    if (!_obj_sequence_active) { // 1000:EE42
        wr_le_u16(actor + 0xEu, 0u);
        return;
    }
    _obj_sequence_active = 0; // 1000:EE48
    ax = rd_le_u16(actor + 0xEu);
    if (ax >= 0x100u) ax = 0x1FFu - ax; // 1000:EE55
    ax >>= 6;
    dx = ((rd_le_u16(actor + 4u) - 0x8Cu) & 0xFFFFu);
    if (dx >= 8u) return; // past 0x6E76's eight entries
    _obj_door_x = (uint16_t)(doors[dx][0] >= 0 ? doors[dx][0] + (int32_t)ax
                                               : doors[dx][0] - (int32_t)ax);
    _obj_door_y = (uint16_t)doors[dx][1]; // 1000:EE8B
    wr_le_u16(actor + 4u, (uint16_t)doors[dx][2]); // 1000:EE94
    wr_le_u16(actor + 8u, (uint16_t)doors[dx][3]);
    wr_le_u16(actor + 0xAu, 0u);

    idx = actor[7]; // 1000:EEB8: the 1-based class
    actor[7] = 0u;
    /* The class byte write is `AND [0x3E5E + idx], 0x3F` (idx 0 reaches
     * 0x3E5E through the `DEC DX` wrap).  The original's record write for a
     * class byte of 0 lands at 0x510D..0x5114, where nothing reads; the port
     * drops it (D-271). */
    _obj_core_class_byte_set(idx,
                             _obj_core_class_byte_at(idx) &
                                 0x3Fu); // 0x3E5F + idx - 1
    if (idx != 0u && 8u + idx < OBJ_SLOTS) {
        rec = _obj_array + (8u + idx) * OBJ_STRIDE; // 0x3F17 + 0x12*(idx-1)
    } else {
        rec = NULL; // no class, or a malformed one (D-101)
    }
    if (rec) {
        wr_le_u16(rec + 8u, 0xFFFFu); // 1000:EED5
        wr_le_u16(rec + 0xAu, 0u);
        wr_le_u16(rec + 0xCu, _obj_door_x); // 1000:EEE5
        wr_le_u16(rec + 0xEu, _obj_door_y);
    }
    _obj_sequence_set_program_index(1u); // 1000:EEF2: [0x5B56] = 1
    _obj_result_pending = 5; // 1000:EEF7
    _obj_result_cursor = 1; // 1000:EEFC
    _obj_core_driver_call(3);
}


/** @brief 1000:FC32: nudge the node's progress.  The *acting* record's own `+0xA`
 * accumulates the step (clamped at +/-8; a value about to pass +/-9 enters at
 * 0xFFF8 / 8) and the signed accumulator is added to the node's `+8`, unless the
 * node's `+0xA` word is non-zero (a "busy" gate) or the stored progress already
 * has the same sign and is 0xA away.
 *
 * @param actor the object record
 * @param step the step
 */
static void _obj_command_nudge(uint8_t *actor, int32_t step)
{
    uint8_t *node = _obj_core_link_record(rd_le_u16(actor + 0xEu));
    uint16_t bx;
    int32_t cx;

    if (!node) return;
    if (rd_le_u16(node + 0xAu) != 0u) return; // 1000:FC3C
    bx = (uint16_t)(rd_le_u16(actor + 0xAu) + (uint16_t)step);
    if ((int16_t)bx < 0) {
        if (bx == (uint16_t)-9) bx = 0xFFF8u; // 1000:FC45
    } else if (bx == 9u) {
        bx = 8u; // 1000:FC4F
    }
    wr_le_u16(actor + 0xAu, bx); // 1000:FC57
    cx = (int16_t)rd_le_u16(node + 8u);
    if ((int16_t)bx < 0) {
        if (cx >= 0) {
            wr_le_u16(node + 8u, (uint16_t)(cx + (int16_t)bx));
            return;
        }
        if (-cx >= 0xA) return; // 1000:FC69
    } else {
        if (cx < 0) {
            wr_le_u16(node + 8u, (uint16_t)(cx + (int16_t)bx));
            return;
        }
        if (cx >= 0xA) return; // 1000:FC78
    }
    wr_le_u16(node + 8u, (uint16_t)(cx + (int16_t)bx)); // 1000:FC7E
}


/** @brief 1000:EBA0 / 1000:EB78: step the node's progress by +1 / -1 until it reaches
 * +0xF / -0xF, where the acting record takes frame 0x4A and the node's class
 * byte is parked at 0xC (`& 7` = 4).
 *
 * @param actor the object record
 * @param forward the forward flag
 */
static void _obj_command_path_step(uint8_t *actor, int32_t forward)
{
    uint8_t *node = _obj_core_link_record(rd_le_u16(actor + 0xEu));
    int32_t v;

    if (!node) return;
    if ((node[7] & 7u) == 4u) return; // 1000:EBA6 / 0xEB7E
    v = (int16_t)rd_le_u16(node + 8u);
    if (forward) {
        if (v >= 0 || -v < 0xF) {
            _obj_command_nudge(actor, 1);
            return;
        }
    } else {
        if (v < 0 || v < 0xF) {
            _obj_command_nudge(actor, -1);
            return;
        }
    }
    wr_le_u16(actor + 4u, 0x004Au); // 1000:EBBa / 0xEB90
    node[7] = 0x0Cu;
}


/** @brief 1000:EBD0: advance the node to the map under it.  It probes
 * (node.x + 8, node.y + 0x10), reads the item byte for the fine x (bit 7 picks
 * the 1/3 step, `word[node+8]`'s sign the other case), scales |progress| >> 3
 * through 0x6E3C (the 0x3B / 0x30 cells add 8 first), and - only while the
 * node's `+0xA` is 0 - stores the result at `+0xA` and the node's y at `+0x10`,
 * with effect 2.
 *
 * @param actor the object record
 */
static void _obj_command_path_advance(uint8_t *actor)
{
    uint8_t *node = _obj_core_link_record(rd_le_u16(actor + 0xEu));
    uint32_t x, y, tile, fine, step;
    int32_t ax;

    if (!node) return;
    if (node[7] != 0u) return; // 1000:EBD3
    x = (rd_le_u16(node + 0u) + 8u);
    y = rd_le_u16(node + 2u);
    fine = x & 0xFu;
    tile = _obj_core_map_at_abs((int32_t)x, (int32_t)y + 0x10,
                                NULL); // 1000:EBEF: the map cell
    step = 3;
    if (level_slope_byte(tile, fine) & 0x80u) { // 1000:EC27, field A
        if (!(rd_le_u16(node + 8u) & 0x8000u)) step = 1;
    } else if (rd_le_u16(node + 8u) & 0x8000u) {
        step = 1;
    }
    ax = (int16_t)rd_le_u16(node + 8u); // 1000:EC43
    if (ax < 0) {
        ax = -ax;
        if (tile == 0x3Bu) ax += 8; // 1000:EC53
    } else if (tile == 0x30u) {
        ax += 8; // 1000:EC5D
    }
    ax >>= 3;
    step += (uint32_t)(int8_t)_obj_path_scale[(uint32_t)ax & 0x1Fu];
    (void)tile;
    if (rd_le_u16(node + 0xAu) != 0u) return; // 1000:EC72
    wr_le_u16(node + 0xAu, (uint16_t)step); // 1000:EC78 (DI = node+0xA)
    wr_le_u16(node + 0x10u, (uint16_t)y); // 1000:EC7A
    _obj_core_driver_call(2); // 1000:EC7E
}


/** @brief The carried form a matched type-15 probe takes (0xDC9A..0xDCBF).
 *
 * 1000:DC1F runs after the path handlers: it probes around the linked node
 * (`+0xE`, the 1000:9CF0 record) for the first other record within |dy| <= 8
 * of node.y + 0x10 and |dx| <= 1 of node.x + 8, type <= 2.
 *
 * A match becomes frame 0x55, type 0x10, +0xA/+0xC cleared, +0xE = 0xF0 +
 * ([0x4103] & 0x3F) and effect 8.  The player's record takes the give exit
 * (0xDD3C) instead.  The group pass dissolves a matching entry's first member
 * and writes the form into that placed record (0xDD2D), not the entry.
 *
 * @param rec the record
 * @param phase the phase
 */
static void _obj_command_carry_form(uint8_t *rec, uint32_t phase)
{
    wr_le_u16(rec + 4u, 0x0055u); // 1000:DC9A
    wr_le_u16(rec + 8u, 0x0010u);
    wr_le_u16(rec + 0xAu, 0u);
    wr_le_u16(rec + 0xCu, 0u);
    wr_le_u16(rec + 0xEu, (uint16_t)(0xF0u + phase)); // 1000:DCB0
    _obj_core_driver_call(8); // 1000:DCBF
}


/**
 * @brief 1000:DD3C: the type-15 carry give-back tail.
 *
 * @param actor the object record
 */
static void _obj_command_type15_give(uint8_t *actor)
{
    uint8_t cls = actor[7];
    uint8_t slot;
    uint8_t *rec;

    actor[7] = 0u;
    if (!cls) return;
    _obj_core_class_byte_set(cls,
                             _obj_core_class_byte_at(cls) &
                                 0x7Fu); // 0x3E5F + cls - 1
    slot = 8u + cls; // 0x3F17 + 0x12*(cls-1)
    if (slot >= OBJ_SLOTS) return;
    rec = _obj_array + slot * OBJ_STRIDE;
    wr_le_u16(rec + 0xAu, 1u);
    wr_le_u16(rec + 0xEu, 0xFFFFu);
    rec[6] &= 0x7Fu; // 1000:DD63
    wr_le_u16(actor + 0u, rd_le_u16(rec + 0u));
    wr_le_u16(actor + 2u, (uint16_t)(rd_le_u16(rec + 2u) - 8u));
    wr_le_u16(actor + 4u, 0x0103u);
    wr_le_u16(actor + 8u, 0x001Eu);
    wr_le_u16(actor + 0xAu, 6u);
    wr_le_u16(actor + 0xCu, 0u);
    wr_le_u16(actor + 0xEu, 0x000Au);
}


/**
 * @brief 1000:DC1F (type 15): probe the linked record and carry or give back.
 *
 * @param actor the object record
 */
static void _obj_command_type15_tail(uint8_t *actor)
{
    uint8_t *node = _obj_core_link_record(rd_le_u16(actor + 0xEu));
    int32_t ax, bx;
    uint8_t phase;

    if (!node) return; // 1000:DC1F (DI = the node)
    ax = (int32_t)rd_le_u16(node + 0u) + 8; // 1000:DC32
    bx = (int32_t)rd_le_u16(node + 2u) + 0x10; // 1000:DC34
    phase = _obj_frame & 0x3Fu; // 1000:DC3D -> [0x4230]
    if (rd_le_u16(node + 8u) != 0u) { // 1000:DC22
        for (uint16_t i = 0; i < 8u; i++) { // 1000:DC3A: CX = 7
            uint8_t *r = object_core_slot(i);
            int32_t d;

            if (!r || r == actor) continue; // 1000:DC4B
            d = (int32_t)(int16_t)(uint16_t)(rd_le_u16(r + 2u) + 0x18u) - bx;
            if (d < 0) d = -d;
            if (d > 8) continue; // 1000:DC72
            d = (int32_t)(int16_t)rd_le_u16(r + 0u) - ax;
            if (d < 0) d = -d;
            if (d > 1) continue; // 1000:DC83
            if (rd_le_u16(r + 8u) > 2u) continue; // 1000:DC8A
            if ((int32_t)i == object_core_player()) { // 1000:DC91
                _obj_command_type15_give(actor);
                return;
            }
            _obj_command_carry_form(r, phase); // 1000:DC9A
            return;
        }
    }
    for (uint16_t g = 0; g < 8u; g++) { // 1000:DCD0
        uint8_t *e = (uint8_t *)_obj_register[g];
        int32_t d;

        if (rd_le_u16(e + 0u) & 0x8000u) continue; // 1000:DCEC
        d = (int32_t)(int16_t)(uint16_t)(rd_le_u16(e + 2u) + 0x18u) - bx;
        if (d < 0) d = -d;
        if (d > 8) continue;
        d = (int32_t)(int16_t)(uint16_t)(rd_le_u16(e + 0u) + 8u) - ax;
        if (d < 0) d = -d;
        if (d > 1) continue; // 1000:DD0F
        if ((uint32_t)e[5] == (uint32_t)object_core_player()) { // 1000:DD27
            _obj_command_type15_give(actor);
            return;
        }
        {
            uint8_t *placed = object_core_slot(e[5]);

            if (!placed) continue;
            object_units_group_dissolve_at(g); // 1000:DD2E
            _obj_command_carry_form(placed,
                                    phase); // 1000:DC9A with BP = the record
        }
        return;
    }
    return;
}


/** @brief 1000:EA0F / 1000:EA4D: the two "ride" commands.  Both require the record's
 * behaviour state to be 2 (1000:F2CF) and the acting entity not to be in a
 * stack (1000:EA33); they then set a frame (0xAB / 0xAD) with a three-frame
 * hold and hand 1000:FC81 the x offset and the bit argument.
 *
 * @param actor the object record
 * @param frame the frame index
 * @param xoff the x offset
 * @param bit the bit index
 */
void _obj_command_ride(uint8_t *actor, uint32_t frame, uint32_t xoff,
                       uint32_t bit)
{
    if (!_obj_sequence_state_is(actor, 2u)) return; // 1000:EA15
    if (_obj_units_stack_masked()) return; // 1000:EA1A
    wr_le_u16(actor + 4u, (uint16_t)frame);
    wr_le_u16(actor + 0xAu, 3u);
    _obj_sel_type = (uint16_t)(bit); // 1000:EA29 / 1000:EA67
    _obj_core_scan_point(actor, xoff); // 1000:FC81
}


/** @brief 1000:ED4F / 1000:ED69 / 1000:ED83 / 1000:ED9D: the four one-step animation
 * nudges.  Each waits on the 25-frame cadence (1000:F9E4) and then moves the
 * frame one step toward its bound unless it is already there.  With [0x6F1D]
 * set, the handler is the enter-the-building sequence (1000:EE3B) instead.
 *
 * @param actor the object record
 * @param bound the bound value
 * @param up the up flag
 */
static void _obj_command_frame_step(uint8_t *actor, uint32_t bound, int32_t up)
{
    uint16_t f;

    if (_obj_sequence_active) {
        _obj_command_enter(actor);
        return;
    } // 1000:ED56
    if (_obj_command_frame_cadence(actor)) return; // 1000:ED5C
    f = rd_le_u16(actor + 4u);
    if (f == bound) return;
    wr_le_u16(actor + 4u, (uint16_t)(up ? f + 1u : f - 1u));
}


/* Command: return the record to its home frame/type. */
int32_t _obj_command_home_state(uint8_t *actor)
{
    uint16_t t = rd_le_u16(actor + 8u);

    if (t == 0x12u || t == 0x17u || t == 0x1Fu) {
        wr_le_u16(actor + 4u, 0u); // 1000:EDD9
        wr_le_u16(actor + 8u, 0u);
    } else if (t == 0x14u || t == 0x18u || t == 0x20u) {
        wr_le_u16(actor + 4u, 8u); // 1000:EDE5
        wr_le_u16(actor + 8u, 1u);
    } else {
        return 0; // 1000:EDD8
    }
    wr_le_u16(actor + 0xAu, 0u); // 1000:EDEF
    wr_le_u16(actor + 0xCu, 0u);
    wr_le_u16(actor + 0xEu, 0u);
    wr_le_u16(actor + 0x10u, 0u);
    _obj_sequence_active = 0; // 1000:EDFD
    object_core_set_action_latched(1); // 1000:EE01
    return 1; // AX = 0: converted
}


/** @brief 1000:EE07 (and 0xEFD1 / 0xEFD4, its jump-table aliases): advance the record's
 * direction pointer while the ACTION press is not being consumed.  The first
 * pass latches [0x6F1D] and clears the pointer; `+0xE` then walks 0..0x1FF in
 * steps of four (a 128-entry circle of four-word vectors).  `[0x6916]` becomes 4
 * unless the previous dispatch code was already 4 (the debounce byte 0x197A),
 * and 0x1987 is armed.
 *
 * @param actor the object record
 */
static void _obj_command_turn(uint8_t *actor)
{
    /* 1000:EE0C.  The live charge loop has [0x13A4] clear; the port also has
     * to let the first post-op4 frame through when the ACTION latch is still
     * up. */
    if (object_core_action_latched() && !_obj_charging) return;
    if (!_obj_sequence_active) {
        _obj_sequence_active = 0xFF; // 1000:EE16
        wr_le_u16(actor + 0xEu, 0u);
    }
    wr_le_u16(actor + 0xEu,
              (uint16_t)((rd_le_u16(actor + 0xEu) + 4u) & 0x1FFu));
    if (_obj_last_result != 4u) _obj_result_pending = 4; // 1000:EE30
    _obj_charge_arm = 0xFF; // 1000:EE35
}


/** @brief 1000:EFE4: the *leave* sequence - what 1000:EFD7 jumps to when the latch is
 * set (the release the type-23/24 rows' idle command runs).  It releases the
 * latch, derives the animation index from the frozen direction counter
 * (`+0xE`, folded at 0x1FF for values >= 0x100 - `SUB CX,0x1FF` / `NEG CX` at
 * 1000:F013 - and divided by 16), sets a three-frame hold, bumps the type by 2
 * and the frame by 1, nudges the record 4 px right and 4 px UP for frame 0xBB
 * (`ADD word [SI],4` / `SUB word [SI+2],4` at 1000:F03A/F03C), else 4 px left
 * and 4 px up, and runs the fall entry the `DI` switch selects (`0xF067` for
 * frame 0xBB, otherwise `0xF078`, i.e. the (0/0) or (8/1) landing pair).  When
 * the record did not land (`+0x10` still set) the nudged position stays;
 * otherwise the position is restored and the landing pair is written back.
 *
 * @param actor the object record
 */
static void _obj_command_leave(uint8_t *actor)
{
    uint16_t cx = rd_le_u16(actor + 0xEu);
    int32_t ax = 4, bx = 2;
    uint8_t down = 1;

    _obj_sequence_active = 0; // 1000:EFE4
    if (rd_le_u16(actor + 4u) != 0x00BBu) { // 1000:EFF5
        ax = -4;
        bx = -2;
        down = 0;
    }
    wr_le_u16(actor + 0x10u, (uint16_t)(int16_t)bx); // 1000:F005
    wr_le_u16(actor + 0xEu, 0u);
    if (cx >= 0x100u) cx = 0x1FFu - cx; // 1000:F00D
    cx >>= 4; // 1000:F019
    wr_le_u16(actor + 0xCu, (uint16_t)(0xFu - cx)); // 1000:F021
    wr_le_u16(actor + 0xAu, 3u); // 1000:F029
    wr_le_u16(actor + 8u,
              (uint16_t)(rd_le_u16(actor + 8u) + 2u)); // 1000:F02E
    wr_le_u16(actor + 4u,
              (uint16_t)(rd_le_u16(actor + 4u) + 1u)); // 1000:F032
    {
        uint16_t sx = rd_le_u16(actor + 0u), sy = rd_le_u16(actor + 2u);

        wr_le_u16(actor + 0u, (uint16_t)(sx + (uint16_t)(int16_t)ax));
        wr_le_u16(actor + 2u, (uint16_t)(sy - 4u)); // 1000:F03C
        _obj_core_driver_call(2); // 1000:F041
        _obj_command_fall(actor, down ? 0 : 8, down ? 0 : 1, down ? 0x10u : 0u);
        if (rd_le_u16(actor + 0x10u) == 0u) { // 1000:F04A
            wr_le_u16(actor + 0u, sx); // 1000:F051: restore
            wr_le_u16(actor + 2u, sy);
            wr_le_u16(actor + 4u, (uint16_t)_obj_side); // 1000:F056: frame
            wr_le_u16(actor + 8u, _obj_sel_xoff); // 1000:F05C: type
        }
    }
}


/** @brief 1000:EFD7: release the record's direction pointer while the latch is clear
 * (otherwise the enter sequence runs).
 *
 * @param actor the object record
 */
static void _obj_command_turn_idle(uint8_t *actor)
{
    if (_obj_sequence_active) {
        _obj_command_leave(actor);
        return;
    } // 1000:EFDC -> 0xEFE4
    wr_le_u16(actor + 0xEu, 0u);
}


/* Command: enter the fall machine with facing/target/frame. */
static void _obj_command_fall(uint8_t *actor, int32_t x, int32_t y,
                              uint32_t frame)
{
    _obj_side = (uint16_t)(int16_t)x;
    _obj_sel_xoff = (uint16_t)y;
    _obj_sel_frame = (uint16_t)(frame);
    _obj_units_fall_machine(actor);
}


/** @brief 1000:F08D / 1000:F0A2: the two landing placements - a frame/type pair and a
 * scratch point, then 1000:F23A.
 *
 * @param actor the object record
 * @param x the x coordinate
 * @param y the y coordinate
 * @param frame the frame index
 */
static void _obj_command_place(uint8_t *actor, int32_t x, int32_t y,
                               uint32_t frame)
{
    _obj_side = (int16_t)x; // 1000:F08D
    _obj_sel_xoff = (uint16_t)y;
    _obj_sel_frame = (uint16_t)(frame);
    _obj_core_place_at(actor); // 1000:F23A
}


/**
 * @brief 1000:F0B3: the "stand" form - frame 0x10, type 2, a one-frame hold.
 *
 * @param actor the object record
 */
static void _obj_command_stand(uint8_t *actor)
{
    wr_le_u16(actor + 4u, 0x0010u);
    wr_le_u16(actor + 8u, 0x0002u);
    wr_le_u16(actor + 0xAu, 0x0001u);
}


/** @brief 1000:E7C0 stream_script_bytes_timed.  The dispatch's one-shot [0x6DEA]
 * gates it; when the hold 0x6DEC expires, fetch a frame/hold pair, set the
 * actor's +4 frame and the next hold, and wrap at 0xFF.
 *
 * @param actor the object record
 */
static void _obj_command_stream_timed(uint8_t *actor)
{
    const uint8_t *p;

    if (_obj_cmd_timer != 0u) return;
    if (_obj_stream_count != 0u) {
        _obj_stream_count--;
        return;
    }
    _obj_stream_count = 0u;
    p = _obj_stream_ptr;
    wr_le_u16(actor + 4u, p[0]); // MOV [SI+4], AX
    _obj_stream_count = p[1]; // [0x6DEC]
    p += 2;
    if (*p == 0xFFu) p = _obj_anim_stream; // 1000:E7FA
    _obj_stream_ptr = p; // [0x6DEE]
}


/**
 * @brief 1000:EBCA: type 15 command 8 clears the record's +0xA hold.
 *
 * @param actor the object record
 */
static void _obj_command_clear_countdown(uint8_t *actor)
{ wr_le_u16(actor + 0xAu, 0u); }


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_stream_command(uint8_t *actor)
{ _obj_command_stream_timed(actor); }
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_clear_countdown(uint8_t *actor)
{ _obj_command_clear_countdown(actor); }
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_set_command_timer(uint32_t v)
{ _obj_cmd_timer = (uint16_t)v; }
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief Dispatch one command handler address.
 *
 * @param actor the object record
 * @param addr the handler address
 * @return 1 when the address was handled, 0 otherwise
 */
static int32_t _obj_command_call(uint8_t *actor, uint16_t addr)
{
    switch (addr) {
        case 0xD9C7u: return 1; // the common tail: a bare RET
        case 0xE4F8u: (void)object_command_step_down(actor); return 1;
        case 0xE609u: (void)object_command_step_up(actor); return 1;
        case 0xE72Au: _obj_command_idle_a(actor); return 1;
        case 0xE790u: _obj_command_idle_b(actor); return 1;
        case 0xE860u: (void)object_units_stack_step_off(actor); return 1;
        case 0xE8DDu: (void)object_units_stack_leave(actor); return 1;
        case 0xE963u: (void)object_units_stack_snap(actor); return 1;
        case 0xE99Du: (void)object_units_stack_stand(actor); return 1;
        case 0xE93Du: (void)object_command_selector_walk(actor, 0); return 1;
        case 0xE952u: (void)object_command_selector_walk(actor, 1); return 1;
        /* the rest of the rows' handlers (see the block above) */
        case 0xE6FBu: _obj_command_walk(actor, 1); return 1;
        case 0xE764u: _obj_command_walk(actor, 0); return 1;
        case 0xEB78u: _obj_command_path_step(actor, 0); return 1;
        case 0xEBA0u: _obj_command_path_step(actor, 1); return 1;
        case 0xEBD0u: _obj_command_path_advance(actor); return 1;
        case 0xDC1Fu:
            _obj_command_type15_tail(actor);
            return 1; // the type-15 rows' tail
        /* the rows' tails (the +0xC word): 0xD9C7 is the bare RET above */
        case 0xD9C8u: _obj_units_tail_frame_bound(actor, 0x112u); return 1;
        case 0xD9CDu: _obj_units_tail_frame_bound(actor, 0x116u); return 1;
        case 0xD9E8u: _obj_units_tail_type3(actor); return 1;
        case 0xDB48u:
            _obj_units_tail_span_owner(actor, 0u, 0u, 0xABu);
            return 1;
        case 0xDB61u:
            _obj_units_tail_span_owner(actor, 8u, 1u, 0xADu);
            return 1;
        case 0xDD93u: _obj_units_tail_type16(actor); return 1;
        case 0xDDDAu: _obj_units_tail_type29(actor); return 1;
        case 0xE802u:
            _obj_units_tail_special_tile(actor, 4u, 0x16, 0);
            return 1;
        case 0xE831u:
            _obj_units_tail_special_tile(actor, 5u, 0x18, 1);
            return 1;
        case 0xEAACu: _obj_units_tail_type11(actor); return 1;
        case 0xEAE6u: _obj_units_tail_type12(actor); return 1;
        case 0xEB5Cu: _obj_units_tail_walk_countdown(actor, 1); return 1;
        case 0xEB6Au: _obj_units_tail_walk_countdown(actor, 0); return 1;
        case 0xECB7u: _obj_units_tail_type17(actor); return 1;
        case 0xEF0Au: return 1; // a bare RET
        case 0xEF0Bu: return 1; // a bare RET
        case 0xF067u: _obj_command_fall(actor, 0, 0, 0x10u); return 1;
        case 0xF078u: _obj_command_fall(actor, 8, 1, 0u); return 1;
        case 0xF0C9u: _obj_units_tail_type33(actor); return 1;
        case 0xDBCDu: _obj_units_tail_type28(actor); return 1;
        case 0xDE7Eu: _obj_units_tail_type30(actor); return 1;
        case 0xDED1u: _obj_units_tail_type34(actor); return 1;
        case 0xEF66u: _obj_units_tail_type22(actor); return 1;
        case 0xEA0Fu: _obj_command_ride(actor, 0xABu, 0xCu, 9u); return 1;
        case 0xEA4Du: _obj_command_ride(actor, 0xADu, 2u, 0xAu); return 1;
        case 0xEC99u:
            _obj_command_walk(actor, 0);
            _obj_command_neighbour_pass(actor, 0xDu, 0xCu, 0xA);
            return 1;
        case 0xECA8u:
            _obj_command_walk(actor, 1);
            _obj_command_neighbour_pass(actor, 3u, 0xCu, -0xA);
            return 1;
        case 0xED4Fu: _obj_command_frame_step(actor, 0x8Eu, 1); return 1;
        case 0xED69u: _obj_command_frame_step(actor, 0x8Cu, 0); return 1;
        case 0xED83u: _obj_command_frame_step(actor, 0x91u, 0); return 1;
        case 0xED9Du: _obj_command_frame_step(actor, 0x93u, 1); return 1;
        case 0xEDB7u: _obj_command_home_state(actor); return 1;
        case 0xEE07u: _obj_command_turn(actor); return 1;
        case 0xEE3Bu: _obj_command_enter(actor); return 1;
        case 0xEFD1u: _obj_command_home_state(actor); return 1; // JMP 0xEDB7
        case 0xEFD4u: _obj_command_turn(actor); return 1; // JMP 0xEE07
        case 0xEFD7u: _obj_command_turn_idle(actor); return 1;
        case 0xF08Du: _obj_command_place(actor, -8, 8, 1u); return 1;
        case 0xF0A2u: _obj_command_place(actor, 0x18, 0, 0u); return 1;
        case 0xF0B3u: _obj_command_stand(actor); return 1;
        case 0xF0C3u: _obj_command_home_state(actor); return 1;
        case 0xF0C6u: _obj_command_home_state(actor); return 1;
        case 0xE7C0u: _obj_command_stream_timed(actor); return 1;
        case 0xEBCAu: _obj_command_clear_countdown(actor); return 1;
        case 0xEA0Eu: return 1; // bare RET
        default: return 0;
    }
}


int32_t object_command_run(uint8_t *actor, uint8_t input)
{
    ObjCommand c;
    uint16_t before;
    int32_t ran = OBJ_CMD_NONE;
    uint8_t skip = 0;
    uint8_t dispatch = input;

    /* 1000:3702 reuses the press frame's [0x5ACE] for every update_world of
     * the blocking charge loop; the port's per-frame model still polls the
     * keyboard, so keep the press directions and the live fire bit (the
     * release is what ends the loop). */
    if (_obj_charging)
        dispatch = (uint8_t)((input & JOYBITS_FIRE) |
                             (_obj_charge_joy & JOYBITS_DIRS));

    /* 1000:E447: the output state [0x6916]/[0x6917] is per-call, so a queued
     * code cannot re-arm itself on every later frame (which would reset the
     * ACTION cursor through 1000:382D). */
    _obj_result_pending = 0u;
    _obj_result_cursor = 0u;
    c = object_command_select(actor, dispatch);
    before = rd_le_u16(actor + 8u);

    if (c.handler) {
        if (!_obj_command_call(actor, c.handler)) return OBJ_CMD_UNPORTED;
        ran = OBJ_CMD_HANDLER;
        if (rd_le_u16(actor + 8u) != before) skip = 1; // 1000:E4CC
    }
    if (!skip) {
        if (c.hook) _obj_command_settle(actor); // 1000:D8BD
        if (c.tail) {
            if (!_obj_command_call(actor, c.tail)) return OBJ_CMD_UNPORTED;
            ran = OBJ_CMD_TAIL;
        }
    }
    if (_obj_result_pending != 0u) { // 1000:E4E4
        _obj_pending = _obj_result_pending;
        _obj_cursor = _obj_result_cursor;
        /* The turn that queues event 4 is the press frame: latch its word
         * for the loop's later update_world calls. */
        if (_obj_result_pending == 4u && !_obj_charging)
            _obj_charge_joy = dispatch;
    }
    return ran;
}


void object_command_row_pass(void)
{
    uint8_t *actor = _obj_core_actor();

    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        const uint8_t *row;
        uint32_t type, tail;

        if (r == actor) continue; // 1000:D86D
        if (rd_le_u16(r + 0u) == 0xFFFFu) continue; // 1000:D873
        type = rd_le_u16(r + 8u);
        if (type >= 35u) continue; // the original reads past 0x698A
        row = _obj_cmd_rows[type];
        if (row[0xAu]) _obj_command_settle(r); // 1000:D89F
        tail = rd_le_u16(row + 0xCu);
        if (tail) (void)_obj_command_call(r, (uint16_t)(tail)); // 1000:D8A8
    }
}


int32_t object_command_step_down(uint8_t *actor)
{
    int32_t x = (int32_t)(int16_t)rd_le_u16(actor + 0u);
    int32_t y = (int32_t)(int16_t)rd_le_u16(actor + 2u);
    uint32_t sub, cell = _obj_core_map_at_abs(x + 8, y + 0x16, &sub);

    if (cell == level_special_tile(0u) || cell == level_special_tile(1u)) {
        wr_le_u16(actor + 0u, (uint16_t)((x + 8) & 0xFFF0));
        wr_le_u16(actor + 2u, (uint16_t)(y - 1));
        wr_le_u16(actor + 4u, 0x0023u);
        wr_le_u16(actor + 8u, 0x0004u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        return 1;
    }
    for (uint16_t i = 0; i < 2u; i++) {
        uint16_t *s = _obj_span[i];
        int32_t sx = (int16_t)s[1], sy = (int16_t)s[2], sh = (int16_t)s[3], d;

        if (rd_le_u16((uint8_t *)s + 0u) & 0x8000u) continue;
        d = sx - (x + 8);
        if (d < 0) d = -d;
        if (d >= 8) continue; // |span.x - (x+8)| < 8
        d = sy - y;
        if (d >= 0 && d >= 0x18) continue; // span.y - y < 0x18
        if (sy + sh < y) continue; // the span covers y
        wr_le_u16(actor + 2u, (uint16_t)(y - 1));
        wr_le_u16(actor + 0u, (uint16_t)(sx - 8));
        wr_le_u16(actor + 4u, 0x0052u);
        wr_le_u16(actor + 8u, 0x000Bu);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        wr_le_u16(actor + 0xEu,
                  (uint16_t)((uintptr_t)s - (uintptr_t)_obj_span));
        _obj_units_stack_unmark(actor); // 1000:F3CA
        return 2;
    }
    {
        int32_t r = object_units_stack_grab(actor);

        return r == 1 ? 3 : (r == 0 ? 4 : -1);
    }
}


int32_t object_command_step_up(uint8_t *actor)
{
    int32_t x = (int32_t)(int16_t)rd_le_u16(actor + 0u);
    int32_t y = (int32_t)(int16_t)rd_le_u16(actor + 2u);
    uint32_t sub, cell = _obj_core_map_at_abs(x + 8, y + 0x19, &sub);

    if (cell == level_special_tile(0u) || cell == level_special_tile(1u)) {
        wr_le_u16(actor + 0u, (uint16_t)((x + 8) & 0xFFF0));
        wr_le_u16(actor + 2u, (uint16_t)(y + 1));
        wr_le_u16(actor + 4u, 0x0025u);
        wr_le_u16(actor + 8u, 0x0005u);
        wr_le_u16(actor + 0xAu, 0x0000u);
        wr_le_u16(actor + 0xCu, 0x0000u);
        return 1;
    }
    for (uint16_t i = 0; i < 2u; i++) {
        uint16_t *s = _obj_span[i];
        int32_t sx = (int16_t)s[1], sy = (int16_t)s[2], sh = (int16_t)s[3], d;
        int32_t line = y + 0x18;

        if (rd_le_u16((uint8_t *)s + 0u) & 0x8000u) continue;
        d = sx - (x + 8);
        if (d < 0) d = -d;
        if (d >= 0x10) continue; // |span.x - (x+8)| < 0x10
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
        _obj_units_stack_unmark(actor);
        return 2;
    }
    return object_units_stack_push(actor) ? 3 : 4;
}


static int32_t object_command_selector_walk(uint8_t *actor, int32_t right)
{
    return right ? object_command_selector(actor, 1u, 0u, 0u)
                 : object_command_selector(actor, 0xFFFFu, 8u, 1u);
}


int32_t object_command_selector(uint8_t *actor, uint16_t xoff, uint16_t frame,
                                uint16_t type)
{
    uint16_t ax;
    uint8_t *below, *entry;
    int32_t r;

    _obj_sel_xoff = xoff;
    _obj_sel_frame = frame;
    _obj_sel_type = type;
    ax = (uint16_t)(_obj_sel_xoff + rd_le_u16(actor + 0u)); // 1000:F185
    r = object_units_stack_pop(ax, &below, &entry);
    if (r < 0) return -1; // 1000:F18D
    if (r == 0) {
        if (rd_le_u16(actor + 4u) != 0x20u) { // 1000:F1F2
            int32_t d = (int32_t)(int16_t)rd_le_u16(actor + 4u) -
                        (int32_t)(int16_t)_obj_sel_frame;

            if (d < 0 || d >= 8)
                wr_le_u16(actor + 4u, _obj_sel_frame); // 1000:F20C
            _obj_command_walk(actor, _obj_sel_type != 0u); // 1000:E6FB / 0xE764
            return 0;
        }
        wr_le_u16(actor + 4u, _obj_sel_frame); // 1000:F191
        return 0;
    }
    if (below) { // 1000:F198
        wr_le_u16(below + 2u, (uint16_t)(rd_le_u16(below + 2u) - 8u));
        wr_le_u16(below + 4u, 0x0020u);
    }
    wr_le_u16(actor + 4u, frame);
    wr_le_u16(actor + 8u, type);
    /* 1000:F1B2 also writes [0x6FE6] = 0xFF; that byte has no decoded reader. */
    {
        uint32_t bit =
            _obj_units_index_bit((uint32_t)(actor - _obj_array) / OBJ_STRIDE);

        if ((_obj_ter_slot[0][0x13] | _obj_ter_slot[1][0x13]) & bit)
            return 0; // 1000:F1CB
    }
    /* 1000:F1D7: the step probe's distance test.  A blocked probe, or a step
     * of 10 px or more, goes to 1000:F223; a small step does nothing. */
    {
        uint32_t cell = 0xFFu;
        int32_t bx = _obj_core_step_probe(actor, &cell), d;

        if (bx >= 0) {
            d = bx - 0x18 - (int32_t)(int16_t)rd_le_u16(actor + 2u);
            if (d < 0) d = -d;
            if (d - 0xA < 0) return 0; // 1000:F1F1
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


/** @brief 1000:D6B2 phases 1/2: walk the pending-move table, raising a record's
 * +4 tag 0x21 -> 0x22 for 0x28 ticks, then putting it back.
 */
static void _obj_command_pending_moves_tick(void)
{
    uint16_t placed = object_core_placed_humans();

    if (_obj_anim_move_tag == 0u) {
        uint8_t div = _obj_anim_move_div;
        _obj_anim_move_div = (uint8_t)(div - 1u);
        if (div > 0u) return;
        if (_obj_anim_move_cursor / 4u < 17u && placed != 0u) {
            uint16_t idx = _obj_anim_move_index +
                           _obj_anim_move_off[_obj_anim_move_cursor / 4u];

            while (idx >= placed) idx -= placed;
            _obj_anim_move_div =
                (uint8_t)(_obj_anim_move_wait[_obj_anim_move_cursor / 4u]);
            _obj_anim_move_index = idx;
            _obj_anim_move_cursor = (uint16_t)(_obj_anim_move_cursor + 4u);
            if (_obj_anim_move_off[_obj_anim_move_cursor / 4u] == 0xFFFFu)
                _obj_anim_move_cursor = 0u;
            {
                uint8_t *r = object_core_slot(idx);

                if (r && rd_le_u16(r + 4u) == 0x21u) {
                    wr_le_u16(r + 4u, 0x22u);
                    _obj_anim_move_tag = 1u;
                    _obj_anim_move_count = 0x28u;
                    _obj_anim_move_slot = idx;
                }
            }
        }
    } else if (_obj_anim_move_count-- == 0u) {
        uint8_t *r = object_core_slot(_obj_anim_move_slot);

        if (r && rd_le_u16(r + 4u) == 0x22u) wr_le_u16(r + 4u, 0x21u);
        _obj_anim_move_tag = 0u;
        _obj_anim_move_count = 0u;
    }
}


/* --- 1000:D6B2 phase 3: the group-member animator ---------------------- */
void object_command_animator_tick(void)
{
    _obj_command_pending_moves_tick();

    if (_obj_anim_div == 0u) _obj_anim_div = 6;
    else _obj_anim_div = (uint8_t)(_obj_anim_div - 1u);

    for (uint16_t gi = 0; gi < 8u; gi++) {
        uint8_t *g = (uint8_t *)_obj_register[gi];
        uint8_t len = 0, limit, c;
        uint8_t dl;

        if (rd_le_u16(g + 0u) & 0x8000u) continue;
        while ((g[5u + len] & 0x80u) == 0) len++;
        if (len < 3u || len > 8u) continue;
        limit = (uint8_t)(2u * (len - 1u));
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
        for (uint16_t m = 0; m < len; m++) {
            uint8_t *r = object_core_slot(g[5u + m]);
            uint16_t frame;

            if (!r) continue;
            frame = rd_le_u16(r + 4u);
            if (frame < 0x20u || frame > 0x22u) continue;
            wr_le_u16(
                r + 0u,
                (uint16_t)(rd_le_u16(g + 0u) +
                           (uint16_t)_obj_anim_x[len - 3u][c][len - 1u - m]));
        }
        g[4] = (uint8_t)(dl | (c & 0x0Fu)); // 1000:D813 OR DL,CL
    }
}


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t *object_command_group_at(uint32_t i)
{ return i < 8u ? (uint8_t *)_obj_register[i] : NULL; }
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_command_animator_reset(void)
{
    _obj_anim_div = 0;
    _obj_anim_move_tag = _obj_anim_move_count = _obj_anim_move_slot =
        _obj_anim_move_index = _obj_anim_move_cursor = 0u;
    _obj_anim_move_div = 0;
}
#endif /* HUMANSRE_TEST_PROBES */
