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
 * (`object_loadTerrainObjects`, all 25 tiles), their sprites come from the
 * storage law derived from `draw_object_scaled_x1` (`1000:66EC`: four plane
 * passes of 4 bytes per row, `block = 4h`, see `blitSprite`), and the claim
 * (`object_claimBegin`/`object_claimTick`) runs the whole flash/blink/animate/
 * append/settle sequence.  `[0x1AF2]` is the claimed record, `[0x180A]` the
 * player's own (`resolve_object_sprite` `1000:4147` gives it the program index).
 *
 * The ACTION machine (`script_op_dispatch` 1000:3979, `object_actionRun`) is
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
 * hold), and `1000:D6B2`'s animator: phases 1/2 (`objPendingMovesTick`, the
 * 0x67FF pending-move table) and phase 3 (`object_animatorTick`, the 0x6F22
 * groups and the 0x66B3/0x66B5/objAnimX tables).  The class/behaviour
 * overlays of `1000:653C` and all six `1000:35D6` goal-test handlers are
 * ported.  The bare-RET slot `1000:EA0E` is in the table (type 16 command 8).
 */
#include "game.h"
#include <string.h>

#define OBJ_STATIC 8u                    /* the records 0x3640/check_highscore scan */
/* 0x3E75..0x407F: the array's initialiser (`1000:4CAB`) clears 29 records. */
#define OBJ_SLOTS  29u
#define OBJ_STRIDE 0x12u
/* `load_terrain_b` (`1000:516F`) sets [0x3E73] to 8; its per-tile walk appends
 * one live record per placed object (`0x3F17 + 0x12*([0x3E73]-8)`), so the
 * count is 8 + the walk's appends (level 1: one 0xAB cell = the spear). */
#define OBJ_LIVE0  8u

static uint8_t objArray[OBJ_SLOTS * OBJ_STRIDE];
static int objCurrent = -1;              /* [0x1AF2], -1 = none */
static int objPlayer = -1;               /* [0x180A], -1 = none */
static unsigned objLiveCount = OBJ_LIVE0;/* [0x3E73]: the array's used slots     */
static unsigned objPlacedHumans;         /* [0x3E20]: the 0xAA handlers' count   */
static unsigned objEntity;               /* [0x3E22]: the acting record          */
static unsigned objProgramIndex = 1;     /* [0x5B56], 1-based                    */
static uint16_t objCmdTimer;            /* [0x6DEA]: the command one-shot */
static uint16_t objStreamCount;         /* [0x6DEC]: stream hold countdown */
static const uint8_t *objStreamPtr;     /* [0x6DEE]: the script stream    */
static uint8_t objStackMask[2];         /* 0x64AD / 0x64C1: in-a-stack bits */
/* DI at the handler/hook call (1000:E467): the byte offset of the type row's
 * selected half inside objCmdRows.  Set by object_commandSelect, read by the
 * hooks (1000:F3ED/0xF423 and 0xF6F6). */
static unsigned objRowOfs;
static int16_t objSide;                 /* [0x4230]: which side the settle stepped from */
static int objStepProbeAt(const uint8_t *actor, int dx, int dy, unsigned *cellOut);
static int objCmdHomeState(uint8_t *actor);     /* 1000:EDB7               */
static void objDriverCall(unsigned n);          /* 1000:83B7               */
static void objFrameStepDown(uint8_t *actor, unsigned lo, unsigned hi); /* 1000:FDBF */
static uint8_t *objLinkRecord(unsigned off);   /* the +0xE record link */
void object_terSlotInit(void);                 /* 1000:4CC3/4CC6 */
static void objBuildShade(unsigned frame, uint8_t *out);
/* 0x649A / 0x64A2 / 0x64AE / 0x64B6: the four corner-pair blocks the 0xAF..0xB2
 * tiles write at load ({x, y, x, y} followed by a zeroed 12-byte entry, or just
 * {x, y}).  No ported consumer reads them yet - they are the level's "pair"
 * markers, and the write is all the original does. */
static uint16_t objPairA[8], objPairB[2], objPairC[8], objPairD[2];
/* 0x32B9: the 0xA9 background-sprite records (12 bytes each), filled by that
 * tile handler (1000:5206); the settle's 1000:DF2C reads them. */
static struct {
    uint16_t x, y;
    uint8_t *dst;
} objBg[32];
static unsigned objBgCount;

/* [0x5B56] is shared with the status bar (its action row's group), so every
 * write goes through here. */
static void objSetProgramIndex(unsigned index)
{
    if (index < 1u || index > 6u) return;
    objProgramIndex = index;
}
static unsigned objPending;              /* [0x1979]                             */
static unsigned objCursor = 1;           /* [0x1988]                             */
static unsigned objFault;                /* op 7 was dispatched (see below)      */
static const OSystem *objOsys;           /* the fatal halt's route (osystem.h)   */
static uint16_t objRegister[8][7];       /* 0x6F22: 8 x 0xE (clear is 0x70 bytes at
                                          * 0x60F4; op 1 walks AX = 7..0), bit15 = free */
static uint16_t objSpan[2][14];          /* 0x6F92, 2 x 0x1C: {id, .. +6, +8, +0x12} */
static int objAnimDiv;                   /* [0x66B1]: the 6-tick divider */
/* 0x67F1's pending-move chain: tag, phase-2 countdown, slot, phase-1
 * divider/index/cursor into the 0x67FF (offset, divider) table. */
static uint16_t pendTag, pendCount, pendSlot, pendIndex, pendCursor;
static int pendDiv;
static const int16_t objAnimX[6][15][9] = {
    { /* length 3 */
        { -2,-1,0,0,0,0,0,0,-1 },
        { -1,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,1 },
        { 1,0,0,0,0,0,0,0,2 },
        { 2,1,0,0,0,0,0,0,3 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
    },
    { /* length 4 */
        { -3,-2,-1,0,0,0,0,0,-2 },
        { -2,-1,0,0,0,0,0,0,-1 },
        { -1,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,1 },
        { 1,0,0,0,0,0,0,0,2 },
        { 2,1,0,0,0,0,0,0,3 },
        { 3,2,1,0,0,0,0,0,4 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
    },
    { /* length 5 */
        { -4,-3,-2,-1,0,0,0,0,-3 },
        { -3,-2,-1,0,0,0,0,0,-2 },
        { -2,-1,0,0,0,0,0,0,-1 },
        { -1,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,1 },
        { 1,0,0,0,0,0,0,0,2 },
        { 2,1,0,0,0,0,0,0,3 },
        { 3,2,1,0,0,0,0,0,4 },
        { 4,3,2,1,0,0,0,0,5 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
    },
    { /* length 6 */
        { -5,-4,-3,-2,-1,0,0,0,-4 },
        { -4,-3,-2,-1,0,0,0,0,-3 },
        { -3,-2,-1,0,0,0,0,0,-2 },
        { -2,-1,0,0,0,0,0,0,-1 },
        { -1,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,1 },
        { 1,0,0,0,0,0,0,0,2 },
        { 2,1,0,0,0,0,0,0,3 },
        { 3,2,1,0,0,0,0,0,4 },
        { 4,3,2,1,0,0,0,0,5 },
        { 5,4,3,2,1,0,0,0,6 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
    },
    { /* length 7 */
        { -6,-5,-4,-3,-2,-1,0,0,-5 },
        { -5,-4,-3,-2,-1,0,0,0,-4 },
        { -4,-3,-2,-1,0,0,0,0,-3 },
        { -3,-2,-1,0,0,0,0,0,-2 },
        { -2,-1,0,0,0,0,0,0,-1 },
        { -1,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,1 },
        { 1,0,0,0,0,0,0,0,2 },
        { 2,1,0,0,0,0,0,0,3 },
        { 3,2,1,0,0,0,0,0,4 },
        { 4,3,2,1,0,0,0,0,5 },
        { 5,4,3,2,1,0,0,0,6 },
        { 6,5,4,3,2,1,0,0,7 },
        { 0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0 },
    },
    { /* length 8 */
        { -7,-6,-5,-4,-3,-2,-1,0,-6 },
        { -6,-5,-4,-3,-2,-1,0,0,-5 },
        { -5,-4,-3,-2,-1,0,0,0,-4 },
        { -4,-3,-2,-1,0,0,0,0,-3 },
        { -3,-2,-1,0,0,0,0,0,-2 },
        { -2,-1,0,0,0,0,0,0,-1 },
        { -1,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,1 },
        { 1,0,0,0,0,0,0,0,2 },
        { 2,1,0,0,0,0,0,0,3 },
        { 3,2,1,0,0,0,0,0,4 },
        { 4,3,2,1,0,0,0,0,5 },
        { 5,4,3,2,1,0,0,0,6 },
        { 6,5,4,3,2,1,0,0,7 },
        { 7,6,5,4,3,2,1,0,0 },
    },
};

/* 0x6DF2: the static two-byte (frame, hold) stream the type-2 command-8
 * handler walks; 0xFF 0xFF terminates and wraps. */
static const uint8_t objAnimStream[] = {
    0x10,0xC8, 0x10,0xC8, 0x12,0x05, 0x10,0x14, 0x12,0x05, 0x10,0xC8,
    0x10,0xC8, 0x13,0x06, 0x14,0x06, 0x15,0x06, 0x16,0x28, 0x15,0x06,
    0x14,0x06, 0x13,0x06, 0x10,0xC8, 0x12,0x05, 0x10,0x0A, 0x12,0x05,
    0x10,0x32, 0x13,0x06, 0x14,0x06, 0x15,0x06, 0x16,0x23, 0x15,0x06,
    0x14,0x06, 0x13,0x06, 0x10,0xC8, 0x10,0xC8, 0x12,0x05, 0x10,0x14,
    0x12,0x05, 0x10,0xC8, 0x10,0xC8, 0xFF,0xFF,
};


/* [0x3E5E + b] — the class byte of the object in source slot b (0 = none).
 * `pred_sprite_class_match` (1000:3659) reads it as `[0x3E5E + rec[+7]] & 0x3F`
 * and the claim frees the slot (`[0x3E5E + rec[7]] = 0`, 1000:3B1E); the spawn
 * writes the behaviour id at `[0x3E5F + n]`, i.e. slot (n+1). */
static uint8_t objSlotClass[0x40];

/* the acting record: get_object_ptr (1000:8DF4) = the 29-word table baked at
 * DS:0x3E24, indexed by [0x3E22] — i.e. objArray[objEntity]. */
static uint8_t *objActor(void) { return objArray + objEntity * OBJ_STRIDE; }

static uint16_t rd16(const uint8_t *r, unsigned off)
{
    return (uint16_t)(r[off] | ((uint16_t)r[off + 1u] << 8));
}

static void wr16(uint8_t *r, unsigned off, uint16_t v)
{
    r[off] = (uint8_t)(v & 0xFFu);
    r[off + 1u] = (uint8_t)(v >> 8);
}

/* The four shipped recipes (202F:0x19BA, 12 bytes each): [uses][behaviour]
 * then the sprite word and the four parameter words, re-armed to one use
 * every level start (level_setup 1000:31C3). */
static const uint8_t objRecipes[4][12] = {
    { 1, 5, 0x00, 0x60, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 },  /* op 11 -> behaviour 5 */
    { 1, 7, 0x00, 0x96, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 },  /* op 12 -> behaviour 7 */
    { 1, 6, 0x00, 0x9C, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 },  /* AX=2, never fired    */
    { 1, 4, 0x00, 0x38, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0 }, /* op 10 */
};
static uint8_t objUses[4];               /* the live copy of [DI+0] */

unsigned object_staticSlots(void) { return OBJ_STATIC; }

/* The array's initial state (`1000:4CAB`): every one of the 29 records gets
 * `x = y = 0xFFFF` and `+6 = 0x80` (flags bit 7 = free, class 0), the rest 0;
 * `[0x3E20]`/`[0x3E22]`/`[0x32B9]` are cleared and the recipes re-armed.
 * The live level confirms it: slots 4..7 read `x = y = 0xFFFF, flags = 0x80`. */
void object_init(void)
{
    memset(objArray, 0, sizeof objArray);
    for (unsigned i = 0; i < OBJ_SLOTS; i++) {
        uint8_t *r = objArray + i * OBJ_STRIDE;
        wr16(r, 0u, 0xFFFFu);
        wr16(r, 2u, 0xFFFFu);
        wr16(r, 6u, 0x0080u);
    }
    memset(objSlotClass, 0, sizeof objSlotClass);
    objCurrent = -1;
    objPlayer = -1;
    objLiveCount = OBJ_LIVE0;
    objPlacedHumans = 0u;
    objEntity = 0u;                  /* [0x3E22] cleared (1000:4CB7)      */
    objBgCount = 0u;                 /* [0x32B9] cleared (1000:4CBA)      */
    objPending = 0u;                 /* [0x1979]                          */
    objCursor = 1u;                  /* [0x1988]                          */
    objSetProgramIndex(1u);          /* [0x5B56]                          */
    objCmdTimer = 0u;                /* [0x6DEA]                          */
    objStreamCount = 0u;             /* [0x6DEC]                          */
    objStreamPtr = objAnimStream;    /* [0x6DEE] = 0x6DF2                 */
    objAnimDiv = 0;                  /* [0x66B1]                          */
    pendTag = pendCount = pendSlot = pendIndex = pendCursor = 0u;
    pendDiv = 0;
    objStackMask[0] = objStackMask[1] = 0u;
    objFault = 0u;
    memset(objRegister, 0xFF, sizeof objRegister);   /* 0x6F22, all free  */
    memset(objSpan, 0xFF, sizeof objSpan);   /* 0x6F92: 0x38 bytes at 0x60FF */
    for (unsigned i = 0; i < 4u; i++) objUses[i] = objRecipes[i][0];
    object_terSlotInit();           /* 1000:4CC3/4CC6: both slots off */
}

unsigned object_placedHumans(void) { return objPlacedHumans; }

uint8_t *object_slot(unsigned i)
{
    return i < OBJ_SLOTS ? objArray + i * OBJ_STRIDE : NULL;
}

unsigned object_slots(void) { return OBJ_SLOTS; }

unsigned object_liveCount(void) { return objLiveCount; }

uint8_t object_slotClass(unsigned slot)
{
    return slot < sizeof objSlotClass ? objSlotClass[slot] : 0u;
}

void object_setSlotClass(unsigned slot, unsigned cls)
{
    if (slot < sizeof objSlotClass) objSlotClass[slot] = (uint8_t)cls;
}

int object_current(void) { return objCurrent; }

void object_setCurrent(int slot)
{
    objCurrent = (slot >= 0 && (unsigned)slot < OBJ_SLOTS) ? slot : -1;
}

int object_player(void) { return objPlayer; }

void object_setPlayer(int slot)
{
    objPlayer = (slot >= 0 && (unsigned)slot < OBJ_SLOTS) ? slot : -1;
}

/* --- object_test_dispatch's predicates (1000:35D6) ---------------------
 *
 * The six handlers are reachable from the level's goal-test word at
 * [0x17F2] through the CS word table at 0x35F4.  All six are ported here
 * (0x3600 is object_lowPhaseMatch below); the earlier "[0x1996] byte" reading
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

/* 1000:3640 — -1 when NO record's type is 0x1D. */
int object_noneType1d(void)
{
    for (unsigned i = 0; i < OBJ_STATIC; i++)
        if (rd16(objArray + i * OBJ_STRIDE, 8u) == 0x1Du) return 0;
    return -1;
}

/* 1000:3659 — the current record's +7 indexes the source-slot table 0x3E5E;
 * a zero slot is false, and the slot's low six bits are compared with the
 * test's byte. */
int object_classMatches(unsigned want)
{
    unsigned b;

    b = objActor()[7u];
    if (!b) return 0;
    return ((objSlotClass[b] & 0x3Fu) == (uint8_t)want) ? -1 : 0;
}

/* 1000:3678 — the current record's type equals the test's word. */
int object_typeEquals(unsigned type)
{
    return rd16(objActor(), 8u) == type ? -1 : 0;
}

/* 0xF761's map addressing for a record: `cell = 32*((y+0xC)&0xFFF0)/16 +
 * ((x+8)>>4)` — the row is the record's y rounded down to the 16-px grid,
 * doubled into the 32-column row stride.  0xFFFF when it falls outside the
 * map (the original would read whatever follows it). */
/* The map buffer as 1000:3138 builds it: 640 bytes plus a second copy of the
 * first row at +0x20 (672 bytes).  1000:D9E8's `[DI-0x20]` / `[DI+0x20]` reads
 * can address the duplicate, so they go through this rather than the 640-byte
 * map directly; past 672 the original reads whatever follows the buffer and the
 * port returns 0. */
static unsigned objMapBufferByte(unsigned idx)
{
    const uint8_t *map = level_map();

    if (!map) return 0u;
    if (idx < 640u) return map[idx];
    if (idx < 672u) return map[idx - 640u];
    return 0u;
}

/* 1000:E361: the map cell at (x + 8, y + 0xC).  Returns the cell's *index* so
 * the callers that read a neighbour (`1000:3600`'s `[DI+0x20]`, one map row
 * below) can address it; 0xFFFF when the point is outside the map. */
static unsigned objMapCellIndex(const uint8_t *rec)
{
    unsigned x = (unsigned)rd16(rec, 0u) + 8u;
    unsigned y = (unsigned)rd16(rec, 2u) + 0xCu;
    unsigned cell;

    if (!level_map() || y < 16u) return 0xFFFFu;
    cell = 32u * (y / 16u) + (x / 16u);
    return cell < 640u ? cell : 0xFFFFu;
}

static unsigned objMapCell(const uint8_t *rec)
{
    const uint8_t *map = level_map();
    unsigned cell = objMapCellIndex(rec);

    return cell < 640u && map ? map[cell] : 0xFFFFu;
}

/* The map cell under a record, for the score (55FA calls 0xF761 with AX = 8,
 * BX = 0xC on each of the eight static records): 0xFFFF outside the map. */
unsigned object_recordMapCell(unsigned slot)
{
    if (slot >= OBJ_STATIC) return 0xFFFFu;
    return objMapCell(objArray + slot * OBJ_STRIDE);
}

/* 1000:36AB — the record's type is <= 2 and the map cell under it equals the
 * test's byte. */
int object_mapCellEquals(unsigned want)
{
    if (rd16(objActor(), 8u) > 2u) return 0;
    return objMapCell(objActor()) == (uint16_t)want
           ? -1 : 0;
}

/* 1000:3688 — the entity table (count 0x64C2, records 0x64C4, stride 0x10):
 * -1 when there are none or every one's +4 is 5 (the knocked/finished state),
 * 0 as soon as one is not. */
int object_entitiesFinished(void)
{
    unsigned n = entity_count();

    for (unsigned i = 0; i < n; i++) {
        const Entity *e = entity_at(i);
        if (e && e->handler != 5u) return 0;
    }
    return -1;
}

/* the frame counter the low-phase predicate gates on ([0x4103]) */
static unsigned objFrame;
static unsigned lowPhaseCount;      /* [0x1976]: 0x8C = 140 */

/* --- pred_type_low_phase_match (1000:3600) ----------------------------
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
int object_lowPhaseMatch(void)
{
    static unsigned lastFrame = ~0u;

    if (rd16(objActor(), 8u) > 2u) return 0;
    if (lastFrame == objFrame) return 0;            /* once per frame */
    lastFrame = objFrame;
    {
        /* 1000:361F..3629: the feet cell, then the cell one map row *below* it
         * (`[DI+0x20]`, which is `map[cell + 32]`).  Either being the goal tile
         * 0xA8 counts; the earlier reading of this as a `[0x1996]` byte was
         * wrong - see re_history.md. */
        const uint8_t *map = level_map();
        unsigned cell = objMapCellIndex(objActor());
        int goal = (cell < 640u && map && map[cell] == 0xA8u);

        /* The original's map buffer is 640 bytes followed by a copy of the first
         * row at +0x20 (1000:3138 copies it twice), so the last row's below-cell
         * wraps to row 0 rather than reading off the buffer. */
        if (!goal && cell < 640u && map) {
            unsigned below = cell + 32u;

            if (below >= 640u) below -= 640u;
            if (map[below] == 0xA8u) goal = 1;
        }
        if (!goal) {
            lowPhaseCount = 0u;                     /* 1000:362C */
            return 0;
        }
    }
    if (++lowPhaseCount != 0x8Cu) return 0;
    return -1;
}

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
 *   reveal  `init_dirty_cell_list` (`1000:3C70`) then `flush_dirty_cell_list`
 *           (0x534A) looping `queue_dirty_cell_packed` (0x53B3) until
 *           `[0x1B02]`: 0x100 dirty cells queued from the 0x1B76 coordinate
 *           table over the 0x48BC list, shaded from 0x19EA — ported as
 *           `level_sparkBegin`/`sparkStep`/`sparkDraw`
 *   hold    41 frames, then 1..0xBF -> white div 5, one frame, the neighbour's
 *           `+6 = 0xFFFF`, `[0xC] = [0x5B12] = 0xFF`, two page frames, then
 *           1..0xBF -> the level div 0x28
 */
enum {
    CLAIM_OFF,
    CLAIM_TO_RED,        /* 1000:3B56  1..0x7F -> red, div 5                */
    CLAIM_RED_BACK,      /* 1000:3B68  -> the level palette, div 0x1E       */
    CLAIM_TO_WHITE,      /* 1000:3B82  1..0x7F -> white, div 5              */
    CLAIM_PAN,           /* 1000:3B91  1000:3D04: the two-frame present    */
    CLAIM_WHITE_BACK,    /* 1000:3B97  -> the level palette, div 0x19       */
    CLAIM_FRAME,         /* 1000:3BA9  one 0x9F56 frame, then the blinks    */
    CLAIM_BLINK_WHITE,   /* 1000:3BB3  the 0x80 bank -> white, div n        */
    CLAIM_BLINK_LEVEL,   /* 1000:3BC3  the 0x80 bank -> the level, div n    */
    CLAIM_ANIM,          /* 1000:3BD6  6 x 9 frames, `frame++` per pass     */
    CLAIM_HOLD,          /* 1000:3C7D  41 frames                            */
    CLAIM_WHITE_OUT,     /* 1000:3C88  1..0xBF -> white, div 5              */
    CLAIM_FRAME2,        /* 1000:3C9F  one frame, then the remove           */
    CLAIM_FLIP,          /* 1000:3CB5  the two page frames                  */
    CLAIM_SETTLE         /* 1000:3CDE  1..0xBF -> the level, div 0x28       */
};

static int      claimPhase = CLAIM_OFF;
static int      claimNeighbour = -1;
static unsigned claimBehav, claimSprite;
static unsigned claimParams[4];
static unsigned claimBlinks, claimTimer;
static uint8_t  claimRed[768], claimWhite[768];

/* 0x54F4: the whole 768-byte ramp := one 6-bit colour.  The fill's register
 * order is (BL, AH, AL) = (R, G, B), hence the red call's AX = 0, BL = 0x3F. */
static void claimFillRamp(uint8_t *ramp, unsigned r, unsigned g, unsigned b)
{
    for (unsigned i = 0; i < 256u; i++) {
        ramp[i * 3u] = (uint8_t)r;
        ramp[i * 3u + 1u] = (uint8_t)g;
        ramp[i * 3u + 2u] = (uint8_t)b;
    }
}

/* The scan at 1000:3A91..1000:3AD9, for the acting record `actor`. */
int object_findNeighbour(int actor)
{
    const uint8_t *cur;
    int cx, cy;

    if (actor < 0 || (unsigned)actor >= OBJ_STATIC) return -1;
    cur = objArray + actor * OBJ_STRIDE;
    cx = (int16_t)rd16(cur, 0u);
    cy = (int16_t)rd16(cur, 2u);

    for (unsigned i = 0; i < OBJ_STATIC; i++) {
        const uint8_t *r = objArray + i * OBJ_STRIDE;
        int dx, dy;

        if ((int)i == actor) continue;               /* 1000:3A9C */
        if (rd16(r, 8u) > 2u) continue;              /* type <= 2 */
        dx = (int)(int16_t)rd16(r, 0u) - cx;
        dy = (int)(int16_t)rd16(r, 2u) - cy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 0x20 || dy > 8) continue;
        if (r[6] & 0x80u) continue;                  /* active    */
        return (int)i;
    }
    return -1;
}

int object_claimBegin(unsigned recipe, int actor)
{
    int n;
    uint8_t *rec, *live;
    unsigned cls;

    if (actor < 0 || (unsigned)actor >= OBJ_STATIC) return -1;
    if (recipe >= 4u) return -1;
    if (rd16(objArray + actor * OBJ_STRIDE, 8u) > 2u) return -1;
    if (!objUses[recipe]) return -1;                 /* 1000:3A89: the use counter */

    n = object_findNeighbour(actor);
    if (n < 0) return -1;

    rec = objArray + n * OBJ_STRIDE;
    cls = rec[7u];
    if (cls) {                                       /* 1000:3AE2..1000:3B00 */
        unsigned slot = cls - 1u;
        objSlotClass[cls] = 0u;                      /* 0x3E5F + slot */
        live = objArray + (OBJ_LIVE0 + slot) * OBJ_STRIDE;
        wr16(live, 6u, 0xFFFFu);                     /* 0x3F17 + 0x12*slot */
    }
    object_setCurrent(n);                            /* 1000:3B03 */
    wr16(rec, 4u, 0x0103u);                          /* 1000:3B07 */
    wr16(rec, 6u, 0u);                               /* 1000:3B0D */
    wr16(objArray + actor * OBJ_STRIDE, 4u, 0x0020u);/* 1000:3B13 */
    statusbar_setActive(0);                          /* 1000:3B23: [0x5B12] = 0 */
    /* 1000:3B19/3B28: the world freezes for the sequence, and the display
     * window is snapped to a 4-pixel boundary (`AND [0x0], 0xFFFC`) because the
     * strip save below works in whole page bytes.  Neither moves the camera. */
    level_setUpdates(0);
    level_setView(level_viewX() & ~3u, level_viewY());

    /* 1000:3B34..1000:3B46: spend the use and stash the recipe's fields (the recipe
     * itself is no longer readable once its use is gone) */
    objUses[recipe]--;
    claimBehav = objRecipes[recipe][1];
    claimSprite = (unsigned)((objRecipes[recipe][2] << 8) | objRecipes[recipe][3]);
    for (unsigned k = 0; k < 4u; k++)
        claimParams[k] = rd16(objRecipes[recipe], 4u + 2u * k);

    claimNeighbour = n;
    claimBlinks = 10u;                               /* 1000:3BAC */
    claimTimer = 0u;
    claimFillRamp(claimRed, 0x3Fu, 0u, 0u);          /* 1000:3B4F: AX=0, BL=0x3F */
    claimFillRamp(claimWhite, 0x3Fu, 0x3Fu, 0x3Fu);  /* 1000:3B7A: AX=0x3F3F   */
    claimPhase = CLAIM_TO_RED;
    palette_buildFade(1u, 0x7Fu, 5u, claimRed + 3u); /* 1000:3B56 */
    return n;
}

int object_claimActive(void) { return claimPhase != CLAIM_OFF; }

/* One frame of the sequence; the fades advance through `palette_tick`, which
 * the level's own tick drives.  Every `palette_fadeActive()` wait below is the
 * original's `0x9F8B` (spin while the fade runs). */
void object_claimTick(const uint8_t *levelPal)
{
    uint8_t *nrec = claimNeighbour >= 0
        ? objArray + claimNeighbour * OBJ_STRIDE : NULL;

    switch (claimPhase) {
    case CLAIM_OFF:
        return;
    case CLAIM_TO_RED:                               /* 1000:3B56 */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_RED_BACK;
        palette_buildFade(1u, 0x7Fu, 0x1Eu, levelPal + 3u);  /* 1000:3B68 */
        break;
    case CLAIM_RED_BACK:                             /* 1000:3B68 */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_TO_WHITE;
        palette_buildFade(1u, 0x7Fu, 5u, claimWhite + 3u);   /* 1000:3B82 */
        break;
    case CLAIM_TO_WHITE:                             /* 1000:3B91: 1000:3D04 */
        if (palette_fadeActive()) return;
        claimTimer = 0u;
        claimPhase = CLAIM_PAN;                      /* two frames */
        break;
    case CLAIM_PAN:                                  /* 1000:3D04, two frames */
        /* 1000:3D04 presents the world twice (5810, 5d56, 3d5e); on the first
         * pass 1000:3D41 hands min(player, claimed) to 1000:3DC0, which SAVES a
         * 64x18-byte page strip at that point to A000:0xF000 — a scratch save
         * for the sequence, not a scroll: no [0x0]/[0x2] write exists anywhere
         * in it.  The camera stays where the follower left it (frozen above). */
        if (++claimTimer < 2u) break;
        claimTimer = 0u;
        claimPhase = CLAIM_WHITE_BACK;
        palette_buildFade(1u, 0x7Fu, 0x19u, levelPal + 3u);  /* 1000:3B97 */
        break;
    case CLAIM_WHITE_BACK:                           /* 1000:3BA9 */
        if (palette_fadeActive()) return;
        claimTimer = 0u;
        claimPhase = CLAIM_FRAME;
        break;
    case CLAIM_FRAME:
        if (++claimTimer <= 1u) break;               /* 1000:3BA9's 0x9F56 */
        claimTimer = 0u;
        claimPhase = CLAIM_BLINK_WHITE;
        palette_buildFade(0x80u, 0x40u, claimBlinks, claimWhite); /* 1000:3BB3 */
        break;
    case CLAIM_BLINK_WHITE:                          /* 1000:3BB3 */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_BLINK_LEVEL;
        palette_buildFade(0x80u, 0x40u, claimBlinks,
                          levelPal + 0x80u * 3u);   /* 1000:3BC3 */
        break;
    case CLAIM_BLINK_LEVEL:                          /* 1000:3BC3 */
        if (palette_fadeActive()) return;
        if (--claimBlinks == 0u) {
            claimTimer = 0u;
            claimPhase = CLAIM_ANIM;
            break;
        }
        claimPhase = CLAIM_BLINK_WHITE;
        palette_buildFade(0x80u, 0x40u, claimBlinks, claimWhite);
        break;
    case CLAIM_ANIM:                                 /* 1000:3BD6..1000:3BF6 */
        if (++claimTimer % 9u == 0u)                 /* the 1000:3BE9 frame++ */
            wr16(nrec, 4u, (uint16_t)(rd16(nrec, 4u) + 1u));
        if (claimTimer < 56u) break;
        claimTimer = 0u;
        /* 1000:3BF9..1000:3C64: the append from the stashed fields.  The live record is
         * `0x3F17 + 0x12*([0x3E73]-8)` (port index count+1) and its class
         * `[0x3E5F + count-8]` = `[0x3E5E + count-7]` (count-7 here), i.e.
         * class c <-> live index 8+c. */
        if (objLiveCount + 1u < OBJ_SLOTS) {
            uint8_t *nr = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
            wr16(nr, 0u, rd16(nrec, 0u));
            wr16(nr, 2u, (uint16_t)(rd16(nrec, 2u) + 9u));
            wr16(nr, 4u, (uint16_t)claimSprite);
            wr16(nr, 6u, 0u);
            wr16(nr, 8u, (uint16_t)claimParams[0]);
            wr16(nr, 0xAu, (uint16_t)claimParams[1]);
            wr16(nr, 0xCu, (uint16_t)claimParams[2]);
            wr16(nr, 0xEu, (uint16_t)claimParams[3]);
            objSlotClass[objLiveCount - 7u] = (uint8_t)claimBehav;
            objLiveCount++;
            /* 1000:3C70 calls init_dirty_cell_list (1000:3E73), whose tail
             * hands (x - 0x20, y - 0x20) of the appended record to 1000:3DC0:
             * the same strip save, at the new record.  It does not move the
             * window either; the 0x48BC cell list it builds is the redraw list
             * this flat compose replaces. */
            {
                /* 1000:3E73's other half: the shade table from the recipe's
                 * sprite, and the list starts around the appended record. */
                uint8_t shade[16 * 16];

                objBuildShade(claimSprite, shade);
                level_sparkBegin(rd16(nr, 0u), rd16(nr, 2u), shade);
            }
        }
        claimPhase = CLAIM_HOLD;
        break;
    case CLAIM_HOLD:                                 /* 1000:3C7D: 0x28+1 */
        if (++claimTimer < 41u) break;
        claimTimer = 0u;
        claimPhase = CLAIM_WHITE_OUT;
        palette_buildFade(1u, 0xBFu, 5u, claimWhite + 3u);   /* 1000:3C90 */
        break;
    case CLAIM_WHITE_OUT:                            /* 1000:3C88 */
        if (palette_fadeActive()) return;
        claimTimer = 0u;
        claimPhase = CLAIM_FRAME2;
        break;
    case CLAIM_FRAME2:                               /* 1000:3C9F */
        if (++claimTimer <= 1u) break;               /* one 0x9F56 */
        claimTimer = 0u;
        wr16(nrec, 6u, 0xFFFFu);                     /* 50A6: removed */
        statusbar_setActive(1);                      /* 50B0: [0x5B12] */
        claimPhase = CLAIM_FLIP;
        break;
    case CLAIM_FLIP:                                 /* 1000:3CB5: LOOP CX=2 */
        if (claimTimer == 0u) level_setUpdates(1);   /* 1000:3CAB: [0xC] = 0xFF */
        if (++claimTimer < 2u) break;
        claimTimer = 0u;
        claimPhase = CLAIM_SETTLE;
        palette_buildFade(1u, 0xBFu, 0x28u, levelPal + 3u);  /* 50DE */
        break;
    case CLAIM_SETTLE:                               /* 1000:3CDE */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_OFF;
        claimNeighbour = -1;
        break;
    default:
        claimPhase = CLAIM_OFF;
        break;
    }
}

/* --- the level's object walk (`load_terrain_b`, 1000:5168) -------------
 *
 * The level start clears the array (`1000:4CAB`), sets `[0x3E73] = 8` and the
 * 20 behaviour bytes at `0x3E5F` to 0 (`1000:516F`), then walks the map **once
 * per tile**, in the order of the 25-entry table `202F:0x1EB3` (`1000:5184`:
 * `CX = [0x1EB1]` = 25 pairs, `BX` = the tile bytes, `SI` = the handlers at
 * `0x1ECC`).  Every match is handed to its handler with
 * `CX = col*16`, `DX = row*16` (`1000:51BF`'s four shifts each).
 *
 * Two of the 25 handlers are ported (they are what the shipped levels use for
 * the placed objects); the rest (`0xA9`'s five-record table at `0x1EFE`, the
 * `0x32B9` background list, the movers) are the next pass:
 *
 *   0xAA  `1000:524B`  the human: the **first free static slot** (the scan
 *                    tests `+6` bit 7), `x = col*16`, `y = row*16 - 7`,
 *                    `+4 = 0x10`, `+6 = 0`, `+8 = 2`, `[0x3E20]++`.
 *                    ⚠ the live records sit at `y = row*16 - 8` (memory reads
 *                    and the sprite's screen rect both say so), so the humans
 *                    are not placed by this handler as read — open question,
 *                    see re_map.md.
 *   0xAB  `1000:527D`  the spear: **appended** at the live end, `x = col*16`,
 *                    `y = row*16` (no bias), `+4 = 0x60`, `+6 = 0`, `+8 = 0xFFFF`,
 *                    `+0xA = 0`, the behaviour 5 into `[0x3E5F + n]`, `[0x3E73]++`.
 *                    Level 1's map has exactly one 0xAB cell, at (28,2) — which
 *                    is why the live `[0x3E73] = 9`, slot byte `[0x3E5F] = 5`
 *                    and record 9 hold `+4 = 0x60, +8 = 0xFFFF`.
 */
#define OBJ_COLS 32u
#define OBJ_ROWS 20u

/* 202F:0x1EB3, the tile ids the walk matches, in table order. */
static const uint8_t objTiles[25] = {
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
    0xB3, 0xB4, 0xB5, 0xC0, 0xB6, 0xB7, 0xB8, 0xBA, 0xBB, 0xBE,
    0xBF, 0xC1, 0xF0, 0xF1, 0xF2,
};

/* 1000:524B (tile 0xAA): the human, into the first free static slot. */
static int objPlaceHuman(unsigned x, unsigned y)
{
    for (unsigned i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = objArray + i * OBJ_STRIDE;
        if (!(r[6] & 0x80u)) continue;
        wr16(r, 0u, (uint16_t)x);
        wr16(r, 2u, (uint16_t)y);        /* 0x524B: y = row*16 - 7 */
        wr16(r, 4u, 0x0010u);
        wr16(r, 6u, 0u);
        wr16(r, 8u, 0x0002u);
        objPlacedHumans++;
        return (int)i;
    }
    return -1;
}

/* 1000:527D (tile 0xAB): the spear, appended at the live end. */
static void objAppendSpear(unsigned x, unsigned y)
{
    uint8_t *r;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)x);
    wr16(r, 2u, (uint16_t)y);
    wr16(r, 4u, 0x0060u);
    wr16(r, 6u, 0u);
    wr16(r, 8u, 0xFFFFu);
    wr16(r, 0xAu, 0u);
    objSlotClass[objLiveCount - 7u] = 5u;        /* [0x3E5F + count-8] = 0x05 */
    objLiveCount++;
}

/* 0xBE / 0xBF's hand-off: 0xBE keeps the record it appended (`0x1F1C`) and the
 * slot 0xBF joins into (`0x1F1E`); the port keeps the record index. */
static int objJoinSlot = -1;

/* 1000:54D9 (0xBB): {x, y, +4 = 0x26, +6 = 0, +8 = the map cell - 1},
 * behaviour 9. */
static void objSpawnBB(unsigned col, unsigned row)
{
    uint8_t *r;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)(col * 16u));
    wr16(r, 2u, (uint16_t)(row * 16u));
    wr16(r, 4u, 0x0026u);
    wr16(r, 6u, 0x0000u);
    wr16(r, 8u, (uint16_t)(row * OBJ_COLS + col - 1u));
    objSlotClass[objLiveCount - 7u] = 9u;
    objLiveCount++;
}

/* 1000:54FC (0xBE): {x, y, +4 = 0x82, +6 = 0, +8 = 0, +0xA = x, +0xE = 1},
 * behaviour 2 - and it remembers the record for 0xBF. */
static void objSpawnBE(unsigned col, unsigned row)
{
    uint8_t *r;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)(col * 16u));
    wr16(r, 2u, (uint16_t)(row * 16u));
    wr16(r, 4u, 0x0082u);
    wr16(r, 6u, 0x0000u);
    wr16(r, 8u, 0x0000u);
    wr16(r, 0xAu, (uint16_t)(col * 16u));
    wr16(r, 0xEu, 0x0001u);
    objSlotClass[objLiveCount - 7u] = 2u;
    objJoinSlot = (int)(objLiveCount + 1u);      /* the record, not the count */
    objLiveCount++;
}

/* 1000:552B (0xBF): the join half - written into the record 0xBE made. */
static void objJoinBF(unsigned col)
{
    if (objJoinSlot < 0) return;
    wr16(objArray + (unsigned)objJoinSlot * OBJ_STRIDE, 0xCu, (uint16_t)(col * 16u));
}

/* 1000:5535 / 1000:553D (0xC0 / 0xC1): 0xC0 runs the human handler and keeps
 * its slot as the player's record ([0x180A]); 0xC1 appends {x, y-0x10,
 * +4 = 0x100}, behaviour 0. */
static void objSpawnC0(unsigned col, unsigned row)
{
    int slot = objPlaceHuman(col * 16u, row * 16u - 7u);

    if (slot >= 0) objPlayer = slot;
}

static void objSpawnC1(unsigned col, unsigned row)
{
    uint8_t *r;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)(col * 16u));
    wr16(r, 2u, (uint16_t)(row * 16u - 0x10u));
    wr16(r, 4u, 0x0100u);
    wr16(r, 6u, 0x0000u);
    objSlotClass[objLiveCount - 7u] = 0u;
    objLiveCount++;
}

/* 1000:5310 / 1000:532F / 1000:5338 / 1000:5357: the four data-only tiles. */
static void objWritePair(unsigned tile, unsigned x, unsigned y)
{
    switch (tile) {
    case 0xAFu:
        objPairA[0] = (uint16_t)x; objPairA[1] = (uint16_t)y;
        objPairA[2] = (uint16_t)x; objPairA[3] = (uint16_t)y;
        objPairA[4] = objPairA[5] = objPairA[6] = objPairA[7] = 0u;
        break;
    case 0xB0u:
        objPairB[0] = (uint16_t)x; objPairB[1] = (uint16_t)y;
        break;
    case 0xB1u:
        objPairC[0] = (uint16_t)x; objPairC[1] = (uint16_t)y;
        objPairC[2] = (uint16_t)x; objPairC[3] = (uint16_t)y;
        objPairC[4] = objPairC[5] = objPairC[6] = objPairC[7] = 0u;
        break;
    default:
        objPairD[0] = (uint16_t)x; objPairD[1] = (uint16_t)y;
        break;
    }
}

/* 1000:53BA (0xB6): a placed object in the first *static* slot - frame 0xFE,
 * type 0x1D, +0xA = 0xFA, +0xC = the record-area pointer - yet the live count
 * still grows.  The pointer is stored as the record-area offset here. */
static void objSpawnB6(unsigned col, unsigned row)
{
    for (unsigned i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = objArray + i * OBJ_STRIDE;

        if (!(r[6] & 0x80u)) continue;
        wr16(r, 0u, (uint16_t)(col * 16u));
        wr16(r, 2u, (uint16_t)(row * 16u - 0x10u));
        wr16(r, 4u, 0x00FEu);
        wr16(r, 6u, 0x0000u);
        wr16(r, 8u, 0x001Du);
        wr16(r, 0xAu, 0x00FAu);
        wr16(r, 0xCu, (uint16_t)(((objLiveCount + 1u) * OBJ_STRIDE) & 0xFFFFu));
        objSlotClass[objLiveCount - 7u] = 0u;
        objLiveCount++;
        return;
    }
}

/* 1000:53FD (0xB7): TWO appended records; the first carries the only computed
 * class (`+6 = (count - 6) << 8`), frame 0x11A when a 0xB6 tile sits within four
 * columns either side, else 0x116, behaviour 8; the second is empty, behaviour
 * 0x85. */
static void objSpawnB7(const uint8_t *map, unsigned col, unsigned row)
{
    uint8_t *r;
    unsigned cell;
    int nearB6 = 0;

    if (objLiveCount + 2u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    cell = row * OBJ_COLS + col;
    if (map) {
        static const int off[8] = { -5, -4, -3, -2, 0, 1, 2, 3 };

        for (unsigned k = 0; k < 8u; k++) {
            long c = (long)cell + off[k];

            if (c >= 0 && c < 640 && map[c] == 0xB6u) { nearB6 = 1; break; }
        }
    }
    wr16(r, 0u, (uint16_t)(col * 16u));
    wr16(r, 2u, (uint16_t)(row * 16u - 8u));
    wr16(r, 4u, nearB6 ? 0x011Au : 0x0116u);
    wr16(r, 6u, (uint16_t)(((objLiveCount - 6u) << 8) & 0xFF00u));
    wr16(r, 8u, 0x0000u);
    wr16(r, 0xAu, nearB6 ? 3u : 2u);
    objSlotClass[objLiveCount - 7u] = 8u;
    objLiveCount++;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    memset(r, 0, OBJ_STRIDE);
    objSlotClass[objLiveCount - 7u] = 0x85u;
    objLiveCount++;
}

/* 1000:5481 (0xB8): one appended record, frame 0xAF, behaviour 3, and
 * `+0xE = SI - 1` - the map cell pointer, stored here as the cell index. */
static void objSpawnB8(unsigned col, unsigned row)
{
    uint8_t *r;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)(col * 16u));
    wr16(r, 2u, (uint16_t)(row * 16u - 8u));
    wr16(r, 4u, 0x00AFu);
    wr16(r, 6u, 0x0000u);
    wr16(r, 8u, 0x0000u);
    wr16(r, 0xAu, 0x0000u);
    wr16(r, 0xCu, 0x0000u);
    wr16(r, 0xEu, (uint16_t)(row * OBJ_COLS + col - 1u));
    objSlotClass[objLiveCount - 7u] = 3u;
    objLiveCount++;
}

/* 1000:52A2 (0xAC, frame 0x96 -> behaviour 7), 1000:52C7 (0xAD, frame 0x9C ->
 * behaviour 6) and 1000:52EB (0xAE, frame 0x38 -> behaviour 4): one appended
 * record each, then the shared tail 1000:5639 snaps its y to the terrain step.
 * Note the behaviours: the campfire (0x96) is 7 and the torch (0x9C) is 6 - the
 * summary row in re_map.md had those two swapped until this read.
 * 1000:5662 (called first) nudges y by 3 on a style-6 level when the cell
 * diagonally below-left is marked in the static 0x1F20 table. */
static void objAppendStyle(const uint8_t *map, unsigned col, unsigned row,
                           unsigned frame, unsigned behaviour, unsigned f8,
                           unsigned fc, unsigned fe)
{
    /* 0x1F20: the per-tile bit table 1000:5662 tests (static in the image). */
    static const uint8_t nudgeBits[19] = {
        0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07, 0xE0, 0x00,
        0x02, 0x02, 0x00, 0x03, 0xC3, 0x81, 0x80, 0x01, 0x80,
    };
    uint8_t *r;
    unsigned y3 = 0u, probeCell;
    int bx;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    /* 1000:5662: on a style-6 level, the cell diagonally below-left decides
     * whether the decoration sits 3 px lower (`[0x17EE] == 6`, the 0x1F20 bit). */
    {
        unsigned cell = row * OBJ_COLS + col;
        unsigned tile2 = (map && cell + 0x1Fu < 640u) ? map[cell + 0x1Fu] : 0xFFu;

        if (level_style() == 6u && tile2 <= 0x90u) {
            uint8_t bit = (uint8_t)(1u << (7u - (tile2 & 7u)));

            if (nudgeBits[tile2 >> 3] & bit) y3 = 3u;
        }
    }
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)(col * 16u));
    wr16(r, 2u, (uint16_t)(row * 16u + y3));
    wr16(r, 4u, frame);
    wr16(r, 6u, 0u);
    wr16(r, 8u, f8);
    wr16(r, 0xAu, 0u);
    wr16(r, 0xCu, fc);
    wr16(r, 0xEu, fe);
    objSlotClass[objLiveCount - 7u] = (uint8_t)behaviour;
    objLiveCount++;
    bx = objStepProbeAt(r, 8, 0x10, &probeCell);        /* 1000:5639 */
    if (bx >= 0) wr16(r, 2u, (uint16_t)(bx - 0x10));
}

/* 1000:51F9 decode_terrain_row (tile 0xA9): five records from the 0x1EFE
 * offset table at (col*16 + dx, row*16 + 1 + dy), frames 0x33..0x37, `+8 = 0`,
 * behaviour 0, plus one 0x32B9 background entry whose `+4` points at their first
 * record and whose `+8` is the map pointer + 0x20.
 * Its first two instructions mark the two map cells one row below as 0xC2/0xC9 -
 * the ids 1000:DF2C tests, so this handler is what makes the settle's background
 * mark reachable. */
static void objAppendRow9(uint8_t *map, unsigned col, unsigned row)
{
    static const uint8_t table[5][3] = {         /* 0x1EFE: dx, dy, frame */
        { 0x10, 0x13, 0x33 }, { 0x16, 0x15, 0x34 }, { 0x1C, 0x16, 0x35 },
        { 0x23, 0x15, 0x36 }, { 0x29, 0x13, 0x37 },
    };
    unsigned first = objLiveCount + 1u;
    uint8_t *firstRec = objArray + first * OBJ_STRIDE;

    for (unsigned i = 0; i < 5u; i++) {
        uint8_t *r;

        if (objLiveCount + 1u >= OBJ_SLOTS) return;
        r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
        wr16(r, 0u, (uint16_t)(col * 16u + table[i][0]));
        wr16(r, 2u, (uint16_t)(row * 16u + 1u + table[i][1]));
        wr16(r, 4u, table[i][2]);
        wr16(r, 6u, 0u);
        wr16(r, 8u, 0u);
        objSlotClass[objLiveCount - 7u] = 0u;     /* behaviour 0 */
        objLiveCount++;
    }
    /* 1000:51F9's first two instructions: mark the two cells one row below as
     * the ids 1000:DF2C looks for.  The original's buffer carries a duplicated
     * first row at +0x20, so its write always lands; the port's map is exactly
     * 640 bytes, so the pair is bounds-checked instead. */
    {
        unsigned cell = row * OBJ_COLS + col;

        if (cell + OBJ_COLS + 1u < 640u) {
            map[cell + OBJ_COLS] = 0xC2u;
            map[cell + OBJ_COLS + 1u] = 0xC9u;
        }
    }
    if (objBgCount < sizeof objBg / sizeof objBg[0]) {
        objBg[objBgCount].x = (uint16_t)(col * 16u);
        objBg[objBgCount].y = (uint16_t)(row * 16u + 1u);
        objBg[objBgCount].dst = firstRec;
        objBgCount++;                            /* [0x32B9] */
    }
}

/* 1000:556B terrain_append_decoration, the tile handler 0xF0/0xF1/0xF2 share
 * (0xF0 is a four-instruction stub that sets BX and jumps in; the frame is
 * 0xC1 / 0xC5 / 0xC9, per the table in re_map.md): an appended record
 * {x, y, frame, +6 = 0, +8 = frame, +0xA = 0} with behaviour 1. */
static void objAppendDecoration(unsigned x, unsigned y, uint8_t tile)
{
    static const uint16_t frame[3] = { 0x00C1u, 0x00C5u, 0x00C9u };
    uint16_t f = frame[tile - 0xF0u];
    uint8_t *r;

    if (objLiveCount + 1u >= OBJ_SLOTS) return;
    r = objArray + (objLiveCount + 1u) * OBJ_STRIDE;
    wr16(r, 0u, (uint16_t)x);
    wr16(r, 2u, (uint16_t)y);
    wr16(r, 4u, f);
    wr16(r, 6u, 0u);
    wr16(r, 8u, f);
    wr16(r, 0xAu, 0u);
    objSlotClass[objLiveCount - 7u] = 1u;        /* [0x3E5F + count-8] = 0x01 */
    objLiveCount++;
}

unsigned object_loadTerrainObjects(uint8_t *map)
{
    if (!map) return objLiveCount;
    objLiveCount = OBJ_LIVE0;                    /* 1000:516F */
    memset(objSlotClass, 0, 20u);                /* 1000:5175: 0x14 bytes */
    for (unsigned t = 0; t < 25u; t++) {
        uint8_t tile = objTiles[t];
        /* all 25 tiles' bodies are ported (re_map.md has each one's
         * summary); the guard is the table walk's tile list. */
        if (tile != 0xA9u && tile != 0xAAu && tile != 0xABu &&
            tile != 0xACu && tile != 0xADu && tile != 0xAEu &&
            tile != 0xAFu && tile != 0xB0u && tile != 0xB1u && tile != 0xB2u &&
            tile != 0xB6u && tile != 0xB7u && tile != 0xB8u &&
            tile != 0xBBu && tile != 0xBEu && tile != 0xBFu &&
            tile != 0xC0u && tile != 0xC1u &&
            (tile < 0xF0u || tile > 0xF2u)) continue;
        for (unsigned row = 0; row < OBJ_ROWS; row++)
            for (unsigned col = 0; col < OBJ_COLS; col++) {
                if (map[row * OBJ_COLS + col] != tile) continue;
                /* live-confirmed (2026-10-30): level 1's records read
                 * (48,25) (208,25) (256,137) (464,265) = row*16 - 7, exactly
                 * what 0x524B writes. */
                if (tile == 0xBBu)
                    objSpawnBB(col, row);
                else if (tile == 0xBEu)
                    objSpawnBE(col, row);
                else if (tile == 0xBFu)
                    objJoinBF(col);
                else if (tile == 0xC0u)
                    objSpawnC0(col, row);
                else if (tile == 0xC1u)
                    objSpawnC1(col, row);
                else if (tile == 0xB6u)
                    objSpawnB6(col, row);
                else if (tile == 0xB7u)
                    objSpawnB7(map, col, row);
                else if (tile == 0xB8u)
                    objSpawnB8(col, row);
                else if (tile >= 0xAFu && tile <= 0xB2u)
                    objWritePair(tile, col * 16u, row * 16u);
                else if (tile == 0xA9u)
                    objAppendRow9(map, col, row);
                else if (tile == 0xAAu)
                    objPlaceHuman(col * 16u, row * 16u - 7u);
                else if (tile == 0xABu)
                    objAppendSpear(col * 16u, row * 16u);
                else if (tile == 0xACu)
                    objAppendStyle(map, col, row, 0x96u, 7u, 0xFFFFu, 0u, 0u);
                else if (tile == 0xADu)
                    objAppendStyle(map, col, row, 0x9Cu, 6u, 0xFFFFu, 0u, 0u);
                else if (tile == 0xAEu)
                    /* 1000:52EB differs from 0x52A2/0x52C7: +8 and +0xA are
                     * 0 and BOTH +0xC and +0xE are 0xFFFF (+0xE is the
                     * give/hold gate 0x9CF6 tests with bit 15). */
                    objAppendStyle(map, col, row, 0x38u, 4u, 0x0000u, 0xFFFFu,
                                   0xFFFFu);
                else
                    objAppendDecoration(col * 16u, row * 16u, tile);
            }
    }
    return objLiveCount;
}

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
 * `SHR AL,1 / AND 0x1F`) and dispatches on `mode`; only mode 1 (the unscaled
 * blit) is ported, and every frame the port draws so far is mode 1.
 */
typedef struct { uint32_t off; uint8_t rows, ybias, mode; } ObjSprite;
#define OBJ_FRAME_SLOTS 0x11Eu              /* apply_relocations: 0x39A8..0x3E20 */

static const ObjSprite objSprites[OBJ_FRAME_SLOTS] = {
    /* 000 */ {12416,24, 0,1}, {12800,24, 0,1}, {13184,23, 1,1}, {13552,23, 1,1},
    /* 004 */ {13920,24, 0,1}, {14304,24, 0,1}, {14688,23, 1,1}, {15056,23, 1,1},
    /* 008 */ {15424,24, 0,1}, {15808,24, 0,1}, {16192,23, 1,1}, {16560,23, 1,1},
    /* 00C */ {16928,24, 0,1}, {17312,24, 0,1}, {17696,23, 1,1}, {18064,23, 1,1},
    /* 010 */ {18432,24, 0,1}, {18816, 0,24,1}, {18816,24, 0,1}, {19200,24, 0,1},
    /* 014 */ {19584,24, 0,1}, {19968,24, 0,1}, {20352,24, 0,1}, {    0,13, 2,1},
    /* 018 */ {20736, 0,24,1}, {20736, 0,24,1}, {20736, 0,24,1}, {20736, 0,24,1},
    /* 01C */ {20736, 0,24,1}, {54256, 0, 0,1}, {54256, 0, 0,1}, {54256, 0,16,1},
    /* 020 */ {20736,24, 0,1}, {  208,16, 0,1}, {  464,16, 0,1}, {21120,24, 0,1},
    /* 024 */ {21504,23, 0,1}, {21872,24, 0,1}, {0,0,0,0}, {0,0,0,0},
    /* 028 */ {54256, 9,15,2}, {54544,15, 9,2}, {55024,18, 6,2}, {55600,20, 4,2},
    /* 02C */ {56240,22, 2,2}, {56944,22, 1,2}, {  720,14, 0,1}, {  944,14, 0,1},
    /* 030 */ { 1168,14, 0,1}, { 1392,14, 0,1}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},
    /* 038 */ { 1616,16, 0,1}, { 1872,16, 0,1}, { 2128,16, 0,1}, { 2384,16, 0,1},
    /* 03C */ {57648,32, 0,2}, {58672,31, 1,2}, {59664,32, 0,2}, {60688,31, 1,2},
    /* 040 */ {61680, 0, 0,3}, {61680,32, 0,2}, {62704,31, 1,2}, {63696,32, 0,2},
    /* 044 */ {64720,31, 1,2}, {65712,32, 0,2}, {66736,31, 1,2}, {67728, 0, 0,3},
    /* 048 */ {67728,32, 0,2}, {68752,31, 1,2}, {69744,32, 0,2}, {70768,32, 0,2},
    /* 04C */ {71792,32, 0,2}, {72816,32, 0,2}, {73840,32, 0,2}, {74864,32, 0,2},
    /* 050 */ {75888,32, 0,2}, {76912,32, 0,2}, {22256,24, 0,1}, {22640,23, 0,1},
    /* 054 */ {23008,24, 0,1}, {23392,24, 0,1}, {23776,23, 1,1}, {24144,24, 0,1},
    /* 058 */ { 2640,16, 0,1}, { 2896,16, 0,1}, { 3152,14, 0,1}, { 3376, 7, 4,1},
    /* 05C */ { 3488, 5, 5,1}, { 3568,11, 2,1}, { 3744,16, 0,1}, { 4000,16, 0,1},
    /* 060 */ { 4256,16, 0,1}, { 4512,16, 0,1}, { 4768,14, 2,1}, { 4992, 7, 5,1},
    /* 064 */ { 5104, 5, 6,1}, { 5184,11, 3,1}, { 5360,16, 0,1}, { 5616,16, 0,1},
    /* 068 */ {24528, 5,13,1}, {24608, 5,13,1}, {24688, 5,12,1}, {24768, 5,14,1},
    /* 06C */ {24848, 6,13,1}, {24944, 5,14,1}, {25024, 5,15,1}, {25104, 5,15,1},
    /* 070 */ {25184, 6,13,1}, {25280, 5,13,1}, {25360, 5,12,1}, {25440, 5,14,1},
    /* 074 */ {25520, 5,14,1}, {25600, 5,14,1}, {25680, 5,14,1}, {25760, 4,15,1},
    /* 078 */ {25824, 4,15,1}, {25888, 6, 0,1}, {25984, 5, 0,1}, {26064, 5, 1,1},
    /* 07C */ {26144, 6, 0,1}, {26240, 5, 0,1}, {26320, 5, 1,1}, {26400,24, 0,1},
    /* 080 */ {26784,24, 0,1}, {27168,24, 0,1}, { 5872,16, 0,1}, { 6128,15, 1,1},
    /* 084 */ { 6368,16, 0,1}, { 6624,15, 1,1}, { 6864,16, 0,1}, { 7120,15, 1,1},
    /* 088 */ { 7360,16, 0,1}, { 7616,15, 1,1}, { 7856,16, 0,1}, { 8112,16, 0,1},
    /* 08C */ {27552,24, 0,1}, {27936,24, 0,1}, {28320,24, 0,1}, {28704, 0,24,1},
    /* 090 */ {28704, 0,24,1}, {28704,24, 0,1}, {29088,24, 0,1}, {29472,24, 0,1},
    /* 094 */ {29856, 0,24,1}, {29856, 0,24,1}, { 8368, 6,10,1}, { 8464,13, 1,1},
    /* 098 */ { 8672,15, 1,1}, { 8912,15, 1,1}, { 9152,14, 1,1}, { 9376,12, 1,1},
    /* 09C */ { 9568,12, 4,1}, { 9760,13, 3,1}, { 9968,11, 5,1}, {10144, 0,16,1},
    /* 0A0 */ {10144, 0,16,1}, {10144, 7, 7,1}, {10256,10, 3,1}, {10416,14, 0,1},
    /* 0A4 */ {10640,14, 0,1}, {10864,10, 3,1}, {11024, 7, 7,1}, {11136, 0,16,1},
    /* 0A8 */ {11136, 0,16,1}, {11136,10, 4,1}, {11296,13, 1,1}, {29856,24, 0,1},
    /* 0AC */ {30240,24, 0,1}, {30624,24, 0,1}, {31008,24, 0,1}, {31392,16, 8,1},
    /* 0B0 */ {31648,16, 8,1}, {31904,23, 1,1}, {32272,22, 2,1}, {32624,23, 1,1},
    /* 0B4 */ {32992,15, 9,1}, {33232,13,11,1}, {33440,13,11,1}, {0,0,0,0},
    /* B8.. */ {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {33648,23, 1,1},
    /* 0BC */ {34016,24, 0,1}, {34400,23, 0,1}, {34768,23, 1,1}, {35136,24, 0,1},
    /* 0C0 */ {35520,23, 0,1}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},
    /* C8.. */ {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {11504,11, 5,1}, {11680,11, 5,1}, {11856, 8, 8,1},
    /* 0D0 */ {11984, 8, 8,1}, {12112, 5,11,1}, {12192, 5,11,1}, {12272, 9, 7,1},
    /* 0D4 */ {35888,24, 0,1}, {36272,23, 1,1}, {36640,24, 0,1}, {37024,24, 0,1},
    /* 0D8 */ {37408,24, 0,1}, {37792,23, 1,1}, {38160,24, 0,1}, {38544,24, 0,1},
    /* 0DC */ {38928,23, 1,1}, {39296,23, 1,1}, {39664,24, 0,1}, {40048,23, 1,1},
    /* 0E0 */ {40416,24, 0,1}, {40800,23, 1,1}, {41168,24, 0,1}, {41552,24, 0,1},
    /* 0E4 */ {41936,24, 0,1}, {42320,23, 1,1}, {42688,24, 0,1}, {43072,24, 0,1},
    /* 0E8 */ {43456,23, 1,1}, {43824,23, 1,1}, {44192,24, 0,1}, {44576,23, 1,1},
    /* 0EC */ {44944, 4,14,1}, {45008, 4,15,1}, {45072, 4,13,1}, {45136, 4,12,1},
    /* 0F0 */ {45200, 5,14,1}, {45280, 4,14,1}, {45344, 4,13,1}, {45408, 4,12,1},
    /* 0F4 */ {45472, 4, 2,1}, {45536, 4,14,1}, {45600, 4,15,1}, {45664, 4,13,1},
    /* 0F8 */ {45728, 4,12,1}, {45792, 5,14,1}, {45872, 4,14,1}, {45936, 4,13,1},
    /* 0FC */ {46000, 4,12,1}, {46064, 4, 2,1}, {77936,24, 8,2}, {78704,24, 8,2},
    /* 100 */ {79472,22,10,2}, {80176,22,10,2}, {80880,27, 5,2}, {46128,24, 0,1},
    /* 104 */ {46512,24, 0,1}, {46896,24, 0,1}, {47280,24, 0,1}, {47664,24, 0,1},
    /* 108 */ {48048,24, 0,1}, {48432,24, 0,1}, {48816,19, 5,1}, {49120,11,13,1},
    /* 10C */ {49296,14,10,1}, {49520,14,10,1}, {49744,24, 0,1}, {50128,24, 0,1},
    /* 110 */ {50512,24, 0,1}, {0,0,0,0}, {50896,24, 0,1}, {51280,24, 0,1},
    /* 114 */ {51664,24, 0,1}, {0,0,0,0}, {52048,23, 1,1}, {52416,23, 1,1},
    /* 118 */ {52784,23, 1,1}, {0,0,0,0}, {53152,23, 1,1}, {53520,23, 1,1},
    /* 11C */ {53888,23, 1,1}, {0,0,0,0},
};

unsigned object_frameSlots(void) { return OBJ_FRAME_SLOTS; }

/* 0x32B9's queue for the drawer at 1000:5F54 (level.c's level_drawBgSprites). */
unsigned object_bgCount(void) { return objBgCount; }

void object_bgAt(unsigned i, unsigned *x, unsigned *y)
{
    if (i >= objBgCount) { *x = 0; *y = 0; return; }
    *x = objBg[i].x;
    *y = objBg[i].y;
}

static const uint8_t *objBank;            /* APL\\SPRITES.DAT, the sprite image */
static uint32_t objBankSize;

void object_publishBank(const uint8_t *bank, uint32_t size)
{
    objBank = bank;
    objBankSize = size;
}

/* 1000:653C's base blit: the sprite at `src`, top-left (x,y), 0 = transparent.
 * `cols` is 16 for mode 1 (`1000:66EC`) and 32 for mode 2 (`1000:6C74`) — the
 * only difference between the two (the frame stride confirms it: mode-2 frames
 * advance rows*32 bytes).  The layout is derived, not probed: the blit is four
 * plane passes (`67BA: AH = 1 << ([0x6C32] & 3)`, `OUT 0x3C4`), each row-taking
 * `LODSW`/store/INC DI x2 (`67F9..681A`) = **4 bytes, one per pixel, 4 px per
 * byte of one plane**; between passes `6C15 ADD SI, [0x6C2C]` and [0x6C2C] is 0
 * unless clipped at the top, so the frame is consumed linearly in four blocks
 * of 4h (`block = col mod 4`, `byte = row*4 + col/4`). */
static void blitSprite(uint8_t *page, const uint8_t *src, unsigned rows,
                       unsigned cols, int x, int y)
{
    unsigned w = cols >> 2, block = rows * w;

    for (unsigned row = 0; row < rows; row++) {
        int py = y + (int)row;
        if (py < 0 || py >= 200) continue;
        for (unsigned col = 0; col < cols; col++) {
            uint8_t v = src[(col & 3u) * block + row * w + (col >> 2)];
            int px = x + (int)col;
            if (!v) continue;
            if (px < 0 || px >= 320) continue;
            page[(unsigned)py * 320u + (unsigned)px] = v;
        }
    }
}

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
 *                       the port's `objClsCol` copies 0x3566..0x35A5, i.e. the
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
static const uint8_t objClsCol[0x40] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x00, 0x12, 0x12, 0x12, 0x12,
    0x12, 0x00, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x15, 0x16, 0x16, 0x16, 0xC3, 0xC4, 0xC5,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t objFrameRow[0x11A] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x00, 0x12, 0x12, 0x12, 0x12, 0x12, 0x00, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x15,
    0x16, 0x16, 0x16, 0xC3, 0xC4, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xC7, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x18, 0x19, 0x00,
    0x00, 0x1A, 0x1B, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1D, 0x1D, 0x1E, 0x1E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x00, 0x00, 0x00,
    0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0x38,
    0x39, 0x38, 0x3A, 0x3B, 0x3C, 0x3B, 0x3D, 0x3E, 0x3F, 0x3E,
};
static const uint8_t objOverlayIdx[0xE1] = {
    0x40, 0x41, 0x42, 0x41, 0x01, 0x1C, 0x30, 0x02, 0x1D, 0x31, 0x03, 0x1E, 0x32, 0x04, 0x1F, 0x33,
    0x05, 0x20, 0x34, 0x06, 0x21, 0x35, 0x07, 0x22, 0x36, 0x08, 0x23, 0x37, 0x09, 0x24, 0x38, 0x0A,
    0x25, 0x39, 0x0B, 0x26, 0x3A, 0x0C, 0x27, 0x3B, 0x0D, 0x28, 0x3C, 0x0E, 0x29, 0x3D, 0x0F, 0x2A,
    0x3E, 0x10, 0x2B, 0x3F, 0x11, 0x2C, 0x40, 0x11, 0x2C, 0x40, 0x12, 0x2D, 0x41, 0x13, 0x2E, 0x42,
    0x14, 0x2F, 0x43, 0x15, 0x8E, 0x8F, 0x16, 0x46, 0x4C, 0x17, 0x47, 0x4D, 0x18, 0x48, 0x4E, 0x19,
    0x49, 0x4F, 0x1A, 0x4A, 0x50, 0x1B, 0x4B, 0x51, 0x00, 0x44, 0x00, 0x00, 0x45, 0x00, 0x52, 0x00,
    0x00, 0x53, 0x00, 0x00, 0x54, 0x00, 0x00, 0x55, 0x00, 0x00, 0x56, 0x00, 0x00, 0x57, 0x00, 0x00,
    0x58, 0x00, 0x00, 0x59, 0x00, 0x00, 0x5A, 0x00, 0x00, 0x5B, 0x00, 0x00, 0x5C, 0x00, 0x00, 0x5D,
    0x00, 0x00, 0x5E, 0x00, 0x00, 0x5F, 0x00, 0x00, 0x60, 0x00, 0x00, 0x61, 0x00, 0x00, 0x62, 0x00,
    0x00, 0x63, 0x00, 0x00, 0x64, 0x00, 0x00, 0x65, 0x00, 0x00, 0x66, 0x00, 0x00, 0x67, 0x00, 0x00,
    0x68, 0x00, 0x00, 0x69, 0x00, 0x00, 0x6A, 0x00, 0x70, 0x6B, 0x00, 0x71, 0x6C, 0x00, 0x72, 0x6D,
    0x00, 0x73, 0x6E, 0x00, 0x74, 0x6F, 0x00, 0x75, 0x76, 0x00, 0x00, 0x77, 0x00, 0x00, 0x78, 0x00,
    0x00, 0x79, 0x00, 0x00, 0x7A, 0x00, 0x00, 0x7B, 0x00, 0x00, 0x7C, 0x7F, 0x82, 0x7D, 0x80, 0x83,
    0x7E, 0x81, 0x84, 0x85, 0x88, 0x8B, 0x86, 0x89, 0x8C, 0x87, 0x8A, 0x8D, 0x00, 0x00, 0x00, 0x00,
    0x02,
};
static const uint8_t objOverlayEnt[0x240] = {
    0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x59, 0x68, 0x04, 0x08, 0x5A, 0x69, 0x06, 0x06, 0x5A, 0x6A,
    0x04, 0x09, 0x5A, 0x6B, 0x01, 0x08, 0x5A, 0x6C, 0x00, 0x09, 0x5A, 0x6D, 0xFC, 0x0A, 0x5B, 0x6E,
    0xFF, 0x09, 0x5A, 0x6F, 0xFF, 0x07, 0x66, 0x70, 0xFB, 0x05, 0x66, 0x71, 0xFC, 0x06, 0x66, 0x72,
    0xFC, 0x07, 0x66, 0x73, 0x00, 0x06, 0x66, 0x74, 0x01, 0x07, 0x66, 0x75, 0x03, 0x07, 0x65, 0x76,
    0x02, 0x08, 0x66, 0x77, 0x06, 0x08, 0x58, 0x78, 0x00, 0x08, 0x66, 0x70, 0x00, 0x10, 0x66, 0x70,
    0x00, 0x20, 0x66, 0x70, 0x00, 0xFA, 0x5C, 0x00, 0xFE, 0xFB, 0x5B, 0x79, 0xFD, 0xFB, 0x5B, 0x7A,
    0xFC, 0xFC, 0x5A, 0x7B, 0x01, 0xF9, 0x65, 0x7C, 0x02, 0xF9, 0x65, 0x7D, 0x05, 0xFB, 0x66, 0x7E,
    0x00, 0x0C, 0x99, 0x68, 0x05, 0x0B, 0x9A, 0x69, 0x07, 0x0A, 0x9B, 0x6A, 0x05, 0x0D, 0x9A, 0x6B,
    0x01, 0x0C, 0x99, 0x6C, 0xFF, 0x0D, 0x98, 0x6D, 0xFB, 0x0E, 0x97, 0x6E, 0xFE, 0x0D, 0x98, 0x6F,
    0x00, 0x0D, 0x99, 0x70, 0xFC, 0x0A, 0x98, 0x71, 0xFB, 0x0B, 0x97, 0x72, 0xFC, 0x0D, 0x98, 0x73,
    0x00, 0x0D, 0x99, 0x74, 0x02, 0x0D, 0x9A, 0x75, 0x06, 0x0D, 0x9B, 0x76, 0x02, 0x0E, 0x9A, 0x77,
    0x05, 0x0D, 0x99, 0x78, 0x00, 0x0D, 0x99, 0x70, 0x00, 0x15, 0x99, 0x70, 0x00, 0x25, 0x99, 0x70,
    0x04, 0x05, 0xA2, 0x68, 0x06, 0x04, 0xA2, 0x69, 0x04, 0x03, 0xA3, 0x6A, 0x03, 0x05, 0xA3, 0x6B,
    0x00, 0x05, 0xA3, 0x6C, 0x03, 0x06, 0xA2, 0x6D, 0xFE, 0x07, 0xA1, 0x6E, 0x02, 0x06, 0xA2, 0x6F,
    0x00, 0x05, 0xA4, 0x70, 0xFD, 0x03, 0xA4, 0x71, 0xFC, 0x02, 0xA4, 0x72, 0xFE, 0x05, 0xA4, 0x73,
    0x00, 0x05, 0xA4, 0x74, 0xFE, 0x05, 0xA5, 0x75, 0x02, 0x06, 0xA6, 0x76, 0xFF, 0x06, 0xA5, 0x77,
    0x06, 0x03, 0xAA, 0x78, 0x00, 0x05, 0xA4, 0x70, 0x00, 0x0D, 0xA4, 0x70, 0x00, 0x1D, 0xA4, 0x70,
    0xFC, 0x08, 0x96, 0x00, 0x02, 0x08, 0x96, 0x00, 0xFD, 0xF9, 0x97, 0x79, 0xFC, 0xF9, 0x97, 0x7A,
    0xFB, 0xFA, 0x97, 0x7B, 0x03, 0xF9, 0x97, 0x7C, 0x04, 0xF9, 0x97, 0x7D, 0x05, 0xFA, 0x97, 0x7E,
    0x00, 0xF8, 0xA2, 0x79, 0xFF, 0xF8, 0xA2, 0x7A, 0xFE, 0xF9, 0xA2, 0x7B, 0xFF, 0xF8, 0xA5, 0x7C,
    0x00, 0xF8, 0xA5, 0x7D, 0x01, 0xF9, 0xA5, 0x7E, 0x06, 0x09, 0x60, 0x00, 0x01, 0x10, 0x61, 0x00,
    0xF7, 0x08, 0x64, 0x00, 0xF9, 0x09, 0x60, 0x00, 0xFC, 0x10, 0x5F, 0x00, 0x0A, 0x09, 0x5C, 0x00,
    0xFD, 0x09, 0x5C, 0xEC, 0xFE, 0x09, 0x5C, 0xED, 0x00, 0x08, 0x5C, 0xEE, 0x00, 0x06, 0x5B, 0xEF,
    0xFE, 0x08, 0x5B, 0xF0, 0xFF, 0x08, 0x5B, 0xF1, 0x00, 0x07, 0x5B, 0xF2, 0xFF, 0x06, 0x5B, 0xF3,
    0xFC, 0xFD, 0x5B, 0xF4, 0x02, 0x08, 0x64, 0xF5, 0x01, 0x08, 0x64, 0xF6, 0xFF, 0x07, 0x64, 0xF7,
    0x00, 0x06, 0x64, 0xF8, 0x03, 0x08, 0x64, 0xF9, 0x01, 0x08, 0x64, 0xFA, 0x00, 0x07, 0x64, 0xFB,
    0x02, 0x06, 0x64, 0xFC, 0x04, 0xFB, 0x65, 0xFD, 0x04, 0x07, 0x5B, 0x68, 0x08, 0x07, 0x5B, 0x69,
    0x09, 0x06, 0x5B, 0x6A, 0xFC, 0x07, 0x64, 0x70, 0xF9, 0x07, 0x64, 0x71, 0xF6, 0x03, 0x65, 0x72,
    0x06, 0x04, 0xA2, 0x68, 0x08, 0x04, 0xA2, 0x69, 0x0A, 0x03, 0xA2, 0x6A, 0xFB, 0x04, 0xA5, 0x70,
    0xF8, 0x04, 0xA5, 0x71, 0xF6, 0x03, 0xA5, 0x72, 0xFE, 0x09, 0x5C, 0xEC, 0xFF, 0x09, 0x5C, 0xED,
    0x01, 0x08, 0x5C, 0xEE, 0x02, 0x08, 0x64, 0xF5, 0x01, 0x08, 0x64, 0xF6, 0x00, 0x06, 0x64, 0xF8,
    0xFB, 0xFA, 0x59, 0x00, 0xFB, 0xFD, 0x58, 0x00, 0xFB, 0x01, 0x58, 0x00, 0xFA, 0xFF, 0x97, 0x00,
    0xFA, 0x05, 0x97, 0x00, 0xFA, 0x0A, 0x97, 0x00, 0xFC, 0xF9, 0xA4, 0x00, 0xFB, 0xFE, 0xA4, 0x00,
    0xFC, 0x03, 0xA4, 0x00, 0xFA, 0xFD, 0x66, 0x00, 0xFD, 0xF9, 0x66, 0x00, 0xFF, 0xF8, 0x66, 0x00,
    0xF8, 0x05, 0x97, 0x00, 0xFB, 0x00, 0x97, 0x00, 0xFD, 0xFE, 0x97, 0x00, 0xFB, 0xFC, 0xA4, 0x00,
    0xFD, 0xF8, 0xA4, 0x00, 0xFF, 0xF8, 0xA4, 0x00, 0x00, 0xF2, 0x96, 0x00, 0x00, 0xF7, 0xA1, 0x00,
};

static unsigned objOverlayRow(unsigned frame)
{
    /* the image's table at DS:0x3570 runs to frame 0x119 (0x29A bytes before
     * the 0x368A index table); frames past it read row 0, as the port's
     * frames 0x11A..0x11D do. */
    return frame < 0x11Au ? objFrameRow[frame] : 0u;
}

/* The base tile of one frame at a world position (select_object_tile
 * 1000:6625's draw): the sprite table entry decides the mode, the row count and
 * the pixel bias. */
static void objBlitFrame(uint8_t *page, unsigned frame, int x, int y)
{
    const ObjSprite *sp;
    unsigned cols;

    if (frame >= OBJ_FRAME_SLOTS) return;
    sp = &objSprites[frame];
    if (!sp->rows || sp->mode > 2u) return;          /* blank / mode 0 or 3 */
    cols = sp->mode == 2u ? 32u : 16u;               /* 1000:6C74 vs 1000:66EC */
    if ((uint32_t)sp->off + sp->rows * cols > objBankSize) return;
    blitSprite(page, objBank + sp->off, sp->rows, cols, x, y + (int)sp->ybias);
}

/* 1000:653C draw_object_cell's class half: the class id names both the
 * behaviour class (0x3E5E) and the placed record (0x3F17 + 0x12*(id-1)) that
 * carries the extra sprite.  That record's x/y/frame are overwritten with the
 * carrier-relative position, as the original does - it is a per-frame cache,
 * not a scratch copy. */
static void objDrawClassOverlay(uint8_t *page, int scrollX, int scrollY, uint8_t *rec)
{
    unsigned cls = rec[7];
    unsigned col, row, idx, x, y, frame;
    const uint8_t *e;
    uint8_t *carried;

    if (!cls) return;
    /* The original indexes 0x3E5F + (cls-1) with no range test, so a class id
     * above the table reads whatever follows it in DS; the port bounds it. */
    if (cls > sizeof objSlotClass) return;
    /* 1000:6570: CX = rec[+7]; SUB CX,1; SI = 0x3E5F + CX, i.e. the slot at
     * 0x3E5E + class - objSlotClass is based at 0x3E5E (the predicate
     * 1000:3659 reads 0x3E5E + AX and the port reads objSlotClass[b]). */
    col = objClsCol[objSlotClass[cls] & 0x3Fu];              /* 1000:657F */
    if (!col) return;
    frame = rd16(rec, 4u);
    row = objOverlayRow(frame) & 0x7Fu;                      /* 1000:65A7 */
    if (!row) return;
    idx = objOverlayIdx[3u * row + col];                     /* 1000:65C2 */
    if (!idx) return;
    e = objOverlayEnt + 4u * idx;                            /* 1000:65D5 */
    x = rd16(rec, 0u);
    y = rd16(rec, 2u);
    if (e[2]) {                                              /* 1000:65E4 */
        if (8u + cls < OBJ_SLOTS) {
            carried = objArray + (8u + cls) * OBJ_STRIDE;
            wr16(carried, 0u, (uint16_t)(x + (uint16_t)(int16_t)(int8_t)e[0]));
            wr16(carried, 2u, (uint16_t)(y + (uint16_t)(int16_t)(int8_t)e[1]));
            wr16(carried, 4u, e[2]);
            objBlitFrame(page, e[2], (int)(int16_t)rd16(carried, 0u) - scrollX,
                         (int)(int16_t)rd16(carried, 2u) - scrollY);
        }
    }
    if (e[3])                                                /* 1000:6605 */
        objBlitFrame(page, e[3], (int)(int16_t)x - scrollX,
                     (int)(int16_t)y - scrollY);
}

/* The one blitter, for the entity pass (1000:5C5F -> 1000:6C74): the same law,
 * mode 2's 32-px cells. */
void object_blitSprite(uint8_t *page, const uint8_t *src, unsigned rows,
                       unsigned cols, int x, int y)
{
    blitSprite(page, src, rows, cols, x, y);
}

/* One placed/live record through the object drawer.  `scan` selects the
 * scan_object_rows path (slots 8+): its class slot 0x3E5E + i-8 can mark the
 * record taken, and a +7 == 0 record is drawn straight through
 * select_object_tile (1000:6625) with no 0x3570 gate.  The list passes
 * (1000:6495 -> 1000:653C) gate the base on 0x3570 instead. */
static void objDrawRecord(uint8_t *page, int scrollX, int scrollY,
                          unsigned i, int scan)
{
    uint8_t *r = objArray + i * OBJ_STRIDE;
    int x = (int)(int16_t)rd16(r, 0u) - scrollX;
    int y = (int)(int16_t)rd16(r, 2u) - scrollY;
    unsigned frame = rd16(r, 4u);

    if (r[6] & 0x80u) return;                       /* 1000:794C / 1000:6458 */
    /* 1000:6450/1000:6467: the record's class slot (0x3E5E + record-8)
     * skips it when bit 7 is set - the pickup's taken mark (1000:9D47).
     * The carried record is drawn by its carrier's class overlay only. */
    if (scan && (objSlotClass[i - 8u] & 0x80u)) return;
    if (!r[7]) {
        /* 1000:653C skips only the EARLY base draw when 0x3570[frame] bit 7
         * is set; its tail (1000:6610) draws the base at 1000:661C instead.
         * With no class overlay the record is always drawn exactly once, so
         * the climb frames 0x23..0x25 (0x3570 = 0xC3/0xC4/0xC5) must draw. */
        objBlitFrame(page, frame, x, y);
        return;
    }
    if (!(objOverlayRow(frame) & 0x80u)) objBlitFrame(page, frame, x, y);
    objDrawClassOverlay(page, scrollX, scrollY, r);
    if (objOverlayRow(frame) & 0x80u) objBlitFrame(page, frame, x, y);
}

/* 1000:C23C partition_entities_by_type: the eight placed slots split by their
 * type word `+8`; 6..8 (the human-stack states - grab 6, push/stand 7,
 * step-off/register 8) go to the 0x651E list drawn **after** the live
 * records, everything else to the 0x650C list drawn **before** them.  The
 * acting record is skipped in the slot walk and appended last in its own
 * class, so it is the last drawn there. */
static void objPartition(uint8_t *back, unsigned *nb,
                         uint8_t *front, unsigned *nf)
{
    unsigned b = 0, f = 0;

    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *r = objArray + i * OBJ_STRIDE;
        unsigned t;

        if (r[6] & 0x80u) continue;
        if (i == objEntity) continue;
        t = rd16(r, 8u);
        if (t >= 6u && t <= 8u) front[f++] = (uint8_t)i;
        else back[b++] = (uint8_t)i;
    }
    {
        unsigned t;

        if (objEntity >= OBJ_SLOTS) { *nb = b; *nf = f; return; }
        t = rd16(objArray + objEntity * OBJ_STRIDE, 8u);
        if (t >= 6u && t <= 8u) front[f++] = (uint8_t)objEntity;
        else back[b++] = (uint8_t)objEntity;
    }
    *nb = b;
    *nf = f;
}

/* render_objects' object pass (1000:5D56 -> draw_object_group 1000:6421):
 * the 0x650C list, then scan_object_rows (slots 8..[0x3E73]), then the
 * 0x651E list. */
void object_render(uint8_t *page, int scrollX, int scrollY)
{
    uint8_t back[9], front[9];
    unsigned nb = 0, nf = 0;

    if (!page || !objBank) return;

    objPartition(back, &nb, front, &nf);            /* 1000:C23C */

    for (unsigned k = 0; k < nb; k++)               /* 1000:6429 */
        objDrawRecord(page, scrollX, scrollY, back[k], 0);

    /* scan_object_rows (1000:6433): AX = [0x3E73] - 8, then `DEC AX / JNS`,
     * so the walk covers 0x3F05 .. the record at [0x3E73] INCLUSIVE -
     * `[0x3E73]` counts the top live index, not the number of records
     * (level 1: 9 draws slots 8 and 9). */
    for (unsigned i = 8u; i <= objLiveCount && i < OBJ_SLOTS; i++)
        objDrawRecord(page, scrollX, scrollY, i, 1);

    for (unsigned k = 0; k < nf; k++)               /* 1000:642E */
        objDrawRecord(page, scrollX, scrollY, front[k], 0);
}


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
static uint8_t objTerSlot[2][0x14];

void object_terSlotInit(void)
{
    memset(objTerSlot, 0, sizeof objTerSlot);        /* 1000:4CB2.. */
    wr16(objTerSlot[0], 0x10u, 0xFFFFu);             /* 1000:4CC3 [0x64AA] */
    wr16(objTerSlot[1], 0x10u, 0xFFFFu);             /* 1000:4CC6 [0x64BE] */
}

uint8_t *object_terSlotAt(unsigned i) { return i < 2u ? objTerSlot[i] : NULL; }

/* the index table at DS:0x2D7E (the caller's `+0xC` word goes through it) */
static const uint8_t objTerIndex[20] = {
    0x00, 0x01, 0x02, 0x03, 0x03, 0x02, 0x04, 0x05, 0x06, 0x07,
    0x06, 0x05, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x92, 0x2D,
};
static const uint8_t *objTerBank;
static uint32_t objTerSize;

void object_publishTer(const uint8_t *bank, uint32_t size)
{
    objTerBank = bank;
    objTerSize = size;
}

/* 1000:5C2F + the 0x649A loop of 1000:5823. */
void object_renderTer(uint8_t *page, int scrollX, int scrollY)
{
    if (!page || !objTerBank) return;
    for (unsigned s = 0; s < 2u; s++) {
        uint8_t *e = objTerSlot[s];
        unsigned index, idx, src, yoff, rows;

        if (rd16(e, 0x10u) & 0x8000u) continue;      /* 1000:587F */
        index = rd16(e, 0xCu);
        idx = index < sizeof objTerIndex ? objTerIndex[index] : 0u;
        if (4u * idx + 4u > objTerSize || 2u * idx + 0x32u > objTerSize) continue;
        src = (unsigned)((objTerBank[4u * idx + 2u] << 8) | objTerBank[4u * idx + 3u]) + 0x40u;
        yoff = (unsigned)((objTerBank[2u * idx + 0x20u] << 8) | objTerBank[2u * idx + 0x21u]);
        rows = (unsigned)((objTerBank[2u * idx + 0x30u] << 8) | objTerBank[2u * idx + 0x31u]);
        if (src + rows * 8u > objTerSize) continue;
        object_blitSprite(page, objTerBank + src, rows, 32u,
                          (int)(int16_t)rd16(e, 0u) - 0xCu - scrollX,
                          (int)(int16_t)rd16(e, 2u) + (int)yoff - scrollY);
    }
}

/* --- object_test_dispatch (1000:35D6) ---------------------------------
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
int object_testDispatch(uint16_t mode)
{
    unsigned op = mode & 0x7Fu;
    unsigned arg = (mode >> 8) & 0xFFu;

    switch (op) {
    case 0: return object_lowPhaseMatch() == -1;      /* 1000:3600 */
    case 1: return object_noneType1d() == -1;         /* 1000:3640 */
    case 2: return object_classMatches(arg) == -1;    /* 1000:3659 */
    case 3: return object_typeEquals(arg) == -1;      /* 1000:3678 */
    case 4: return object_entitiesFinished() == -1;   /* 1000:3688 */
    case 5: return object_mapCellEquals(arg) == -1;   /* 1000:36AB */
    default: return 0;
    }
}

/* --- object_tick: one level frame for the object machine -------------- */
void object_tick(const uint8_t *levelPal)
{
    objFrame++;
    object_animatorTick();
    object_claimTick(levelPal);
}

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
 *   4  0x3A05 -> 0xEFA1/0xEFB9                                  (per-type, not decoded)
 *   5  0x39B5 type += 0x1F and frame = 0x10E + 4*type
 *   7  0x39CF -> 0xEA0F/0xEA4D                                  (per-type, not decoded)
 *   8  0x39CC -> 0xEA71 `reset_span_entry_on_phase`
 *   9  0x39E4 the player claims (AX = 2), otherwise 0xEF0C/0xEF39  (not decoded)
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
    OP_REGISTER = 1, OP_COLLIDE_A = 2, OP_SELECT = 3, OP_TYPE_A = 4, OP_SETFRAME = 5,
    OP_COLLIDE_B = 6, OP_TYPE_B = 7, OP_RESET = 8, OP_CLAIM_OR_TYPE = 9,
    OP_CLAIM3 = 10, OP_CLAIM0 = 11, OP_CLAIM1 = 12
};

/* the six programs at 0x5B37..0x5B53 (202F image, 12 bytes each). */
static uint8_t objPrograms[6][12] = {
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
 * bar's action-row group header.  Only 4..7 are reachable - 0x5B81 marks
 * 0..3 with bit 7 and 1000:9C9C skips them. */
static const uint8_t objBehProgram[8] = { 0, 0, 0, 0, 5, 1, 3, 2 };
static const uint8_t objBehDir[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 1, 1 };
static const uint8_t objBehCollideProg[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 6, 2, 4, 3 };
/* DS:0x5B97 (behaviour -> the class path's handler index) and its target
 * table at 0x5BA0: {0x9D94, 0x9DD7}.  DS:0x5BA4 is the throw's x offset by
 * the actor's type (0 -> +8, 1 -> -8, everything else 0) and DS:0x5BAA the
 * frame by behaviour (5 -> 0x60, 6 -> 0x9C, 7 -> 0x96). */
static const uint8_t objBehClassIdx[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 1, 1 };
static const int8_t  objThrowDx[8] = { 8, -8, 0, 0, 0, 0, 0, 0 };
static const uint8_t objDropFrame[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x9C, 0x96 };
/* DS:0x6EC3, 16 bytes: object_find_overlap's "this type does not take part"
 * flag.  Types 0..2 and 8 are 0 (they *do*), the rest 1.  Types >= 0x10 read
 * past the table in the original; the port treats them as "no". */
static const uint8_t objTypeOverlaps[16] = {
    0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1
};

unsigned object_actionEntity(void) { return objEntity; }

/* 1000:3344..336C (level_fade_in): slot 8 (`0x3F05`) takes frame 0x17, type 0x1A,
 * +0xA = 0x1006 and +0xC = 0xFF.  `scan_object_rows` starts its walk there and
 * counts `[0x3E73]-8+1` records, so this slot is drawn.
 * Slot 8 is the active-human ARROW: 1000:D64F (object_markerTick, called from
 * update_world) positions and bobs it every frame, and the selector arms it in
 * 1000:4188 (objMarkerArm), so the x/y the array init leaves at 0xFFFF are
 * replaced on the first level frame. */
void object_levelStartRecord(void)
{
    uint8_t *r = object_slot(8u);

    if (!r) return;
    wr16(r, 4u, 0x0017u);
    wr16(r, 6u, 0x0000u);
    wr16(r, 8u, 0x001Au);
    wr16(r, 0xAu, 0x1006u);
    /* The next instruction of level_fade_in, `MOV byte ptr [0xc], 0xff` at
     * 1000:336C, is the GLOBAL update_world gate (level.c's worldUpdates) -
     * not this record's +0xC, which stays 0 as the asm leaves it. */
}

/* 1000:4188 (the selector's tail): the selection arms the active-human marker,
 * record 8, on the new acting record.  The original also writes
 * [0x6DEA] = 0x3E8 (the one-shot command timer). */
static void objMarkerArm(void)
{
    uint8_t *m = object_slot(8u);
    uint8_t *r = objArray + objEntity * OBJ_STRIDE;

    if (!m) return;
    wr16(m, 0u, rd16(r, 0u));
    wr16(m, 2u, (uint16_t)(rd16(r, 2u) - 0x1Au));
    wr16(m, 6u, 0u);
    wr16(m, 8u, 0x001Au);
    wr16(m, 0xAu, 0x0605u);
    objCmdTimer = 0x03E8u;
}

/* 1000:D64F (update_world's 1000:80C9): record 8 follows the acting record;
 * its type word is subtracted from y and steps 0x1A,0x18,0x16,0x14,0x12 (two
 * every third frame - the bob); a 32-px acting sprite shifts x by +8; and the
 * word at +0xA is the lifetime - when its high byte underflows the marker is
 * disabled (+6 = 0x80) and scan_object_rows stops drawing it. */
void object_markerTick(void)
{
    uint8_t *m = object_slot(8u);
    uint8_t *r = objArray + objEntity * OBJ_STRIDE;
    unsigned type, frame, mode, ax;

    if (!m) return;
    if (m[6] & 0x80u) return;                        /* 1000:D655 */
    wr16(m, 0u, rd16(r, 0u));                        /* 1000:D65B */
    type = rd16(m, 8u);
    wr16(m, 2u, (uint16_t)(rd16(r, 2u) - type));     /* 1000:D65F */
    frame = rd16(r, 4u);
    mode = frame < OBJ_FRAME_SLOTS ? objSprites[frame].mode : 0u;
    wr16(m, 0u, (uint16_t)(rd16(m, 0u) + ((int)mode - 1) * 8));  /* 1000:D66B */
    m[0xAu] = (uint8_t)(m[0xAu] - 1u);               /* 1000:D68F */
    if (!(m[0xAu] & 0x80u)) {                        /* 1000:D693 JNS */
        wr16(m, 8u, type);
        return;
    }
    m[0xAu] = 2u;                                    /* 1000:D695 */
    ax = type - 2u;                                  /* 1000:D699 */
    if (ax >= 0x12u) {                               /* 1000:D69F JNC */
        wr16(m, 8u, ax);
        return;
    }
    ax = 0x001Au;                                    /* 1000:D6A1 */
    m[0xBu] = (uint8_t)(m[0xBu] - 1u);               /* 1000:D6A4 */
    if (m[0xBu] & 0x80u) m[6] = 0x80u;               /* 1000:D6A8 */
    wr16(m, 8u, ax);                                 /* 1000:D6AE */
}

void object_setActionEntity(unsigned i) { objEntity = i < OBJ_STATIC ? i : objEntity; }
unsigned object_actionProgram(void) { return objProgramIndex; }
unsigned object_actionFault(void) { return objFault; }

/* `NULL` detaches the halt (the headless tests exercise op 7 on purpose). */
void object_attachOsys(const OSystem *osys) { objOsys = osys; }
/* The program at 0x5B37 + its own pointer offset, `{count, cursor, icon...}`.
 * Those first two bytes are also the status bar's action-row header: the bar
 * draws this group and SPACE steps byte 1, which is the cursor the script
 * dispatcher reads (1000:3947 writes it, 1000:3987 reads prog[cursor+1]).
 * (The six runs overlap in the image, but every write lands outside the other
 * groups' visible bytes 0..5 - checked against 0x5B2B/0x5B37.) */
const uint8_t *object_programHeader(unsigned group)
{
    return (group >= 1u && group <= 6u) ? objPrograms[group - 1u] : NULL;
}

const uint8_t *object_actionHeader(void) { return object_programHeader(objProgramIndex); }

/* 1000:3933's tail (0x3941-0x394D): advance the selected cell with the wrap to
 * 1 past the count.  It is the program's cursor byte, so the next ACTION press
 * runs the op the bar's highlight shows. */
void object_stepActionCell(void)
{
    uint8_t *p = objPrograms[(objProgramIndex - 1u) % 6u];

    if (!p[0]) return;
    p[1] = (uint8_t)(p[1] + 1u);
    if (p[1] > p[0]) p[1] = 1u;
}

/* [0x5B56] (1-based): the shipped writers only produce 1..6, so this exists
 * for tests and for a future caller that restores a program from a save. */
void object_actionSetProgram(unsigned index) { objSetProgramIndex(index); }
/* [0x1988]: the program cursor byte (the pending action 5 writes it, 0x382D). */
void object_actionSetCursor(unsigned cursor)
{
    uint8_t *p = objPrograms[(objProgramIndex - 1u) % 6u];
    objCursor = cursor;
    p[1] = (uint8_t)cursor;
}
unsigned object_actionPending(void) { return objPending; }

/* 0x3A26 (op 1): register the object in the 0x6F22 table and turn it into type 8. */
static void objOpRegister(uint8_t *actor)
{
    if (rd16(actor, 8u) > 2u) return;
    for (unsigned i = 0; i < 8u; i++) {
        uint16_t *s = objRegister[i];
        if (!(s[0] & 0x8000u)) continue;         /* testw $0x8000 -> free  */
        s[0] = rd16(actor, 0u);
        s[1] = rd16(actor, 2u);
        ((uint8_t *)s)[5] = (uint8_t)objEntity;
        wr16(actor, 4u, 0x0020u);
        wr16(actor, 8u, 0x0008u);
        return;
    }
}

/* 0x39B5 (op 5): the "pick up / change state" frame step. */
static void objOpSetFrame(uint8_t *actor)
{
    unsigned t = rd16(actor, 8u);
    if (t >= 2u) return;
    wr16(actor, 8u, (uint16_t)(t + 0x1Fu));
    wr16(actor, 4u, (uint16_t)(0x010Eu + 4u * t));
}

/* 0x9D38 (direction 1): the found live record is taken by the actor. */
static int objTakeFound(uint8_t *actor, uint8_t *found, uint8_t *slot, unsigned ordinal)
{
    if (rd16(found, 8u) != 0xFFFFu) return 0;
    if (rd16(found, 0xAu) != 0u) return 0;
    actor[7] = (uint8_t)ordinal;                 /* [SI+3] = DL */
    *slot |= 0x80u;                              /* ORB $0x80, [BP] */
    wr16(found, 8u, (uint16_t)objEntity);        /* [DI+4] = [0x3E22] */
    return 1;
}

/* 0x9CF0 (direction 0): the "give / hold" variant. */
static int objGiveFound(uint8_t *actor, uint8_t *found, uint8_t *slot, unsigned ordinal)
{
    if (found[6] & 0x40u) return 0;
    if (!(rd16(found, 0xEu) & 0x8000u)) return 0;
    wr16(found, 0xEu, (uint16_t)objEntity);
    found[7] = 0u;
    *slot |= 0x80u;
    wr16(actor, 4u, 0u);                         /* the actor's own record:  */
    actor[7] = (uint8_t)ordinal;
    wr16(actor, 8u, 0x000Fu);
    wr16(actor, 0xAu, 0u);
    wr16(actor, 0xEu, (uint16_t)((uintptr_t)found - (uintptr_t)objArray + 4u));
    return 1;
}

/* 1000:9D94 (the class path's behaviour-4 handler): the carried record is put
 * down and the acting record becomes a normal record again at its position. */
static void objClassDrop(uint8_t *actor, uint8_t *found, uint8_t *slot)
{
    unsigned ax = 0, bx = 0;

    actor[7] = 0u;                               /* 1000:9D94 */
    *slot &= 0x7Fu;
    wr16(found, 0xEu, 0xFFFFu);
    found[6] &= 0x7Fu;
    if (rd16(found, 8u) & 0x8000u) {             /* 1000:9DAA */
        ax = 1u;
        bx = 8u;
    }
    wr16(actor, 0u, rd16(found, 0u));            /* 1000:9DB7 */
    wr16(actor, 2u, (uint16_t)(rd16(found, 2u) - 8u));
    wr16(actor, 4u, (uint16_t)bx);
    wr16(actor, 6u, 0u);
    wr16(actor, 8u, (uint16_t)ax);
}

/* 1000:9DD7 (behaviours 5..7): the carried record is thrown / placed at the
 * acting record's feet and snapped to the terrain.  The two scratch stores the
 * original makes first ([0x4238] = -6 / 0x16, [0x423C] = 0x10) are dead: every
 * reader of those words writes them itself (1000:650B, 1000:A783). */
static void objClassThrow(uint8_t *actor, uint8_t *found, uint8_t *slot)
{
    unsigned type = rd16(actor, 8u);
    unsigned beh = (unsigned)(*slot & 0x3Fu);
    int landed;
    unsigned cell;

    actor[7] = 0u;                               /* 1000:9DF1 */
    *slot &= 0x7Fu;
    wr16(found, 0u, (uint16_t)(rd16(actor, 0u) +
                               (type < 8u ? objThrowDx[type] : 0)));
    wr16(found, 2u, (uint16_t)(rd16(actor, 2u) + 8u));
    wr16(found, 4u, beh < 8u ? objDropFrame[beh] : 0u);
    wr16(found, 6u, 0u);
    wr16(found, 8u, 0xFFFFu);
    wr16(found, 0xAu, 0u);
    wr16(found, 0xCu, 0u);
    wr16(found, 0xEu, 0u);
    landed = objStepProbeAt(found, 8, 0x10, &cell);  /* 1000:9E43 -> E389 */
    if (landed >= 0) wr16(found, 2u, (uint16_t)(landed - 0xF));
}

/* 1000:9D5D (the class path): the acting record's `+7` is set, so it carries
 * the record whose ordinal that byte holds - `DI = 0x3F17 + 0x12*(class-1)`
 * (the first live record is 0x3F17) and `BP = 0x3E5F + class - 1`.  The
 * carried record's class slot picks the handler through 0x5B97/0x5BA0; the
 * tail then always writes [0x1979] = 5, [0x1988] = 1 and [0x5B56] = 1, i.e.
 * the bar returns to the walk group. */
static void objOpClassPath(uint8_t *actor)
{
    unsigned cls = actor[7];
    unsigned live = objLiveCount > OBJ_LIVE0 ? objLiveCount - OBJ_LIVE0 : 0u;
    uint8_t *found, *slot;
    unsigned beh, idx;

    /* the original indexes with the byte unguarded; past the live records it
     * would read the image, which cannot happen - only the take / give paths
     * write it, always with a valid ordinal. */
    if (cls < 1u || cls > live) return;
    found = objArray + (OBJ_LIVE0 + cls) * OBJ_STRIDE;   /* 1000:9D5D */
    slot = objSlotClass + cls;                           /* 1000:9D60 */
    beh = (unsigned)(*slot & 0x3Fu);
    idx = beh < 8u ? objBehClassIdx[beh] : 0xFFu;        /* 1000:9D76 */
    if (idx & 0x80u) return;                             /* 1000:9D7C */
    if (idx == 0u) objClassDrop(actor, found, slot);
    else           objClassThrow(actor, found, slot);
    objPending = 5u;                                     /* 1000:9D84 */
    objCursor = 1u;                                      /* 1000:9D89 */
    objSetProgramIndex(1u);                              /* 1000:9D8E */
}

/* 0x9C1B (ops 2/6): the collision pass over the live records. */
static void objOpCollide(uint8_t *actor)
{
    unsigned type = rd16(actor, 8u);
    int ax, bx;
    unsigned live;

    if (!(type == 0x0Fu || type <= 2u)) return;
    if (actor[7]) {                              /* 1000:9C37 -> the class path */
        objOpClassPath(actor);
        return;
    }
    ax = (int16_t)rd16(actor, 0u);
    bx = (int16_t)rd16(actor, 2u);
    live = objLiveCount > OBJ_LIVE0 ? objLiveCount - OBJ_LIVE0 : 0u;

    for (unsigned k = 0; k < live; k++) {
        unsigned ordinal = k + 1u;               /* DX */
        /* DI starts at 0x3F17 = index 9 = OBJ_LIVE0 + 1 (OBJ_LIVE0 is the
         * count's base, 8). */
        uint8_t *found = objArray + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        uint8_t *slot = objSlotClass + ordinal;  /* BP = 0x3E5F + k */
        int dx, dy;
        unsigned beh, dir, prog;

        if (*slot & 0x80u) continue;
        if (found[6] & 0x80u) continue;
        beh = (unsigned)(*slot & 0x3Fu);
        dir = objBehDir[beh];
        if (dir & 0x80u) continue;
        dx = (int16_t)rd16(found, 0u) - ax;
        if (dx < 0) dx = -dx;                    /* SUB/NEG, then < 0x10 */
        if (dx >= 0x10) continue;
        dy = (int16_t)rd16(found, 2u) - bx;      /* 0 <= dy < 0x18 */
        if (dy < 0 || dy >= 0x18) continue;
        if (dir == 0u) { if (!objGiveFound(actor, found, slot, ordinal)) continue; }
        else if (!objTakeFound(actor, found, slot, ordinal)) continue;
        /* the tail at 1000:9CD9: [0x5B56] = [0x423C] = 0x5B8A[behaviour],
         * which is the ACTION program and the bar's action-row group; a
         * negative id leaves all three writes undone (1000:9CDF). */
        prog = objBehCollideProg[beh];
        if (prog & 0x80u) return;
        objPending = 5u;                         /* [0x1979] = 5 */
        objCursor = 1u;                          /* [0x1988] = 1 */
        objSetProgramIndex(prog);                /* [0x5B56] */
        return;
    }
}

/* 0x4147 (resolve_object_sprite): [0x3E22] := index, program from the class. */
static void objResolveSprite(unsigned index)
{
    uint8_t *cur = objArray + objEntity * OBJ_STRIDE;
    uint8_t *r = objArray + index * OBJ_STRIDE;
    unsigned prog = 0u;

    if (rd16(cur, 8u) == 2u) wr16(cur, 4u, 0x0010u);   /* the previous record */
    objEntity = index;
    if (r[7]) prog = objBehProgram[objSlotClass[r[7]] & 0x3Fu];
    if (index == (unsigned)objPlayer) prog = 4u;
    objSetProgramIndex(prog + 1u);
    objPending = 1u;
    objMarkerArm();                                  /* 1000:4188 */
}

/* 1000:38C3 (resolve_object_sprite_guarded): the key list's F1..F8 handler.
 * AX is the scancode and the record is `scancode - 0x3B`; a slot with `+6`
 * bit 7 set or type 0x1D is skipped, otherwise 1000:4147 resolves it.
 * Returns 1 when a record was selected. */
int object_selectByIndex(unsigned index)
{
    uint8_t *r;

    if (index >= OBJ_STATIC) return 0;
    r = objArray + index * OBJ_STRIDE;
    if ((r[6] & 0x80u) || rd16(r, 8u) == 0x1Du) return 0;   /* 1000:38CE */
    objResolveSprite(index);                                /* 1000:38DA */
    return 1;
}

/* 1000:3933 (the key list's id 8, SPACE).  1000:392D runs the home-state
 * conversion on the acting record (get_object_ptr + 0xEDB7) and the handler
 * returns when it converted; otherwise it plays driver effect 0 and steps the
 * action row's selected cell (1000:3941 -> 0x98F8 + the 0x5B2B header's second
 * byte, wrapping to 1 past the cell count) before the bar redraw. */
int object_spaceAction(void)
{
    if (objCmdHomeState(objActor())) return 1;   /* 1000:392D -> 0xEDB7 */
    objDriverCall(0);                            /* 1000:393A */
    statusbar_cycleAction();                     /* 1000:3941 */
    return 1;
}

/* 0x410A (resolve_next_eligible_object_sprite): cycle the 8 static records. */
int object_cycleEntity(int step)
{
    int at = (int)objEntity;

    for (unsigned n = 0; n < OBJ_STATIC; n++) {
        int idx = at + (step < 0 ? -1 : 1);
        uint8_t *r;

        if (idx < 0) idx = (int)OBJ_STATIC - 1;
        if (idx >= (int)OBJ_STATIC) idx = 0;
        r = objArray + idx * OBJ_STRIDE;
        if (!(r[6] & 0x80u) && rd16(r, 8u) != 0x1Du) {
            objResolveSprite((unsigned)idx);
            return idx;
        }
        at = idx;
    }
    return -1;
}

/* DS:0x6EB6 — the per-behaviour *state* byte nop_handler_3 (0xF2CF) gates on:
 * behaviours 5/6/7 -> 1/3/2, everything else 0xFF (never matches a code). */
static const uint8_t objBehState[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 1, 3, 2 };

/* nop_handler_3 (0xF2CF): ZF set when the acting record's behaviour has this
 * state.  The callers are `CALL`/`JNZ`. */
static int objStateIs(uint8_t *actor, unsigned state)
{
    unsigned b = actor[7u];
    if (!b) return 0;
    return (objBehState[objSlotClass[b] & 0x3Fu] == state);
}

/* 0xF989 (object_record_walk): the live record with slot 3 (behaviour 3) that
 * sits left of `x + step` and above `y + 0xC`, i.e. -0x10 <= dx < 0 and
 * -0x18 <= dy < 0.  Returns the slot index or -1. */
static int objRecordWalk(uint8_t *actor, unsigned step)
{
    int cx = (int16_t)rd16(actor, 2u) + 0x0C;
    int bx = (int16_t)rd16(actor, 0u) + (int)step;
    unsigned live = objLiveCount > OBJ_LIVE0 ? objLiveCount - OBJ_LIVE0 : 0u;

    for (unsigned k = 0; k < live; k++) {
        uint8_t *r = objArray + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        int dx, dy;

        if ((objSlotClass[1u + k] & 0x3Fu) != 3u) continue;
        if (r[6] & 0x80u) continue;
        if (rd16(r, 8u) == 2u) continue;
        dx = (int16_t)rd16(r, 0u) - bx;
        if (!(dx < 0 && dx + 0x10 >= 0)) continue;
        dy = (int16_t)rd16(r, 2u) - cx;
        if (!(dy < 0 && dy + 0x18 >= 0)) continue;
        return (int)(OBJ_LIVE0 + 1u + k);       /* BP = the record           */
    }
    return -1;
}

/* 0xEFA1 / 0xEFB9 (op 4): set the record's fixed state, gated on the
 * behaviour's state byte being 1 (behaviour 5, the spear). */
static void objOpSetState(uint8_t *actor, unsigned type)
{
    if (!objStateIs(actor, 1u)) return;
    if (type == 0u) {
        wr16(actor, 4u, 0x00BBu);
        wr16(actor, 8u, 0x0017u);
    } else {
        wr16(actor, 4u, 0x00BEu);
        wr16(actor, 8u, 0x0018u);
    }
    /* [0x6F1D] = 0 (the dialog flag) — the port has no separate flag */
}

/* 0xF2E0 (object_find_overlap): the static record within |dx|,|dy| <= 0x10 of
 * the actor whose type is "overlappable" ([0x6EC3 + type] == 0) becomes frame
 * 0x103, type 0x1E and params 6/0/0xA.  A record that is the player's is
 * replaced by the actor for the conversion ([0x4254]'s swap).  The original
 * runs 0xF453 scan_groups_for_record (DX = 5) before the conversion: the group
 * whose member list holds the acting record is dissolved. */
static void objGroupDissolve(uint8_t *e);
int object_stackScanDissolve(unsigned recIndex);

static void objFindOverlap(uint8_t *actor)
{
    int ax = (int16_t)rd16(actor, 0u);
    int bx = (int16_t)rd16(actor, 2u);

    (void)object_stackScanDissolve((unsigned)(actor - objArray) / OBJ_STRIDE);

    for (unsigned i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = objArray + i * OBJ_STRIDE;
        uint8_t *found;
        unsigned type;
        int dx, dy;

        if (r == actor) continue;
        if (r[6] & 0x80u) continue;
        dx = (int16_t)rd16(r, 0u) - ax;
        if (dx < 0) dx = -dx;
        if (dx > 0x10) continue;
        dy = (int16_t)rd16(r, 2u) - bx;
        if (dy < 0) dy = -dy;
        if (dy > 0x10) continue;
        found = (i == (unsigned)objPlayer) ? actor : r;
        type = rd16(found, 8u);
        if (type < 16u && objTypeOverlaps[type]) continue;
        /* 1000:F453 scan_groups_for_record (DX = 5) -> 1000:FA13: the group
         * whose member list holds this record is dissolved */
        wr16(found, 4u, 0x0103u);
        wr16(found, 8u, 0x001Eu);
        wr16(found, 0xAu, 0x0006u);
        wr16(found, 0xCu, 0x0000u);
        wr16(found, 0xEu, 0x000Au);
    }
}

/* 0xEF0C / 0xEF39 (op 9, non-player): mark the walked record as state 1,
 * gated on the behaviour's state byte being 3 (behaviour 6), then run
 * object_find_overlap (0xF2E0). */
static void objOpMarkWalk(uint8_t *actor, unsigned step)
{
    int slot;

    if (!objStateIs(actor, 3u)) return;
    slot = objRecordWalk(actor, step);
    if (slot < 0) return;
    {
        uint8_t *r = objArray + (unsigned)slot * OBJ_STRIDE;
        if (rd16(r, 8u) == 1u) return;
        wr16(r, 8u, 0x0001u);
        wr16(r, 0xAu, 0x0003u);
        wr16(r, 0xCu, 0x0000u);
    }
    objFindOverlap(actor);
}


/* DS:0x6908, DS:0x698A, DS:0x6918 — the command dispatch's three tables
 * (1000:E3F6), copied verbatim from the image (file offset = DS offset + 0x106F0):
 *  - objCmdKey      the input bit (low nibble of [0x5ACE]) -> command byte: bit 7
 *                   = no handler, bit 6 = one-shot, low 6 bits = the command
 *                   number; the 4 bytes past the 12-byte table are the 0x6914
 *                   tail word the original reads for indexes 12..15, copied too;
 *  - objCmdRows     the per-type rows, 32 bytes each (two 16-byte halves, bit 7
 *                   of the input picks the second), ending exactly at 0x6DEA;
 *  - objCmdHandlers the 57 handler addresses, index n = word at 0x6918+2(n-1).
 * Index 2 is 1000:E609 (its tail holds the human-stack push), 10/11 the leaves,
 * 14 the snap — re_map.md, "What selects the stack's verbs". */
static const uint16_t objCmdHandlers[57] = {
    0xE4F8u, 0xE609u, 0xE6FBu, 0xE72Au, 0xE764u, 0xE790u,
    0xE7C0u, 0xE802u, 0xE831u, 0xE860u, 0xE8DDu, 0xE93Du,
    0xE952u, 0xE963u, 0xE99Du, 0xEA0Eu, 0xEA0Fu, 0xEA4Du,
    0xEA71u, 0xEAACu, 0xEAE6u, 0xEB5Cu, 0xEB6Au, 0xEB78u,
    0xEBA0u, 0xEBCAu, 0xEBD0u, 0xEC98u, 0xEC99u, 0xECA8u,
    0xECB7u, 0xECFEu, 0xED4Fu, 0xED69u, 0xED83u, 0xED9Du,
    0xEDB7u, 0xEE07u, 0xEE3Bu, 0xEF0Au, 0xEF0Bu, 0xEF0Cu,
    0xEF39u, 0xEF66u, 0xEFA1u, 0xEFB9u, 0xEFD1u, 0xEFD4u,
    0xEFD7u, 0xF067u, 0xF078u, 0xF08Du, 0xF0A2u, 0xF0B3u,
    0xF0C3u, 0xF0C6u, 0xF0C9u,
};

static const uint8_t objCmdKey[16] = {
    0x48u, 0x00u, 0x04u, 0xFFu, 0x02u, 0x01u, 0x03u, 0xFFu, 0x06u, 0x07u, 0x05u, 0x00u, 0xC7u, 0xD9u, 0x00u, 0x00u,
};

static uint8_t objCmdRows[35][32] = {
    /* type  0 */ {0xFFu, 0x01u, 0x05u, 0x05u, 0x05u, 0x02u, 0x04u, 0x04u, 0x04u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0xFFu, 0x00u, 0x00u, 0x1Du, 0x00u, 0x11u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type  1 */ {0xFFu, 0x01u, 0x06u, 0x06u, 0x06u, 0x02u, 0x03u, 0x03u, 0x03u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0xFFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u, 0x1Eu, 0x00u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type  2 */ {0x00u, 0x01u, 0x00u, 0x06u, 0x00u, 0x02u, 0x00u, 0x04u, 0x00u, 0x07u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type  3 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xE8u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xE8u, 0xD9u, 0x00u, 0x00u,},
    /* type  4 */ {0x00u, 0x00u, 0x00u, 0x35u, 0x00u, 0x02u, 0x00u, 0x34u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0xE8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type  5 */ {0x00u, 0x01u, 0x00u, 0x35u, 0x00u, 0x00u, 0x00u, 0x34u, 0x00u, 0x00u, 0x00u, 0x01u, 0x31u, 0xE8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type  6 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x60u, 0xE8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x60u, 0xE8u, 0x00u, 0x00u,},
    /* type  7 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xDDu, 0xE8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xDDu, 0xE8u, 0x00u, 0x00u,},
    /* type  8 */ {0x00u, 0x0Eu, 0x00u, 0x0Du, 0x00u, 0x0Fu, 0x00u, 0x0Cu, 0x00u, 0x00u, 0x00u, 0x12u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type  9 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x48u, 0xDBu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x11u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x48u, 0xDBu, 0x00u, 0x00u,},
    /* type 10 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x61u, 0xDBu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x61u, 0xDBu, 0x00u, 0x00u,},
    /* type 11 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xACu, 0xEAu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xACu, 0xEAu, 0x00u, 0x00u,},
    /* type 12 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xE6u, 0xEAu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xE6u, 0xEAu, 0x00u, 0x00u,},
    /* type 13 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x5Cu, 0xEBu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x5Cu, 0xEBu, 0x00u, 0x00u,},
    /* type 14 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x6Au, 0xEBu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x6Au, 0xEBu, 0x00u, 0x00u,},
    /* type 15 */ {0x00u, 0x00u, 0x19u, 0x19u, 0x19u, 0x00u, 0x18u, 0x18u, 0x18u, 0x1Au, 0x00u, 0x00u, 0x1Fu, 0xDCu, 0x00u, 0x00u, 0x00u, 0x00u, 0x1Bu, 0x1Bu, 0x1Bu, 0x00u, 0x1Bu, 0x1Bu, 0x1Bu, 0x00u, 0x00u, 0x00u, 0x1Fu, 0xDCu, 0x00u, 0x00u,},
    /* type 16 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x93u, 0xDDu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x00u, 0x00u, 0x93u, 0xDDu, 0x00u, 0x00u,},
    /* type 17 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xB7u, 0xECu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xB7u, 0xECu, 0x00u, 0x00u,},
    /* type 18 */ {0x00u, 0x27u, 0x27u, 0x22u, 0x25u, 0x25u, 0x25u, 0x21u, 0x27u, 0x27u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x26u, 0x26u, 0x26u, 0x25u, 0x25u, 0x25u, 0x26u, 0x26u, 0x26u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type 19 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x0Au, 0xEFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x0Au, 0xEFu, 0x00u, 0x00u,},
    /* type 20 */ {0x00u, 0x27u, 0x27u, 0x24u, 0x27u, 0x25u, 0x27u, 0x23u, 0x27u, 0x27u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x26u, 0x26u, 0x26u, 0x25u, 0x25u, 0x25u, 0x26u, 0x26u, 0x26u, 0x00u, 0x02u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type 21 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x0Bu, 0xEFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x0Bu, 0xEFu, 0x00u, 0x00u,},
    /* type 22 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x66u, 0xEFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x66u, 0xEFu, 0x00u, 0x00u,},
    /* type 23 */ {0x00u, 0x31u, 0x31u, 0x31u, 0x25u, 0x2Fu, 0x25u, 0x31u, 0x31u, 0x31u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x30u, 0x30u, 0x30u, 0x25u, 0x25u, 0x25u, 0x30u, 0x30u, 0x30u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type 24 */ {0x00u, 0x31u, 0x31u, 0x31u, 0x25u, 0x2Fu, 0x25u, 0x31u, 0x31u, 0x31u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x30u, 0x30u, 0x30u, 0x25u, 0x25u, 0x25u, 0x30u, 0x30u, 0x30u, 0x00u, 0x03u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type 25 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x67u, 0xF0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x67u, 0xF0u, 0x00u, 0x00u,},
    /* type 26 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x78u, 0xF0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x78u, 0xF0u, 0x00u, 0x00u,},
    /* type 27 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC7u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC7u, 0xD9u, 0x00u, 0x00u,},
    /* type 28 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xDBu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xDBu, 0x00u, 0x00u,},
    /* type 29 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xDAu, 0xDDu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xDAu, 0xDDu, 0x00u, 0x00u,},
    /* type 30 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x7Eu, 0xDEu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x7Eu, 0xDEu, 0x00u, 0x00u,},
    /* type 31 */ {0x00u, 0x00u, 0x00u, 0x37u, 0x00u, 0x36u, 0x00u, 0x38u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC8u, 0xD9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC8u, 0xD9u, 0x00u, 0x00u,},
    /* type 32 */ {0x00u, 0x00u, 0x00u, 0x37u, 0x00u, 0x36u, 0x00u, 0x38u, 0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xD9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xCDu, 0xD9u, 0x00u, 0x00u,},
    /* type 33 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC9u, 0xF0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC9u, 0xF0u, 0x00u, 0x00u,},
    /* type 34 */ {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xD1u, 0xDEu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xD1u, 0xDEu, 0x00u, 0x00u,},
};

/* 1000:E3F6's input normalisation: swap bits 2 and 3, drop bits 4..6, then kill
 * each contradictory pair (0x3 / 0xC) the way the original does. */
static uint8_t objNormaliseInput(uint8_t in)
{
    uint8_t v = (uint8_t)((in & 0x83u) | ((in & 0x08u) >> 1) | ((in & 0x04u) << 1));

    if ((v & 0x03u) == 0x03u) v &= 0x8Du;
    if ((v & 0x0Cu) == 0x0Cu) v &= 0x8Bu;
    return v;
}

/* 1000:E3F6 dispatch_actor_command: the acting record's command for one input
 * word.  The original calls the returned handler, or the row's tail handler when
 * it is 0 and the handler did not change the record's type; the handlers are the
 * next objective, so only the selection (and the one-shot counter's two writes)
 * is ported.  One global counter, not one per command: [0x6DEA]. */
ObjCommand object_commandSelect(uint8_t *rec, uint8_t input)
{
    ObjCommand out;
    uint8_t v = objNormaliseInput(input);
    uint8_t cmd = objCmdKey[v & 0x0Fu];
    const uint8_t *row;
    unsigned type = rd16(rec, 8u);           /* [SI+8] */
    unsigned idx;

    out.handler = 0u;
    out.tail = 0u;
    out.hook = 0u;
    out.command = 0u;
    if (type >= 35u) return out;             /* no row: the original reads 0x6DEA */
    row = objCmdRows[type] + ((v & 0x80u) ? 0x10u : 0u);
    objRowOfs = (unsigned)(row - &objCmdRows[0][0]);   /* DI at 1000:E467 */
    out.tail = rd16(row, 0xCu);
    out.hook = row[0xAu];
    if (cmd & 0x80u) return out;             /* bit 7: this input has no command */
    if (cmd & 0x40u) {                       /* bit 6: the 0x6DEA one-shot */
        uint16_t n = objCmdTimer ? objCmdTimer : 0x4B0u;

        if (objCmdTimer && row[0]) {
            n--;
            if (!n) wr16(rec, 8u, 0x0002u);  /* expired: the record is a human */
        }
        objCmdTimer = n;
    }
    cmd &= 0x3Fu;
    out.command = cmd;
    idx = cmd <= 7u ? row[1u + cmd] : 0u;
    if (idx >= 1u && idx <= 57u) out.handler = objCmdHandlers[idx - 1u];
    return out;
}

/* The original's `1 << (7 - n)` on a 16-bit register: for n > 7 the count is
 * negative, and an 8086 SHL by a count >= 16 shifts every bit out, so the bit is
 * 0.  Records 0..7 therefore own 0x80..0x01 and 8..14 own nothing. */
static unsigned objIndexBit(unsigned idx)
{
    return idx <= 7u ? (1u << (7u - idx)) : 0u;
}

/* 1000:F3CA: clear the record's bit from both masks. */
static void objStackUnmark(uint8_t *actor)
{
    uint8_t keep = (uint8_t)~objIndexBit((unsigned)(actor - objArray) / OBJ_STRIDE);

    objStackMask[0] &= keep;
    objStackMask[1] &= keep;
}

/* 1000:F38F test_bits_64ad_64c1: mark the record as being in a stack, in the
 * mask that the list byte below it (`slot[-1]`) is marked in; when that byte is
 * a terminator or unmarked, clear both instead. */
static void objStackMark(uint8_t *actor, const uint8_t *slot)
{
    unsigned below = slot[-1];

    if (!(below & 0x80u)) {
        unsigned bit = objIndexBit(below);

        for (unsigned m = 0; m < 2u; m++) {
            if (objStackMask[m] & bit) {
                objStackMask[m] |= (uint8_t)objIndexBit((unsigned)(actor - objArray) / OBJ_STRIDE);
                return;
            }
        }
    }
    objStackUnmark(actor);
}

/* 1000:FD91: the walk frame step - `y--`, every 5th frame advance the frame by
 * +/-1 (`+0xC` is the direction) and reverse at the `lo`/`hi` bounds.  Both
 * leaves take this on a `y` mismatch. */
static void objFrameStep(uint8_t *actor, unsigned lo, unsigned hi)
{
    unsigned f;

    wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) - 1u));
    wr16(actor, 0xAu, (uint16_t)(rd16(actor, 0xAu) - 1u));
    if ((int16_t)rd16(actor, 0xAu) >= 0) return;
    wr16(actor, 0xAu, 0x0004u);
    f = rd16(actor, 4u);
    if (actor[0xCu] != 0u) {
        if (--f == lo) actor[0xCu] = 0u;
    } else {
        if (++f == hi) actor[0xCu] = 0xFFu;
    }
    wr16(actor, 4u, (uint16_t)f);
}

/* 1000:E697 (a fall-through inside 1000:E609, the walk-up state): push the
 * acting record onto the `0x6F22` group it stands under.  The original reaches
 * this with AX = actor.x and BX = actor.y + 0x18, after the `0x6F92` span scan
 * has failed; the port takes the record and derives the same two values.
 * Returns 1 when a group took it (frame 0x25, type 7, `+0xE` = its slot). */
int object_stackPush(uint8_t *actor)
{
    uint16_t ax = rd16(actor, 0u);
    uint16_t bx = (uint16_t)(rd16(actor, 2u) + 0x18u);

    for (unsigned i = 0; i < 8u; i++) {              /* `CX = 7` + `JNS`: 8 */
        uint8_t *e = (uint8_t *)objRegister[i];
        uint16_t ex, ey, dx;
        uint8_t *di;
        int de;

        ex = rd16(e, 0u);
        if (ex & 0x8000u) continue;               /* free */
        de = (int)(int16_t)ex - (int)(int16_t)ax;
        if (de < 0) de = -de;
        if (de >= 8) continue;                    /* |entry.x - actor.x| < 8 */
        ey = rd16(e, 2u);
        if (bx >= ey) return 0;                   /* `JNC 0xE6FA`: not under it */
        dx = ey;
        di = e + 5;                               /* the owner byte */
        for (;;) {
            dx -= 0x10u;                          /* 16 px per member */
            if (di[0] & 0x80u) { di++; break; }    /* the terminator */
            di++;
        }
        dx += 0x18u;
        if (bx < dx) return 0;                    /* `JC 0xE6FA` */
        di--;                                     /* the terminator's slot */
        *di = (uint8_t)objEntity;                 /* `[0x3E22]` into the list */
        /* the original stores the slot's pointer; the port stores its offset in
         * the register, the same relation the 0x3E24 paths use for records */
        wr16(actor, 0xEu, (uint16_t)((uintptr_t)di - (uintptr_t)objRegister));
        wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) + 1u));
        wr16(actor, 0u, ex);
        wr16(actor, 4u, 0x0025u);
        wr16(actor, 8u, 0x0007u);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        objStackMark(actor, di);                  /* 1000:F38F */
        return 1;
    }
    return 0;
}

/* 1000:FDED object_script_select (the pop): find the acting record in a
 * `0x6F22` list and, if it is the topmost member, remove it.  Returns the
 * original's AX: -1 = not present or not the top, 0 = the record below is within
 * 8 px of `ax` (the caller's x) so the list is left alone, 1 = removed.
 * `*below` = the record the original leaves in DI (NULL when the entry was
 * freed). */
int object_stackPop(uint16_t ax, uint8_t **below, uint8_t **entry)
{
    *below = NULL;
    if (entry) *entry = NULL;
    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        uint8_t *di = e;
        int bl;

        if (entry) *entry = e;                    /* 1000:FDF7: DX = the entry */

        if (rd16(di, 0u) & 0x8000u) continue;     /* free */
        di += 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)objEntity) di++;
        if (di[0] & 0x80u) continue;              /* not in this entry */
        di++;                                     /* past the match */
        if (!(di[0] & 0x80u)) return -1;          /* a member above it */
        bl = (int8_t)di[-2];                      /* the list byte below */
        if (bl >= 0) {
            uint8_t *nb = object_slot((unsigned)bl);
            int dx = nb ? (int)(int16_t)rd16(nb, 0u) - (int)(int16_t)ax : 8;

            if (dx < 0) dx = -dx;
            if (nb && dx < 8) return 0;           /* already aligned: no removal */
        }
        di[-1] = 0xFFu;                           /* terminate at the match */
        bl = (int8_t)di[-2];
        if (bl < 0) {                             /* nothing below: free the entry */
            wr16(di - 6, 0u, 0xFFFFu);
            wr16(di - 6, 2u, 0xFFFFu);
            return 1;
        }
        *below = object_slot((unsigned)bl);
        return 1;
    }
    return -1;
}

/* 1000:E58B (the tail of 1000:E4F8, the step-down state): grab the group the
 * actor stands at - within 8 px in x and 4 px in y - and append it to the
 * member list (frame 0x23, type 6).  With no group matching, register the
 * actor's position in the last free `0x6F22` entry (frame 0x20, type 8).
 * Returns 1 = grabbed, 0 = registered, -1 = nothing (no group, no free entry:
 * the original would write through a stale `[0x423C]`, the port refuses). */
int object_stackGrab(uint8_t *actor)
{
    uint16_t ax = rd16(actor, 0u);
    uint16_t bx = rd16(actor, 2u);
    uint8_t *lastFree = NULL;

    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        uint16_t ex = rd16(e, 0u), ey;
        uint8_t *di;
        int d;

        if (ex & 0x8000u) { lastFree = e + 2; continue; }   /* `[0x423C] = DI` */
        d = (int)(int16_t)ex - (int)(int16_t)ax;
        if (d < 0) d = -d;
        if (d >= 8) continue;                              /* |entry.x - actor.x| < 8 */
        ey = rd16(e, 2u);
        d = (int)(int16_t)ey - (int)(int16_t)bx;
        if (d < 0) d = -d;
        if (d >= 4) continue;                              /* |entry.y - actor.y| < 4 */
        di = e + 5;
        while (!(di[0] & 0x80u)) di++;                     /* to the terminator */
        *di = (uint8_t)objEntity;
        wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) - 1u));
        wr16(actor, 0u, ex);
        wr16(actor, 4u, 0x0023u);
        wr16(actor, 8u, 0x0006u);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        objStackMark(actor, di);                           /* 1000:F38F */
        return 1;
    }
    if (!lastFree) return -1;
    wr16(lastFree, 0u, bx);                                /* entry + 2: y */
    lastFree[3] = (uint8_t)objEntity;                      /* entry + 5: the owner */
    wr16(lastFree - 2, 0u, ax);                            /* entry + 0: x */
    wr16(actor, 4u, 0x0020u);
    wr16(actor, 8u, 0x0008u);
    return 0;
}

/* 1000:E860 (handler 10): step off the group.  Finds the entry with the actor's
 * x, the actor in its list, and on `entry.y - 0x18 == actor.y` moves the actor
 * down 8 (frame 0x20, type 8) and the list byte below it likewise (frame 0x21).
 * Returns 1 = acted, 0 = not in a group, -1 = the `y` mismatch branch (1000:E8D4
 * -> 1000:FD91, the walk frame step). */
int object_stackStepOff(uint8_t *actor)
{
    unsigned idx = (unsigned)((actor - objArray) / OBJ_STRIDE);
    uint16_t ax = rd16(actor, 0u);

    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        uint8_t *di;
        uint16_t ey;

        if (rd16(e, 0u) != ax) continue;
        ey = rd16(e, 2u);
        di = e + 6;                               /* the first member */
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) {
            ey -= 0x10u;                          /* 1000:E896: one level up */
            di++;
        }
        if (di[0] & 0x80u) continue;
        ey -= 0x18u;                              /* 1000:E89B */
        if (ey != rd16(actor, 2u)) {
            objFrameStep(actor, 0x23u, 0x25u);             /* 1000:E8D4 */
            return -1;
        }
        wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) + 8u));
        wr16(actor, 4u, 0x0020u);
        wr16(actor, 8u, 0x0008u);
        if (!(di[-1] & 0x80u)) {
            uint8_t *nb = object_slot(di[-1]);

            if (nb) {
                wr16(nb, 0u, ax);
                wr16(nb, 2u, (uint16_t)(rd16(nb, 2u) + 8u));
                wr16(nb, 4u, 0x0021u);
            }
        }
        return 1;
    }
    return 0;
}

/* 1000:E8DD (handler 11): leave the group.  Same walk, and on `entry.y ==
 * actor.y` the actor becomes a standing human (frame 0x10, type 2) and the list
 * is cut at the slot the push stored in `actor+0xE`.
 * Returns 1 = left, 0 = not in a group, -1 = the `y` mismatch branch (1000:E934
 * -> 1000:FD91). */
int object_stackLeave(uint8_t *actor)
{
    unsigned idx = (unsigned)((actor - objArray) / OBJ_STRIDE);
    uint16_t ax = rd16(actor, 0u);

    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        uint8_t *di;

        if (rd16(e, 0u) != ax) continue;
        di = e + 6;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        if (rd16(e, 2u) != rd16(actor, 2u)) {
            /* 1000:E934: the *down* stepper (y += 1, frames 0x23..0x25) - the
             * member climbs down the column 1 px a step.  (The step-off's
             * 1000:E8D4 is the one that uses the up stepper.) */
            objFrameStepDown(actor, 0x23u, 0x25u);
            return -1;
        }
        wr16(actor, 4u, 0x0010u);
        wr16(actor, 8u, 0x0002u);
        {
            unsigned off = rd16(actor, 0xEu);

            if (off < sizeof objRegister)                  /* 1000:E922 */
                ((uint8_t *)objRegister)[off] = 0xFFu;
        }
        return 1;
    }
    return 0;
}

/* 1000:E99D (index 15): stand back on the group.  The topmost member takes the
 * entry's x, `y -= 7`, frame 0x25 and type 7 - and stores its slot in `+0xE` -
 * while the list byte below it moves up 8 (frame 0x20).  Like the pop, its
 * "below" lookup reads `match-1` literally, so for the owner itself that byte is
 * the entry's `+4` state. */
int object_stackStand(uint8_t *actor)
{
    unsigned idx = (unsigned)((actor - objArray) / OBJ_STRIDE);

    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        uint8_t *di;
        int bl;

        if (rd16(e, 0u) & 0x8000u) continue;
        di = e + 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        di++;
        if (!(di[0] & 0x80u)) return 0;                 /* a member above it */
        di--;                                            /* the match */
        wr16(actor, 0xEu, (uint16_t)((uintptr_t)di - (uintptr_t)objRegister));
        di--;
        bl = (int8_t)di[0];
        if (bl < 0) return 0;
        {
            uint8_t *nb = object_slot((unsigned)bl);

            if (nb) {
                wr16(nb, 2u, (uint16_t)(rd16(nb, 2u) - 8u));
                wr16(nb, 4u, 0x0020u);
            }
        }
        wr16(actor, 0u, rd16(e, 0u));
        wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) - 7u));
        wr16(actor, 4u, 0x0025u);
        wr16(actor, 8u, 0x0007u);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        return 1;
    }
    return 0;
}

/* 1000:E963 (handler 14): snap onto the group's x.  Only the topmost member
 * (the byte after it is a terminator) moves: `actor.x = entry.x`, frame 0x20. */
int object_stackSnap(uint8_t *actor)
{
    unsigned idx = (unsigned)((actor - objArray) / OBJ_STRIDE);

    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        uint8_t *di;

        if (rd16(e, 0u) & 0x8000u) continue;
        di = e + 5;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        di++;
        if (!(di[0] & 0x80u)) return 0;                    /* a member above it */
        wr16(actor, 0u, rd16(e, 0u));
        wr16(actor, 4u, 0x0020u);
        return 1;
    }
    return 0;
}

/* [0x4234] / [0x4238] / [0x423C] - the selector's three parameters.  The
 * wrappers 1000:E93D/E952 set them and jump, and 1000:E6FB/E764 overwrite the
 * frame/type pair for the walk step, so they are state, not arguments. */
static uint16_t objSelXoff, objSelFrame, objSelType;

/* 1000:E361 map_get, the law shared with objMapCell: the map cell at
 * (x, y) is `map[32*(y>>4) + (x>>4)]`; `*sub` returns `x & 0xF`, the 0x92A0
 * sub-column the step test indexes.  Out of the 640-byte map the original reads
 * the neighbouring buffer, the port returns 0xFF. */
static unsigned objMapAtAbs(int x, int y, unsigned *sub)
{
    const uint8_t *map = level_map();
    unsigned cell;

    if (sub) *sub = (unsigned)x & 0xFu;   /* the fine x, when the caller wants it */
    if (!map) return 0xFFu;
    cell = 32u * (((unsigned)y & 0xFFF0u) >> 4) + ((unsigned)x >> 4);
    return cell < 640u ? map[cell] : 0xFFu;
}

/* 1000:E389 map_cell_lookup as a point: (x = AX, y = BX) -> the landed y, with
 * -1 for "blocked".  The cell at the point and its 0x92A0 byte decide the y: a
 * flat cell keeps y, a slope adds its low bits, a solid cell takes the slope of
 * the row below (blocked = -1 when that is solid too) or, for a zero byte, the
 * row above.  The original's third argument is the item table in `BP`; the port
 * reads the same table through `level_pageByte` (see re_map.md, "the handler's
 * BP", for what the dispatcher leaves in BP). */
static int objLandY(int x, int y, unsigned *cellOut)
{
    unsigned sub, id = objMapAtAbs(x, y, &sub);
    unsigned b = level_pageByte(id, sub);

    *cellOut = id;
    if (!(b & 0x80u)) {
        if (b != 0u) return y + (int)(int8_t)b;
        *cellOut = objMapAtAbs(x, y - 0x10, &sub);    /* 1000:E3BA: the row above */
        b = level_pageByte(*cellOut, sub);
        if (b & 0x80u) return y;                      /* 1000:E3CA, AX = 0 */
        return y - 0x10 + (int)b;
    }
    *cellOut = objMapAtAbs(x, y + 0x10, &sub);        /* 1000:E3E7: the row below */
    b = level_pageByte(*cellOut, sub);
    if (b & 0x80u) return -1;                         /* 1000:E3F1 */
    return y + 0x10 + (int)b;
}

/* 1000:E389 from the actor: the cell ahead (x + 8, y + 0x18). */
static int objStepProbeAt(const uint8_t *actor, int dx, int dy, unsigned *cellOut)
{
    return objLandY((int)(int16_t)rd16(actor, 0u) + dx,
                    (int)(int16_t)rd16(actor, 2u) + dy, cellOut);
}

static int objStepProbe(const uint8_t *actor, unsigned *cellOut)
{
    return objStepProbeAt(actor, 8, 0x18, cellOut);
}

/* 1000:F25F: whether the walk can land - the step probe's landing must not
 * rise more than 8 px above the feet (a step-up blocks), and the item record's
 * bit for the sub-cell 0x10 below the actor's y must be clear.  0 = free,
 * 0xFFFF = blocked.  `1000:F25F` loads AX = [0x4238] for the probe, i.e. it is
 * `x + 2` walking left and `x + 0xE` walking right - not the selector's fixed
 * `x + 8` (`1000:F1D7`). */
static unsigned objWalkTest(const uint8_t *actor)
{
    unsigned sub, id, bit, cell;
    int x, y, d;
    int bx = objStepProbeAt(actor, (int)(int16_t)objSelFrame, 0x18, &cell);

    if (bx >= 0) {
        d = bx - 0x18 - (int)(int16_t)rd16(actor, 2u);
        if (d < 0 && d + 8 < 0) return 0xFFFFu;
    }
    x = (int)(int16_t)rd16(actor, 0u) + (int)(int16_t)objSelFrame;
    y = (int)(int16_t)rd16(actor, 2u) + 0x10;
    id = objMapAtAbs(x, y, &sub);
    bit = 1u << (0xFu - ((unsigned)x & 0xFu));
    return (level_itemWord(id, (unsigned)((unsigned)y & 0xFu)) & bit) ? 0xFFFFu : 0u;
}

/* 1000:E6FB (left) / 1000:E764 (right): the walk step.  Sets the result
 * frame/type to 2/0x10 or 0xE/0x10, asks 1000:F25F, then moves x by one and
 * advances the walk frame (8..0xF left, 0..7 right) every fifth frame. */
static void objWalk(uint8_t *actor, int left)
{
    unsigned lo = left ? 8u : 0u, hi = left ? 0x10u : 8u, f;

    objSelFrame = left ? 2u : 0xEu;
    objSelType = 0x10u;
    if (objWalkTest(actor) != 0u) return;             /* blocked: no step */
    wr16(actor, 0u, (uint16_t)(rd16(actor, 0u) + (left ? 0xFFFFu : 1u)));
    wr16(actor, 0xAu, (uint16_t)(rd16(actor, 0xAu) - 1u));
    if ((int16_t)rd16(actor, 0xAu) >= 0) return;
    wr16(actor, 0xAu, 0x0004u);
    f = (unsigned)((rd16(actor, 4u) + 1u) & 0xFFFFu);
    if (f >= hi) f = lo;
    wr16(actor, 4u, (uint16_t)f);
}

/* The settle's static data: the per-type gate at 0x6EBE (32 bytes) and the two
 * 32-byte footing offset tables at 0x689E / 0x68BE, copied from the image. */
static const uint8_t objTypeGate[32] = {
    0x0F, 0xFF, 0xC2, 0x1D, 0xA0, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t objFootA[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x1A, 0x1A, 0x1A, 0x1A,
    0x1A, 0x1A, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C,
    0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x50,
};
static const uint8_t objFootB[32] = {
    0x08, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A,
    0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x3E, 0x3E,
    0x3E, 0x3E, 0x3E, 0x3E, 0x50, 0x50, 0x50, 0x50,
    0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 0x6847: the per-record settled position (4 bytes per record). */
static uint16_t objSettled[OBJ_SLOTS][2];
/* [0x4220] / [0x4222]: the settle's footing, read by 1000:DF2C. */
static uint16_t objFootX, objFootY;

/* The dispatcher's `DI` at the handler/hook call (1000:E467): the type row's
 * selected half, `DS:0x698A + 32*type [+ 0x10]`.  The hooks receive it as an
 * argument register and read it as their "rect" (`1000:F423`), so the port keeps
 * the same byte offset into its copy of the rows.
 *
 * The earlier model read a `0x6F92` span here.  That was wrong: `1000:E467` sets
 * DI to the row and `1000:E4DD` calls `1000:D8BD` with DI untouched, so
 * `1000:F3ED`/`1000:F423` see the **row**, not a span.  The spans at `0x6F92`
 * are read by `1000:E4F8`/`0xE609` (the step verbs) and written by `1000:FC81`,
 * which is why the confusion was possible - see re_history.md. */
static unsigned objRowWord(unsigned off)
{
    const uint8_t *f = &objCmdRows[0][0];
    unsigned i = objRowOfs + off;

    if (i + 1u >= sizeof objCmdRows) return 0u;
    return (unsigned)(f[i] | (f[i + 1u] << 8));
}

/* 1000:F423: is (BX, CX) inside the window the row half at `off` describes?  A
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
static int objRowRectAt(unsigned off, int bx, int cx, uint8_t bit)
{
    int d;

    if (objRowWord(off + 0x10u) & 0x8000u) return 0;         /* 1000:F425 */
    d = (int16_t)objRowWord(off) - 0x10 - bx;                /* 1000:F42C */
    if (d >= 0) return 0;
    d += 0x1A;                                               /* 1000:F433 */
    if (d < 0) return 0;
    d = (int16_t)objRowWord(off + 2u) - cx;                  /* 1000:F438 */
    if (d < 0) return 0;
    d -= 5;
    if (d < 0 || d > 4) return 0;
    if (bit) {
        unsigned i = objRowOfs + off + 0x13u;

        if (i < sizeof objCmdRows) objCmdRows[i / 32u][i % 32u] |= bit;
    }
    return 1;
}

/* The gate's two windows, for the test that shows they never match. */
int object_rowWindowMatches(unsigned type, unsigned half, int x, int y)
{
    unsigned save = objRowOfs;
    int r;

    if (type >= 35u) return 0;
    objRowOfs = 32u * type + (half ? 0x10u : 0u);
    r = objRowRectAt(0u, x, y, 0u) | objRowRectAt(0x14u, x, y, 0u);
    objRowOfs = save;
    return r;
}

/* 1000:F3ED: clear the stack bits, then - unless the actor's type is marked at
 * 0x6EBE - let the two row windows claim it.  Returns 1 when the settle may run
 * (the original's ZF). */
static int objSettleGate(uint8_t *actor)
{
    unsigned t = rd16(actor, 8u);
    uint8_t bit = (uint8_t)(1u << (7u - (t & 7u)));
    int bx = (int16_t)rd16(actor, 0u), cx = (int16_t)rd16(actor, 2u);

    objStackUnmark(actor);                                   /* 1000:F3CA */
    if (objTypeGate[(t >> 3) & 0x1Fu] & bit) return 1;       /* 1000:F450 */
    if (objRowRectAt(0u, bx, cx, bit)) return 0;             /* 1000:F417 */
    if (objRowRectAt(0x14u, bx, cx, bit)) return 0;          /* 1000:F41F */
    return 1;
}

/* 1000:DF2C: the footing landed inside one of the level's 0xA9 sprites?  Marks
 * that sprite's own buffer and returns -1 (the settle then skips its cache). */
static int objBgMark(const uint8_t *actor)
{
    unsigned sub, cell = objMapAtAbs((int)objFootX, (int)objFootY, &sub);
    unsigned idx = (unsigned)((actor - objArray) / OBJ_STRIDE);
    const uint8_t *tbl;
    unsigned bl;

    if (cell < 0xC2u || cell > 0xCFu) return 0;              /* 1000:DF77 */
    for (unsigned i = 0; i < objBgCount; i++) {
        unsigned rx = objBg[i].x, ry = objBg[i].y;
        int d = (int)objFootX - (int)((rx + 0x10u) & 0xFFF0u);

        if ((objFootY & 0xFFF0u) != ((ry + 0x10u) & 0xFFF0u)) continue;
        if (d < 0 || d >= 0x20) continue;
        if (idx < 8u) {
            if (rd16(actor, 8u) > 1u) return -1;             /* 1000:E001 */
            tbl = objFootB;
        } else {
            int side = (int16_t)objSide;
            if (side == 0) return -1;                        /* 1000:DFF8 */
            tbl = side < 0 ? objFootB : objFootA;
        }
        bl = tbl[(unsigned)(objFootX - rx) & 0x1Fu];
        if (bl && objBg[i].dst) objBg[i].dst[bl] = 0xFFu;
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
static void objCmdSettle(uint8_t *actor)
{
    int x = (int16_t)rd16(actor, 0u);
    int y = (int16_t)rd16(actor, 2u);
    int ax = x + 8, feet = y + 0x18, line = feet & ~0xF, bx, d;
    unsigned sub, cell, b;

    if (!objSettleGate(actor)) return;
    cell = objMapAtAbs(ax, line, &sub);
    if (cell == level_specialTile(0u) || cell == level_specialTile(1u)) return;
    b = level_pageByte(cell, sub);
    objSide = 0;
    if (b == 0u) {                                           /* 1000:D91D */
        b = level_pageByte(objMapAtAbs(ax, line - 0x10, &sub), sub);
        if (!(b & 0x80u)) {
            objSide = 0xFFE0;
            line -= 0x10;
        } else {
            b = 0u;
        }
    } else if (b & 0x80u) {                                  /* 1000:D945 */
        line += 0x10;
        objSide = 0x20;
        b = level_pageByte(objMapAtAbs(ax, line, &sub), sub);
        if (b & 0x80u) goto landed;                          /* 1000:D9B2 */
    }
    bx = line + (int)(int8_t)b;                              /* 1000:D967 */
    d = feet - bx;                                           /* 1000:D96A: CX */
    if (d < 0) {
        d = -d;
        if (d > 8) goto landed;
    }
    wr16(actor, 2u, (uint16_t)(bx - 0x18));
    objFootX = (uint16_t)(rd16(actor, 0u) + 8u);
    objFootY = (uint16_t)(rd16(actor, 2u) + 0x18u);
    if (objBgMark(actor) != 0) return;                       /* 1000:D986 */
    objSettled[(unsigned)((actor - objArray) / OBJ_STRIDE)][0] = rd16(actor, 0u);
    objSettled[(unsigned)((actor - objArray) / OBJ_STRIDE)][1] = rd16(actor, 2u);
    return;
landed:
    wr16(actor, 8u, 0x0003u);                                /* 1000:D9B2 */
    wr16(actor, 0xAu, 0x0001u);
    wr16(actor, 0xCu, 0x0004u);
    wr16(actor, 0xEu, 0x0000u);
}

/* 1000:3E73's shade table: the recipe sprite's bytes as a 16x16 table.  Rows
 * 0..ybias-1 stay zero, then `rows` rows copy four bytes per pass; pass p fills
 * `row*16 + p + 4*j`, so the four passes fill every byte of each row. */
static void objBuildShade(unsigned frame, uint8_t *out)
{
    const ObjSprite *sp;
    unsigned s = 0;

    memset(out, 0, 256u);
    if (frame >= OBJ_FRAME_SLOTS) return;
    sp = &objSprites[frame];
    for (unsigned pass = 0; pass < 4u; pass++)
        for (unsigned row = 0; row < 16u; row++) {
            if (row < sp->ybias || row >= sp->ybias + sp->rows) continue;
            for (unsigned j = 0; j < 4u; j++) {
                if ((uint32_t)(sp->off + s) < objBankSize)
                    out[row * 16u + pass + 4u * j] = objBank[sp->off + s];
                s++;
            }
        }
}

/* 1000:E72A (index 4) and 1000:E790 (index 6): the two idle handlers.  Both
 * keep the frame in its cycle (8..0x10 / below 8 and 0x10) with a 4-frame
 * countdown in `+0xA`; the first returns to frame 0x10/type 2 and then 8/1, the
 * second to 0x10/type 2 and then 0/0. */
static void objCmdIdleA(uint8_t *actor)
{
    unsigned f = rd16(actor, 4u);

    if (f < 8u || f > 0x10u) {                 /* 1000:E739: stand up */
        wr16(actor, 4u, 0x0010u);
        wr16(actor, 8u, 0x0002u);
        wr16(actor, 0xAu, 0x0004u);
        return;
    }
    wr16(actor, 0xAu, (uint16_t)(rd16(actor, 0xAu) - 1u));   /* 1000:E74F */
    if ((int16_t)rd16(actor, 0xAu) >= 0) return;
    wr16(actor, 4u, 0x0008u);
    wr16(actor, 8u, 0x0001u);
    wr16(actor, 0xAu, 0x0004u);
}

static void objCmdIdleB(uint8_t *actor)
{
    unsigned f = rd16(actor, 4u);

    if (f >= 8u && f != 0x10u) {               /* 1000:E79D: stand up */
        wr16(actor, 4u, 0x0010u);
        wr16(actor, 8u, 0x0002u);
        wr16(actor, 0xAu, 0x0004u);
        return;
    }
    wr16(actor, 0xAu, (uint16_t)(rd16(actor, 0xAu) - 1u));   /* 1000:E7AD */
    if ((int16_t)rd16(actor, 0xAu) >= 0) return;
    wr16(actor, 4u, 0x0000u);
    wr16(actor, 8u, 0x0000u);
    wr16(actor, 0xAu, 0x0004u);
}

/* 1000:E4BD's `CALL word ptr [BX + 0x6916]` — the ported handlers, by address.
 * Returns 0 when the address has no port yet (the caller reports it). */
/* [0x6916] / [0x6917]: the per-call output state the handlers may set; 1000:E4E7
 * copies it to [0x1979] / [0x1988] when it is non-zero. */
static uint8_t objOutState, objOutStateHi;

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
 * ones the selector uses: `[0x4230]` (objSide), `[0x4234]` (objSelXoff),
 * `[0x4238]` (objSelFrame), `[0x423C]` (objSelType).  `[0x4224]`/`[0x4226]`
 * are the level-select character's x/y (`frontend.c`'s charX/charY; `[0x4224]`
 * is what `set_scroll_x` `1000:AA05` centres on) and `[0x4228]` a saved
 * argument.
 */

/* [0x13A4]: the ACTION press's latch, as the *sequences* see it.  It is raised
 * by the home-state transition (`1000:EE01`) and cleared by the frame scan
 * (`1000:FC81`); `1000:EE07` refuses to turn while it is set.  game_main keeps
 * the other half of the same byte - the per-press gate that runs the ACTION
 * program once (its `actionLatched`) - because there the press is a backend
 * event; the original has one byte for both. */
static uint8_t objActionLatch;
int  object_actionLatched(void) { return objActionLatch != 0; }
void object_setActionLatched(int on) { objActionLatch = on ? 0xFFu : 0u; }

static int objLat;                  /* [0x6F1D]: a sequence owns the input     */
static unsigned objSfx;             /* the last `1000:83B7` driver argument     */
static uint16_t objPtX, objPtY;     /* [0x4224] / [0x4226]                      */
static uint16_t objAux;             /* [0x4228]                                */
static uint8_t objDebounce;         /* [0x197A]: the previous dispatch code     */

/* 1000:83B7 driver_call: INT 60h command 5 with CX = n, gated by [0x5E5B]
 * (sound enabled) and [0x5AFE] (a driver installed).  The port's audio surface
 * has no sfx entry until M6, so the request is recorded and nothing plays;
 * `object_lastSfx` exposes it so a test can pin which handler asked for which
 * effect.  `n = 0xFF` is the "none" sentinel (1000:83C1). */
static void objDriverCall(unsigned n) { objSfx = n; }
void object_driverCall(unsigned n) { objDriverCall(n); }
unsigned object_lastSfx(void) { return objSfx; }

/* 1000:EA33: the acting entity's bit in the two stack masks, 0x64AD and
 * 0x64C1, the same `1 << (7 - entity)` the stack verbs use. */
static int objStackMasked(void)
{
    return ((objStackMask[0] | objStackMask[1]) & objIndexBit(objEntity)) != 0;
}

/* 1000:F9E4 object_advance_frame: the once-per-frame gate.  The first call in a
 * frame stores the frame number at `+0xA` and resets the 0x19-frame countdown
 * (`+0xE`); the repeats tick it down and, when it expires, reset it.  Returns 1
 * while the record must wait, 0 on the 25-frame tick (the original's ZF: every
 * path EXCEPT the expiry ends in `XOR AX,AX`). */
static int objFrameCadence(uint8_t *actor)
{
    unsigned ax = objFrame;

    if (ax == rd16(actor, 0xAu)) {                        /* 1000:F9FD */
        uint16_t e = (uint16_t)(rd16(actor, 0xEu) - 1u); /* 1000:FA01 SUB */

        wr16(actor, 0xAu, (uint16_t)(ax + 1u));
        wr16(actor, 0xEu, e);
        if ((int16_t)e < 0) {
            wr16(actor, 0xEu, 0x0019u);                  /* 1000:FA0A */
            return 0;
        }
        return 1;
    }
    wr16(actor, 0xAu, (uint16_t)(ax + 1u));              /* 1000:F9EC */
    wr16(actor, 0xEu, 0x0019u);
    wr16(actor, 0x10u, 0u);
    return 1;
}

/* 1000:F23A: place the record at ([0x4230], [0x4234]) when the landing probe at
 * ([0x4230], 0x18) comes back 0..7 px below that row.  The probe is
 * `1000:E389`'s; its `BP` argument is the item table - see re_map.md, "the
 * handler's BP", for the open question about what the dispatcher leaves there. */
static void objPlaceAt(uint8_t *actor)
{
    unsigned cell;
    int bx = objLandY((int)(int16_t)objSide, 0x18, &cell);
    int d;

    if (bx < 0) return;                                  /* 1000:F243 */
    d = bx - 0x18 - (int)(int16_t)rd16(actor, 2u);
    if (d < 0) return;                                   /* 1000:F24B */
    if (d - 8 >= 0) return;                              /* 1000:F250 */
    wr16(actor, 4u, objSelXoff);                         /* 1000:F255 */
    wr16(actor, 8u, objSelFrame);
}

/* 1000:FC81: the point scan.  Runs only on even frames (on odd ones it just
 * clears the ACTION latch).  From (xoff + record.x, record.y + 0x10) it walks
 * the map down a row at a time until a cell's page byte is a surface (bit 7
 * clear); the surface's height gives the distance `d` (clamped to 0x5F) to the
 * floor below.  It then fills one of the two 0x1C-byte span entries at 0x6F92 -
 * the one owned by the acting entity, else the first whose word 0 still has bit
 * 15 set - with the point, the distance and a direction word (1 when the found
 * extent is above the requested y), and copies `[0x423C]` into the record's
 * type.  An entry whose +6 is 0 copies its own point into +0xA/+0xC. */
static void objScanPoint(uint8_t *actor, unsigned xoff)
{
    int x = (int)(int16_t)rd16(actor, 0u) + (int)xoff;
    int y = (int)(int16_t)rd16(actor, 2u) + 0x10;
    int top = y, d;
    unsigned b;
    uint8_t *span = NULL;
    int fallback = -1;

    if (objFrame & 1u) {                     /* 1000:FC81 */
        object_setActionLatched(0);
        return;
    }
    for (;;) {                               /* 1000:FCBA */
        unsigned idx = 2u * ((unsigned)y & 0xFFF0u) + ((unsigned)x >> 4);

        b = level_pageByte(objMapBufferByte(idx), (unsigned)x & 0xFu);
        if (!(b & 0x80u)) {                  /* 1000:FCE0: a surface */
            y += (int)(int8_t)b;
            break;
        }
        y += 0x10;
        if ((unsigned)y >= 0x140u) { y = 0x140; break; }   /* 1000:FCC8 */
    }
    if (y > 0x140) y = 0x140;                /* 1000:FCE9 */
    d = y - 1 - top;                         /* 1000:FCF2 */
    if (d <= 0) return;
    if (d > 0x5F) d = 0x5F;
    objSelXoff = 0;                          /* 1000:FD0E */
    objSelFrame = 0;
    objSide = 1;                             /* [0x4230] = the entry count - 1 */
    for (unsigned i = 0; i < 2u; i++) {      /* 1000:FD20 */
        uint8_t *e = (uint8_t *)objSpan[i];

        if (rd16(e, 0u) & 0x8000u) fallback = (int)i;
        if (rd16(e, 0u) == (uint16_t)objEntity) {
            objSelFrame = rd16(e, 6u);
            span = e;
            break;
        }
        objSide = (uint16_t)(objSide - 1u);  /* 1000:FD3B DEC [0x4230] */
    }
    if (!span) {                             /* 1000:FD41 */
        if (fallback < 0) return;
        span = (uint8_t *)objSpan[fallback];
    }
    wr16(span, 0u, (uint16_t)objEntity);     /* 1000:FD4D */
    wr16(span, 2u, (uint16_t)x);
    wr16(actor, 8u, objSelType);             /* 1000:FD52 */
    wr16(span, 4u, (uint16_t)top);
    wr16(span, 6u, objSelFrame);
    wr16(span, 8u, (uint16_t)d);
    if (objSelFrame == 0u) {                 /* 1000:FD6C */
        wr16(span, 6u, 0u);
        wr16(span, 0xAu, rd16(span, 2u));
        wr16(span, 0xCu, (uint16_t)top);
    }
    wr16(span, 0x12u, 0xFFFFu);              /* 1000:FD7B */
    wr16(span, 0x12u, (uint16_t)(d < (int)objSelFrame ? 1 : 0));
}

/* 1000:FAF4: the neighbour pass.  For every appended record whose class slot is
 * 4, that sits within (-0x10, 0] of the probe point on both axes and whose type
 * magnitude is <= 0xA, it reads the item word under the record's feet
 * (x + (delta >= 0 ? 0x10 : 0), y + 8) and, when the fine-x bit is clear, adds
 * the caller's delta to the record's type word.  `delta` is +0xA from 1000:EC99
 * (a step right) and -0xA from 1000:ECA8 (a step left). */
static void objNeighbourPass(uint8_t *actor, unsigned offX, unsigned offY, int delta)
{
    int x = (int)(int16_t)(uint16_t)(rd16(actor, 0u) + offX);
    int y = (int)(int16_t)(uint16_t)(rd16(actor, 2u) + offY);
    unsigned n = objLiveCount > OBJ_LIVE0 ? objLiveCount - OBJ_LIVE0 : 0u;

    objAux = (uint16_t)delta;                            /* 1000:FAF9 */
    objSelFrame = delta >= 0 ? 0x10u : 0u;               /* 1000:FAFD */
    objSelType = 8u;                                     /* 1000:FB03 */
    for (unsigned k = 0; k < n; k++) {                   /* 1000:FB29 */
        uint8_t *r = objArray + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;
        int d, ty;

        objSide = rd16(r, 0u);
        objSelXoff = rd16(r, 2u);
        if ((objSlotClass[1u + k] & 0x3Fu) != 4u) continue;   /* 1000:FB39 */
        if (r[6] & 0x40u) continue;                           /* 1000:FB46 */
        d = (int16_t)(uint16_t)(rd16(r, 0u) - (uint16_t)x);
        if (d >= 0 || d + 0x10 < 0) continue;                 /* 1000:FB4F */
        d = (int16_t)(uint16_t)(rd16(r, 2u) - (uint16_t)y);
        if (d >= 0 || d + 0x10 < 0) continue;                 /* 1000:FB62 */
        ty = (int16_t)rd16(r, 8u);
        if (ty < 0) ty = -ty;
        if (ty > 0xA) continue;                               /* 1000:FB7E */
        {
            int px = (int)(int16_t)rd16(r, 0u) + (delta >= 0 ? 0x10 : 0);
            int py = (int)(int16_t)rd16(r, 2u) + 8;
            unsigned sub, tile = objMapAtAbs(px, py, &sub);
            unsigned word = level_itemWord(tile, (unsigned)(py & 0xF));
            unsigned bit = 1u << (0xFu - ((unsigned)px & 0xFu));

            if (!(word & bit))                                /* 1000:FC17 */
                wr16(r, 8u, (uint16_t)(rd16(r, 8u) + objAux));  /* 1000:FC1D */
        }
    }
}

/* 1000:EE3B: the enter-the-building sequence, run in place of the handler while
 * [0x6F1D] is set.  It releases the latch, folds the record's direction counter
 * (`+0xE`, 0..0x1FF) into a distance and the frame (0x8C..0x93) into one of the
 * eight `0x6E76` four-word vectors - the door's x, its y, the record's new frame
 * and type - offsets the door x by that distance, frees the record's class slot
 * and turns that slot's placed record into a marker (type 0xFFFF, the door's
 * coordinates in its +0xC/+0xE), then arms the ACTION dispatch ([0x5B56] = 1,
 * [0x6916] = 5, [0x6917] = 1) and plays effect 3. */
static void objCmdEnter(uint8_t *actor)
{
    static const int16_t doors[8][4] = {     /* 0x6E76: dx, y, frame, type */
        {  3,  -5, 0, 0 }, {  2,  -9, 0, 0 }, {  1,  -7, 0, 0 }, { 0,   0, 0, 0 },
        {  0,   0, 0, 0 }, { -3,  -5, 8, 1 }, { -2,  -7, 8, 1 }, { -1,  -9, 8, 1 },
    };
    unsigned ax, idx, dx;
    uint8_t *rec;

    if (!objLat) {                           /* 1000:EE42 */
        wr16(actor, 0xEu, 0u);
        return;
    }
    objLat = 0;                              /* 1000:EE48 */
    ax = rd16(actor, 0xEu);
    if (ax >= 0x100u) ax = 0x1FFu - ax;      /* 1000:EE55 */
    ax >>= 6;
    dx = (unsigned)((rd16(actor, 4u) - 0x8Cu) & 0xFFFFu);
    if (dx >= 8u) return;                    /* past 0x6E76's eight entries */
    objPtX = (uint16_t)(doors[dx][0] >= 0 ? doors[dx][0] + (int)ax
                                          : doors[dx][0] - (int)ax);
    objPtY = (uint16_t)doors[dx][1];         /* 1000:EE8B */
    wr16(actor, 4u, (uint16_t)doors[dx][2]); /* 1000:EE94 */
    wr16(actor, 8u, (uint16_t)doors[dx][3]);
    wr16(actor, 0xAu, 0u);

    idx = actor[7];                          /* 1000:EEB8 */
    actor[7] = 0u;
    /* The original multiplies the decremented slot byte by 0x12 with no range
     * test: for a class byte of 0 the factor is 0xFFFF, `MUL DL` uses only
     * 0xFF, and the writes land at 0x3F17 + 0x11EE = 0x5105 (+8), i.e. on
     * 0x510D..0x5114 - DS data the port has no image of.  The bound below is
 * port_deviations.md D-24:
     * the one unavoidable deviation in this routine: it keeps the port from
     * writing through a pointer it cannot resolve, and the branch only runs for
     * a record whose class byte is 0 or above the table. */
    if (idx && idx - 1u < sizeof objSlotClass &&
        8u + idx - 1u < OBJ_SLOTS) {
        idx--;
        objSlotClass[idx] &= 0x3Fu;          /* 0x3E5E + idx */
        rec = objArray + (8u + idx) * OBJ_STRIDE;                /* 0x3F17 + 0x12*idx */
        wr16(rec, 8u, 0xFFFFu);              /* 1000:EED5 */
        wr16(rec, 0xAu, 0u);
        wr16(rec, 0xCu, objPtX);             /* 1000:EEE5 */
        wr16(rec, 0xEu, objPtY);
    }
    objSetProgramIndex(1u);                  /* 1000:EEF2: [0x5B56] = 1 */
    objOutState = 5;                         /* 1000:EEF7 */
    objOutStateHi = 1;                       /* 1000:EEFC */
    objDriverCall(3);
}

/* [0x6FE6]: the "goal reached" marker 0xEAE6 and 0xF1B2 write; nothing in the
 * port reads it yet (the original's level-end path does). */
static uint8_t objFlag6fE6;
static void objFlag6fE6Set(void) { objFlag6fE6 = 0xFFu; }
/* the flag's reader is unported, so expose the store for the test */
int object_flag6fE6(void) { return objFlag6fE6 != 0u; }


/* 1000:FDBF: the down-walking twin of 0xFD91 (y++ and the direction bytes are
 * the mirror image). */
static void objFrameStepDown(uint8_t *actor, unsigned lo, unsigned hi)
{
    unsigned f;

    wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) + 1u));    /* 1000:FDBF */
    wr16(actor, 0xAu, (uint16_t)(rd16(actor, 0xAu) - 1u));
    if ((int16_t)rd16(actor, 0xAu) >= 0) return;
    wr16(actor, 0xAu, 4u);
    f = rd16(actor, 4u);
    if (actor[0xCu] != 0u) {
        if (++f == hi) actor[0xCu] = 0u;
    } else {
        if (--f == lo) actor[0xCu] = 0xFFu;
    }
    wr16(actor, 4u, f);
}

/* DS:0x6E5C - the frame list 0xECB7 walks: twelve ramp words and then 3-word
 * vectors from 0x6E74 (re_formats.md §"the value table"). */
static const uint16_t objAnimList[0x1A] = {
    0x0010, 0x007F, 0x0080, 0x0081, 0x0080, 0x0081, 0x0080, 0x0081,
    0x0080, 0x0081, 0x007F, 0x0010, 0xFFFF, 0x0003, 0xFFFB, 0x0000,
    0x0000, 0x0002, 0xFFF9, 0x0000, 0x0000, 0x0001, 0xFFF7, 0x0000,
    0x0000, 0x0000,
};

static unsigned objAnimListWord(unsigned off)
{
    return off + 1u < sizeof objAnimList ? objAnimList[off / 2u] : 0u;
}




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
static void objLevelEndProbe(uint8_t *actor, int bx)
{
    uint8_t *t;
    unsigned idx;

    objLat = 0;                                      /* 1000:F5BC */
    if (bx >= 0) {
        t = object_slot((unsigned)bx);
        idx = (unsigned)bx;
    } else {
        t = actor;
        idx = (unsigned)((actor - objArray) / OBJ_STRIDE);
    }
    if (!t) return;
    if (statusbar_tribeActive()) goto goal;          /* 1000:F5E3 */
    wr16(t, 0u, 0xFFFFu);                            /* 1000:F5EF: taken */
    wr16(t, 2u, 0xFFFFu);
    t[6] = 0xFFu;
    wr16(t, 8u, 0x001Bu);
    if (t[7]) {                                      /* 1000:F603 */
        unsigned cls = t[7];

        if (cls - 1u < sizeof objSlotClass) objSlotClass[cls - 1u] = 0u;
        if (8u + cls - 1u < OBJ_SLOTS)
            objArray[(8u + cls - 1u) * OBJ_STRIDE + 6] = 0xFFu;
        t[7] = 0u;
    }
    if (idx != objEntity) return;                    /* 1000:F627 */
    for (unsigned i = 0; i < 8u; i++) {              /* 1000:F630 */
        uint8_t *r = object_slot(i);
        unsigned ty;

        if (!r || (r[6] & 0x80u)) continue;
        ty = rd16(r, 8u);
        if (ty == 0x16u) continue;
        if (ty >= 0x1Bu && ty <= 0x1Eu) continue;
        if (ty == 0x21u) continue;
        objResolveSprite(i);                         /* 1000:F664 -> 0x4147 */
        return;
    }
    level_setEnd(0);                                 /* 1000:F66D */
    return;

goal:                                                /* 1000:F682 */
    statusbar_armOdometer();                         /* [0x5B22] = 0xFF */
    wr16(t, 2u, objSelXoff);                         /* [0x4222] */
    wr16(t, 0u, objSide);                            /* [0x4220] */
    if ((int16_t)objSide < 0) {                      /* 1000:F692 */
        wr16(t, 0u, (uint16_t)((objSettled[idx][0] + 8u) & 0xFFF0u));
        wr16(t, 2u, objSettled[idx][1]);
    }
    wr16(t, 0u, (uint16_t)(rd16(t, 0u) - 8u));       /* 1000:F6B1 */
    wr16(t, 4u, 0x002Du);
    t[6] = 0u;
    wr16(t, 8u, 0x0021u);
    wr16(t, 0xAu, 5u);
    if (idx == objEntity && (int)idx != objPlayer && !t[7]) {
        objSetProgramIndex(1u);                      /* [0x5B56] = 1 */
        objOutState = 5;                             /* [0x6916] = 0x105 */
        objOutStateHi = 1;
    }
    {
        unsigned cell;
        int land = objLandY((int)(int16_t)rd16(t, 0u) + 0x10,
                            (int)(int16_t)rd16(t, 2u) + 0x18, &cell);

        if (land >= 0) wr16(t, 2u, (uint16_t)(land - 0x18));   /* 1000:F6F2 */
    }
}

/* DS:0x6868 - the 20-byte frame table 0xDBCD walks (the frame is a table byte,
 * `+0xC` its cursor), and DS:0x687C - the 4-byte {dx, dy, flag, hold} table
 * 0xDE7E walks. */
static const uint8_t objTab6868[0x14] = {
    0xCD, 0xCE, 0xCD, 0xCE, 0xCD, 0xCE, 0xCF, 0xD0, 0xCF, 0xD0,
    0xCF, 0xD0, 0xD1, 0xD2, 0xD1, 0xD2, 0xD1, 0xD2, 0xD3, 0x00,
};
static const uint8_t objTab687C[0x20] = {
    0x01, 0x06, 0x01, 0x06, 0x01, 0x06, 0x01, 0x06,
    0x01, 0x06, 0x01, 0x06, 0xFF, 0x32, 0x01, 0x06,
    0xFF, 0x32, 0x01, 0x06, 0x01, 0x06, 0x01, 0x06,
    0x01, 0x06, 0x01, 0x06, 0xFF, 0x07, 0x01, 0x07,
};

static unsigned objTabByte(const uint8_t *t, unsigned n, unsigned off)
{
    return off < n ? t[off] : 0u;
}

/* 1000:DBCD (type 28): a 9-frame stepper over 0x6868 (the frame is a table
 * byte, +0xC the cursor); at frame 0xD3 the record climbs one pixel per tick
 * while its y's low nibble is non-zero, and at a row boundary the level-end
 * probe runs with the scratch pair at 0xFFFF. */
static void objTailType28(uint8_t *actor)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:DBCD */
    if ((int16_t)a >= 0) return;
    wr16(actor, 0xAu, 9u);
    if (rd16(actor, 4u) != 0x00D3u) {                /* 1000:DBD7 */
        unsigned off = rd16(actor, 0xCu);

        actor[4] = (uint8_t)objTabByte(objTab6868, sizeof objTab6868, off);
        wr16(actor, 0xCu, (uint16_t)(off + 1u));
        return;
    }
    if (rd16(actor, 2u) & 0xFu) {                    /* 1000:DBEC */
        wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) + 1u));
        return;
    }
    objSide = 0xFFFFu;                               /* [0x4220] */
    objSelXoff = 0xFFFFu;                            /* [0x4222] */
    objLevelEndProbe(actor, -1);                 /* 1000:DC11 */
}

/* 1000:DE7E (type 30): the 0x687C table walk. */
static void objTailType30(uint8_t *actor)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:DE7E */
    if ((int16_t)a >= 0) return;
    {
        unsigned off = rd16(actor, 0xCu);
        int dx = (int)(int8_t)(uint8_t)objTabByte(objTab687C, sizeof objTab687C, off);
        int dy = (int)(int8_t)(uint8_t)objTabByte(objTab687C, sizeof objTab687C, off + 1u);
        unsigned nxt = off + 2u;

        if (objTabByte(objTab687C, sizeof objTab687C, off + 2u) == 0u) {   /* 1000:DE9F */
            unsigned e;

            nxt = (uint16_t)(nxt - 4u);              /* 1000:DEA5 */
            e = (uint16_t)(rd16(actor, 0xEu) - 1u);
            wr16(actor, 0xEu, e);                    /* 1000:DEA8 */
            if ((int16_t)e < 0 && statusbar_tribeActive()) {   /* 1000:DEB4 */
                objSide = rd16(actor, 0u);           /* [0x4220] */
                objSelXoff = rd16(actor, 2u);        /* [0x4222] */
                objLevelEndProbe(actor, -1);     /* 1000:DEC4 */
                return;
            }
        }
        wr16(actor, 0xCu, nxt);                      /* 1000:DEC7 */
        wr16(actor, 0xAu, (uint16_t)(int16_t)dy);    /* [SI+0xA] = CX */
        wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + (uint16_t)(int16_t)dx));
    }
}

/* 1000:DED1 (type 34): a two-state tail on the record `+0xA` points at.  While
 * the +6 bit 7 is set: a target `+4` of 2 or 3 holds, anything else runs the
 * level-end probe at ([SI+0x10], [target+8] + 0x28).  With the bit clear: a
 * target `+4` of 2 or 3 sets the bit when `[target+0xC] == +0xE`, and anything
 * else takes the home form (frame 0x10 / type 2, `+0xA`/`+0xC` cleared). */
static void objTailType34(uint8_t *actor)
{
    /* 1000:DED1: +0xA is the ENTITY record pointer, not an object link.  The
     * entity interaction tags the index (0x8000|n); an untagged link is not
     * a carry and leaves the record alone. */
    unsigned link = rd16(actor, 0xAu);
    const Entity *e = (link & 0x8000u) ? entity_at(link & 0x7FFFu) : NULL;
    unsigned v;

    if (!e) return;
    v = e->handler;
    if (v == 2u || v == 3u) {                        /* 1000:DEDA / 0xDF0A */
        if (!(actor[6] & 0x80u)) {
            if (e->waypoint == rd16(actor, 0xEu)) actor[6] |= 0x80u;
        }
        return;
    }
    if (actor[6] & 0x80u) {                          /* 1000:DEE8 */
        objSide = rd16(actor, 0x10u);                /* [0x4220] */
        objSelXoff = (uint16_t)(e->y + 0x28u);       /* [0x4222] */
        objLevelEndProbe(actor, -1);
        return;
    }
    wr16(actor, 4u, 0x0010u);                        /* 1000:DF17 */
    wr16(actor, 8u, 0x0002u);
    wr16(actor, 0xAu, 0u);
    wr16(actor, 0xCu, 0u);
}

/* 1000:EF66 (type 22): a 5-frame stepper to frame 0x2E, then the level-end
 * probe with the scratch pair at 0xFFFF. */
static void objTailType22(uint8_t *actor)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:EF66 */
    if ((int16_t)a >= 0) return;
    wr16(actor, 0xAu, 5u);
    wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + 1u));
    if (rd16(actor, 4u) != 0x2Eu) return;            /* 1000:EF73 */
    objSide = 0xFFFFu;                               /* [0x4220] */
    objSelXoff = 0xFFFFu;                            /* [0x4222] */
    objLevelEndProbe(actor, -1);                 /* 1000:EF93 */
}

/* --- the rows' tails (the +0xC word of each DS:0x698A row) --------------
 *
 * `1000:E4D1` reads the tail word from the row and `1000:E4E0` calls it after
 * the handler and the `+0xA` hook.  Twenty-seven distinct tails exist; the ones
 * below are the rows' own, each cited to its entry.
 */

/* 1000:D9D0 (shared body of 0xD9C8 / 0xD9CD, types 31 and 32): a 7-frame hold
 * that steps the frame up and wraps four back at the bound (0x112 / 0x116). */
static void objTailFrameBound(uint8_t *actor, unsigned bound)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:D9D0 */
    if ((int16_t)a >= 0) return;
    wr16(actor, 0xAu, 7u);                           /* 1000:D9D6 */
    wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + 1u));
    if (rd16(actor, 4u) == bound)
        wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) - 4u));   /* 1000:D9E3 */
}

/* 0xD9C8 / 0xD9CD: `MOV AX,bound; JMP 0xD9D0`. */

/* 1000:DB30: the frame -> type map the landing tails share. */
static void objTailFrameToType(uint8_t *actor)
{
    unsigned t = 2;

    if (rd16(actor, 4u) < 0x10u) {                   /* 1000:DB33 */
        t = 1;
        if (rd16(actor, 4u) < 8u) t = 0;
    }
    wr16(actor, 8u, t);                              /* 1000:DB44 */
}

/* 1000:DB15: the hard landing. */
static void objTailCrash(uint8_t *actor)
{
    wr16(actor, 0u, (uint16_t)(rd16(actor, 0u) - 7u));   /* 1000:DB15 */
    wr16(actor, 4u, 0x0028u);
    wr16(actor, 8u, 0x0016u);
    wr16(actor, 0xAu, 5u);
    objDriverCall(4);                                /* 1000:DB28 */
}


/* 1000:D9E8 (type 3): the gravity tail.  The gate is the same 0x6EBE/row one
 * the settle uses; then a probe at (x + 8, y + 0x18) with the row above it, the
 * 0xF0..0xF2 platform snap (frame 0xCD / type 0x1C / effect 6), and otherwise
 * the page byte of the feet / one row above / one row below picking a landing:
 * a descent of 0x30 or more crashes (frame 0x28 / type 0x16 / effect 4), a
 * slower one arms [0x2D8E] = speed >> 3, takes the frame->type map and plays
 * effect 5.  `[0x4258]` is the map buffer's end (map + 0x280 = 640 here). */
static void objTailType3(uint8_t *actor)
{
    const uint8_t *map = level_map();
    unsigned ax, bx, cx, dx, di, b;
    int sc;

    if (!objSettleGate(actor)) {                     /* 1000:D9EB */
        objTailFrameToType(actor);
        return;
    }
    ax = (unsigned)(rd16(actor, 0u) + 8u);
    bx = (unsigned)(rd16(actor, 2u) + 0x18u);
    dx = rd16(actor, 0xAu);
    if (bx >= 0x158u) { objTailCrash(actor); return; }   /* 1000:DA02 */
    cx = bx;
    bx &= 0xFFF0u;
    di = 2u * bx + (ax >> 4);                        /* 1000:DA0C..DA23 */
    ax &= 0xFu;                                      /* 1000:DA1C: AX = the FINE x */
    if (map) {
        b = objMapBufferByte(di - 0x20u);            /* 1000:DA30 */

        if (b >= 0xF0u && b - 0xF0u < 3u) {          /* 1000:DA37/DA39 */
            /* AX is the fine x by now, so the snap writes 4 or 20, not the
             * record's x - the original's own arithmetic (1000:F772..F784).
             * Transcribed as it stands. */
            unsigned gx = (ax + 8u) & ~0xFu;

            if (b != 0xF1u) {
                gx -= 4u;                            /* 1000:F778 */
                if (b != 0xF2u) gx += 8u;
                wr16(actor, 0u, (uint16_t)gx);       /* 1000:F784 */
            }
            wr16(actor, 2u, (uint16_t)(bx - 0x1Cu));
            wr16(actor, 4u, 0x00CDu);
            wr16(actor, 8u, 0x001Cu);
            wr16(actor, 0xCu, 0u);
            wr16(actor, 0xAu, 9u);
            objDriverCall(6);
            return;
        }
    }
    if (di >= 0x280u || !map) goto coast;            /* 1000:DA63 */
    b = level_pageByte(objMapBufferByte(di), ax);     /* 1000:DA71 */
    if (b & 0x80u) {                                 /* 1000:DA85 */
        di += 0x20u;
        if (di >= 0x280u) goto coast;                /* 1000:DA8C */
        b = level_pageByte(objMapBufferByte(di), ax);
        if (b & 0x80u) goto coast;                   /* 1000:DAA6 */
        bx += 0x10u;                                 /* 1000:DAA8 */
        sc = (int)(int8_t)(uint8_t)b;
        bx = (unsigned)((int)bx + sc);
        cx = (unsigned)((int)cx - (int)bx);
        if ((int)cx < 0) {
            cx = (unsigned)-(int)cx;
            if (cx > dx) goto coast;                 /* 1000:DB01 */
        }
        goto land;                                   /* 1000:DAB2 */
    }
    if (b != 0u) {                                   /* 1000:DAD6 */
        sc = (int)(int8_t)(uint8_t)b;
        bx = (unsigned)((int)bx + sc);
        cx = (unsigned)((int)cx - (int)bx);
        if ((int)cx < 0) {
            cx = (unsigned)-(int)cx;
            if (cx > dx) goto coast;
        }
        goto land;
    }
    /* b == 0: look two rows above (1000:DAD8) */
    if (di >= 0x40u) {
        b = level_pageByte(objMapBufferByte(di - 0x40u), ax);
        objSide = (uint16_t)(int16_t)(int8_t)(uint8_t)b;   /* [0x4230] */
        if (!(b & 0x80u)) {
            bx -= 0x10u;
            sc = (int)(int16_t)objSide;
            bx = (unsigned)((int)bx + sc);
            cx = (unsigned)((int)cx - (int)bx);
            if ((int)cx < 0) {
                cx = (unsigned)-(int)cx;
                if (cx > dx) goto coast;
            }
            goto land;
        }
    }
    sc = 0;                                          /* 1000:DA87: AX = 0 */
    bx = (unsigned)((int)bx + sc);
    cx = (unsigned)((int)cx - (int)bx);
    if ((int)cx < 0) {
        cx = (unsigned)-(int)cx;
        if (cx > dx) goto coast;
    }
land:                                                /* 1000:DAB2 */
    wr16(actor, 2u, (uint16_t)(bx - 0x18u));
    {
        unsigned sp = rd16(actor, 0xEu);

        if (sp >= 0x30u) { objTailCrash(actor); return; }    /* 1000:DABF */
        level_set_offsetWord((uint16_t)(sp >> 3));           /* 1000:DAC7 */
    }
    objTailFrameToType(actor);                       /* 1000:DACA */
    objDriverCall(5);
    return;
coast:                                               /* 1000:DB05 */
    wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) + dx));
    wr16(actor, 0xEu, (uint16_t)(rd16(actor, 0xEu) + dx));
    wr16(actor, 0xAu, (uint16_t)(dx == rd16(actor, 0xCu) ? dx : dx + 1u));
}

/* 1000:DB7A: does the actor's span entry exist, still marked, and un-used?
 * Returns 1 when the original's ZF is set (the "yes" of `JNZ` at the callers). */
static int objSpanOwner(uint8_t *actor)
{
    unsigned idx = (unsigned)((actor - objArray) / OBJ_STRIDE);

    for (unsigned i = 0; i < 2u; i++) {              /* AX = 1: two entries */
        uint8_t *e = (uint8_t *)objSpan[i];

        if (rd16(e, 0u) != (uint16_t)(idx & 0x7FFFu)) continue;   /* 1000:DB8D */
        if (!(rd16(e, 0x12u) & 0x8000u)) return 0;   /* 1000:DB9D */
        if (rd16(e, 8u) != 0u) return 0;             /* 1000:DBA4 */
        return 1;
    }
    return 0;
}

/* 1000:DBAE: walk the actor's frame toward AX on a 3-frame hold. */
static void objTailWalkToFrame(uint8_t *actor, unsigned frame)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:DBB6 */
    if ((int16_t)a >= 0) return;
    wr16(actor, 0xAu, 3u);                           /* 1000:DBBB */
    if (rd16(actor, 4u) == frame)
        wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + 1u));   /* 1000:DBC9 */
    else
        wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) - 1u));   /* 1000:DBC5 */
}

/* 1000:DB48 / 1000:DB61 (types 9 and 10): the span's owner test; on "yes" the
 * acting record is reset (frame/type 0/0 or 8/1), otherwise it walks toward
 * 0xAB / 0xAD. */
static void objTailSpanOwner(uint8_t *actor, unsigned frame, unsigned f8, unsigned bad)
{
    if (objSpanOwner(actor)) {                       /* 1000:DB4B */
        wr16((uint8_t *)objSpan[0], 0u, 0xFFFFu);    /* 1000:DB4D: free it */
        wr16(actor, 4u, (uint16_t)frame);
        wr16(actor, 8u, (uint16_t)f8);
        return;
    }
    objTailWalkToFrame(actor, bad);                  /* 1000:DB5C -> 0xDBAE */
}

/* 1000:DD93 (type 16): a 10-frame stepper that walks the frame through
 * 0x55..0x57, flipping the direction byte at +0xC and playing effect 8 at 0x55;
 * a `+0xE` countdown then drops it back to frame 0x10 / type 2. */
static void objTailType16(uint8_t *actor)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);
    if ((int16_t)a < 0) {                            /* 1000:DD96 */
        unsigned f = rd16(actor, 4u);

        wr16(actor, 0xAu, 0x0Au);                    /* 1000:DD98 */
        if (actor[0xCu] != 0u) {                     /* 1000:DD A0 */
            f--;
            if (f == 0x55u) {                        /* 1000:DD A7 */
                actor[0xCu] = 0u;
                objDriverCall(8);
                f = 0x55u;
            }
        } else {
            f++;
            if (f == 0x57u) actor[0xCu] = 0xFFu;     /* 1000:DDC3 */
        }
        wr16(actor, 4u, f);                          /* 1000:DD C7 */
    }
    a = (uint16_t)(rd16(actor, 0xEu) - 1u);          /* 1000:DDCA */
    wr16(actor, 0xEu, a);
    if ((int16_t)a < 0) {
        wr16(actor, 4u, 0x0010u);
        wr16(actor, 8u, 0x0002u);
    }
}

/* 1000:EB5C / 1000:EB6A (types 13 and 14): one walk step, then a `+0xE`
 * countdown that sets the type to 1 (left) or 0 (right). */
static void objTailWalkCountdown(uint8_t *actor, int left)
{
    if (left) objWalk(actor, 1); else objWalk(actor, 0);   /* 1000:EB5C */

    {
        unsigned a = (uint16_t)(rd16(actor, 0xEu) - 1u);

        wr16(actor, 0xEu, a);                        /* 1000:EB5F */
        if ((int16_t)a < 0)
            wr16(actor, 8u, left ? 1u : 0u);         /* 1000:EB64 / 0xEB72 */
    }
}

/* 1000:E802 / 1000:E831 (types 4 and 5): on a type match, probe the cell under
 * the feet; a field-K special tile is the climb stepper (`1000:E828`/`E857`:
 * lo 0x23, hi 0x25 into 0xFD91/0xFDBF), anything else drops the record to
 * frame 0x10 / type 2.  (The 0x52/0x54 pair belongs to 1000:EAAC's type-11
 * carry stepper, not here.) */
static void objTailSpecialTile(uint8_t *actor, unsigned type, int yoff, int down)
{
    unsigned cell, sub;

    if (rd16(actor, 8u) != type) return;             /* 1000:E802 */
    cell = objMapAtAbs((int)(int16_t)rd16(actor, 0u) + 8, (int)(int16_t)rd16(actor, 2u) + yoff, &sub);
    if (cell == level_specialTile(0u) || cell == level_specialTile(1u)) {
        if (down) objFrameStepDown(actor, 0x23u, 0x25u);      /* 1000:E857 */
        else      objFrameStep(actor, 0x23u, 0x25u);          /* 1000:E828 */
        return;
    }
    wr16(actor, 4u, 0x0010u);                        /* 1000:E81D */
    wr16(actor, 8u, 0x0002u);
}

/* 1000:ECB7 (type 17): step the 0x6E5C frame list by one, then the shared
 * "restore home" block - which the listing runs on BOTH paths (the `ADD +0xC`
 * at 0xECD8 falls straight into 0xECDD), so the frame/type are always taken
 * from `+0xE`/`+0x10`, `+0xA`/`+0xC` are cleared and `[0x1F1C]` gets 0x82 to its
 * `+4` and 0 to its `+8`. */
static void objTailType17(uint8_t *actor)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:ECB7 */
    if ((int16_t)a < 0) {
        unsigned off = rd16(actor, 0xCu), di;

        wr16(actor, 0xAu, 9u);
        di = 0u;                                     /* 0x6E5C + off */
        (void)di;
        if (off < 0x1Au) wr16(actor, 4u, (uint16_t)objAnimListWord(off));
        if (!(objAnimListWord(off + 2u) & 0x8000u))
            wr16(actor, 0xCu, (uint16_t)(off + 2u));
    }
    wr16(actor, 4u, rd16(actor, 0xEu));              /* 1000:ECDD */
    wr16(actor, 8u, rd16(actor, 0x10u));
    wr16(actor, 0xAu, 0u);
    wr16(actor, 0xCu, 0u);
    /* 1000:ECF1 writes [0x1F1C]'s +4 = 0x82 and +8 = 0; that pointer has no
     * ported writer (re_map.md, "the [0x1F1C] record"), so it is not stored. */
}


/* 1000:DDDA (type 29): the "join the tribe" tail.  It compares the *acting*
 * record with this one (0x79F4 returns it in SI): when the actor is within the
 * 0x20 window, the actor is collected - type 2, its class record marked, the
 * two counters bumped ([0x3E20], [0x11FC]) - with effect 0xC; otherwise a slow
 * two-frame animation toward frame 0xFE and then 0x102 on its class record. */
static void objTailType29(uint8_t *actor)
{
    uint8_t *e = object_slot(object_actionEntity());
    int dx, dy;
    unsigned a;

    if (e && rd16(e, 8u) <= 2u) {                    /* 1000:DDE4 */
        dx = (int)(int16_t)rd16(actor, 0u) - (int)(int16_t)rd16(e, 0u);
        dy = (int)(int16_t)rd16(actor, 2u) - (int)(int16_t)rd16(e, 2u);
        if (dx < 0 && dx + 0x20 >= 0 && dy < 0 && dy + 0x20 >= 0) {
            uint8_t *cls = objLinkRecord(rd16(actor, 0xCu));

            wr16(actor, 8u, 2u);                     /* 1000:DDFD */
            wr16(actor, 0xAu, 0u);
            if (cls) cls[6] = 0x80u;                 /* 1000:DE0A */
            wr16(actor, 4u, 0x0010u);
            wr16(actor, 0u, (uint16_t)(rd16(actor, 0u) + 8u));
            wr16(actor, 2u, (uint16_t)(rd16(actor, 2u) + 8u));
            objPlacedHumans++;                       /* 1000:DE1B: [0x3E20] */
            level_addMember();                       /* 1000:DE1F: [0x11FC] */
            objDriverCall(0xC);
            return;
        }
    }
    a = (uint16_t)(rd16(actor, 0xAu) - 1u);          /* 1000:DE2C */
    wr16(actor, 0xAu, a);
    if ((int16_t)a >= 0) return;
    if (rd16(actor, 4u) == 0x00FEu) {                /* 1000:DE31 */
        uint8_t *cls;

        wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + 1u));
        wr16(actor, 0xAu, 0x32u);
        cls = objLinkRecord(rd16(actor, 0xCu));
        if (cls) {
            wr16(cls, 0u, rd16(actor, 0u));
            wr16(cls, 2u, rd16(actor, 2u));
            wr16(cls, 0u, (uint16_t)(rd16(cls, 0u) + 0x0Fu));   /* 1000:DE60 */
            wr16(cls, 2u, (uint16_t)(rd16(cls, 2u) - 0x0Cu));
            wr16(cls, 4u, 0x0102u);
            cls[6] = 0u;
        }
        objDriverCall(8);
        return;
    }
    wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) - 1u));      /* 1000:DE38 */
    wr16(actor, 0xAu, 0xFAu);
    {
        uint8_t *cls = objLinkRecord(rd16(actor, 0xCu));

        if (cls) cls[6] = 0x80u;
    }
}

/* 1000:EAAC (type 11): the record's +0xE is an index into the 0x3E24 record
 * pointer table; compare the target's y with ours, then either the 0x52/0x54
 * stepper or the "home" form (frame from the target's +8 + 4, +0xE = 0xA). */
static void objTailType11(uint8_t *actor)
{
    uint8_t *t = object_slot(rd16(actor, 0xEu));     /* [[SI+0xE]] */

    if (!t) return;
    if (rd16(actor, 2u) > rd16(t, 2u)) {             /* 1000:EAB8 */
        objFrameStep(actor, 0x52u, 0x54u);
        return;
    }
    {
        unsigned f = rd16(t, 8u);

        wr16(actor, 8u, (uint16_t)(f + 4u));         /* 1000:EAC9 */
        wr16(actor, 4u, f == 9u ? 8u : 0u);
        wr16(actor, 0xEu, 0x000Au);                  /* 1000:EAE0 */
    }
}

/* 1000:EAE6 (type 12): the same table entry; if the record's y is at or below
 * `[t+4] + [t+6]` it is home (frame 0x10, type 2, [0x6FE6] = 0xFF), otherwise
 * the map probe under its feet decides between the down-stepper and home. */
static void objTailType12(uint8_t *actor)
{
    uint8_t *e = object_slot(rd16(actor, 0xEu));
    unsigned sum;

    if (!e) return;
    sum = (unsigned)(rd16(e, 4u) + rd16(e, 6u));
    if (rd16(actor, 2u) > sum) {                     /* 1000:EAF6 */
        unsigned cell, sub;
        int y = (int)(int16_t)rd16(actor, 2u) + 0x18;
        unsigned b;

        cell = objMapAtAbs((int)(int16_t)rd16(actor, 0u) + 8, y, &sub);
        b = level_pageByte(cell, sub);
        if (!(b & 0x80u)) {                          /* 1000:EB4B */
            unsigned line = (unsigned)(y & ~0xFu) + b;

            if (line > (unsigned)(int)(int16_t)rd16(actor, 2u)) {
                objFrameStepDown(actor, 0x52u, 0x54u);
                return;
            }
        }
    }
    wr16(actor, 4u, 0x0010u);                        /* 1000:EAFB */
    wr16(actor, 8u, 0x0002u);
    objFlag6fE6Set();                                   /* 1000:EB05 */
}

/* 1000:F0C9 (type 33): a 5-frame stepper down to frame 0x27, where the record
 * jumps 8 px right and returns to frame 0x10 / type 2; if it is the acting
 * record it also seeds slot 8 (0x3F05) with type 0x1A and [0x6DEA] = 0x3E8. */
static void objTailType33(uint8_t *actor)
{
    unsigned a = (uint16_t)(rd16(actor, 0xAu) - 1u);

    wr16(actor, 0xAu, a);                            /* 1000:F0C9 */
    if ((int16_t)a >= 0) return;
    wr16(actor, 0xAu, 5u);
    wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) - 1u));
    if (rd16(actor, 4u) != 0x27u) return;            /* 1000:F0D6 */
    wr16(actor, 0u, (uint16_t)(rd16(actor, 0u) + 8u));
    wr16(actor, 4u, 0x0010u);
    wr16(actor, 8u, 0x0002u);
    wr16(actor, 0xAu, 0u);
    if ((unsigned)((actor - objArray) / OBJ_STRIDE) != objEntity) return;   /* 1000:F0F7 */
    {
        uint8_t *slot8 = object_slot(8u);            /* 0x3F05 */

        if (!slot8) return;
        wr16(slot8, 0u, rd16(actor, 0u));
        wr16(slot8, 2u, (uint16_t)(rd16(actor, 2u) - 0x1Au));
        wr16(slot8, 6u, 0u);
        wr16(slot8, 8u, 0x001Au);
        wr16(slot8, 0xAu, 0x0605u);
        objCmdTimer = 0x03E8u;                       /* 1000:F11C */
    }
}

/* --- the path handlers (indices 24 / 25 / 27: the type-15 rows) ----------
 *
 * These walk a record that `1000:9CF0` (the give/hold direction) creates: a
 * record whose **type is 0x0F** and whose `+0xE` links to another live record
 * (`found - 0x3E75 + 4`, the port's existing encoding).  So the "node" is a
 * placed record: `[+0]` its x, `[+2]` its y, `[+7]` its class byte, `[+8]` the
 * progress word, `[+0x10]` the map-derived value EBD0 writes.
 */
static uint8_t *objLinkRecord(unsigned off)
{
    unsigned idx;

    if (off < 4u) return NULL;
    idx = (off - 4u) / OBJ_STRIDE;
    return idx < OBJ_SLOTS ? objArray + idx * OBJ_STRIDE : NULL;
}

/* 1000:FC32: nudge the node's progress.  The *acting* record's own `+0xA`
 * accumulates the step (clamped at +/-8; a value about to pass +/-9 enters at
 * 0xFFF8 / 8) and the signed accumulator is added to the node's `+8`, unless the
 * node's `+0xA` word is non-zero (a "busy" gate) or the stored progress already
 * has the same sign and is 0xA away. */
static void objNudge(uint8_t *actor, int step)
{
    uint8_t *node = objLinkRecord(rd16(actor, 0xEu));
    unsigned bx;
    int cx;

    if (!node) return;
    if (rd16(node, 0xAu) != 0u) return;              /* 1000:FC3C */
    bx = (uint16_t)(rd16(actor, 0xAu) + (uint16_t)step);
    if ((int16_t)bx < 0) {
        if (bx == (uint16_t)-9) bx = 0xFFF8u;        /* 1000:FC45 */
    } else if (bx == 9u) {
        bx = 8u;                                     /* 1000:FC4F */
    }
    wr16(actor, 0xAu, bx);                           /* 1000:FC57 */
    cx = (int16_t)rd16(node, 8u);
    if ((int16_t)bx < 0) {
        if (cx >= 0) { wr16(node, 8u, (uint16_t)(cx + (int16_t)bx)); return; }
        if (-cx >= 0xA) return;                      /* 1000:FC69 */
    } else {
        if (cx < 0) { wr16(node, 8u, (uint16_t)(cx + (int16_t)bx)); return; }
        if (cx >= 0xA) return;                       /* 1000:FC78 */
    }
    wr16(node, 8u, (uint16_t)(cx + (int16_t)bx));    /* 1000:FC7E */
}

/* 1000:EBA0 / 1000:EB78: step the node's progress by +1 / -1 until it reaches
 * +0xF / -0xF, where the acting record takes frame 0x4A and the node's class
 * byte is parked at 0xC (`& 7` = 4). */
static void objCmdPathStep(uint8_t *actor, int forward)
{
    uint8_t *node = objLinkRecord(rd16(actor, 0xEu));
    int v;

    if (!node) return;
    if ((node[7] & 7u) == 4u) return;                /* 1000:EBA6 / 0xEB7E */
    v = (int16_t)rd16(node, 8u);
    if (forward) {
        if (v >= 0 || -v < 0xF) { objNudge(actor, 1); return; }
    } else {
        if (v < 0 || v < 0xF) { objNudge(actor, -1); return; }
    }
    wr16(actor, 4u, 0x004Au);                        /* 1000:EBBa / 0xEB90 */
    node[7] = 0x0Cu;
}

/* DS:0x6E3C - the byte EBD0 adds to its 1/3 step, indexed by the node's
 * progress >> 3 (the values run 3, 2 x 7, 1 x 12, then 0). */
static const uint8_t objPathScale[0x20] = {
    0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 1000:EBD0: advance the node to the map under it.  It probes
 * (node.x + 8, node.y + 0x10), reads the item byte for the fine x (bit 7 picks
 * the 1/3 step, `word[node+8]`'s sign the other case), scales |progress| >> 3
 * through 0x6E3C (the 0x3B / 0x30 cells add 8 first), and - only while the
 * node's `+0xA` is 0 - stores the result at `+8` and the node's y at `+0x10`,
 * with effect 2. */
static void objPathAdvance(uint8_t *actor)
{
    uint8_t *node = objLinkRecord(rd16(actor, 0xEu));
    unsigned x, y, tile, fine, step;
    int ax;

    if (!node) return;
    if (node[7] != 0u) return;                       /* 1000:EBD3 */
    x = (unsigned)(rd16(node, 0u) + 8u);
    y = rd16(node, 2u);
    fine = x & 0xFu;
    tile = objMapAtAbs((int)x, (int)y + 0x10, NULL); /* 1000:EBEF: the map cell */
    step = 3;
    if (level_itemByte(tile, fine) & 0x80u) {        /* 1000:EC27 */
        if (!(rd16(node, 8u) & 0x8000u)) step = 1;
    } else if (rd16(node, 8u) & 0x8000u) {
        step = 1;
    }
    ax = (int16_t)rd16(node, 8u);                    /* 1000:EC43 */
    if (ax < 0) {
        ax = -ax;
        if (tile == 0x3Bu) ax += 8;                  /* 1000:EC53 */
    } else if (tile == 0x30u) {
        ax += 8;                                     /* 1000:EC5D */
    }
    ax >>= 3;
    step += (unsigned)(int8_t)objPathScale[(unsigned)ax & 0x1Fu];
    (void)tile;
    if (rd16(node, 0xAu) != 0u) return;              /* 1000:EC72 */
    wr16(node, 8u, (uint16_t)step);                  /* 1000:EC78 */
    wr16(node, 0x10u, (uint16_t)y);                  /* 1000:EC7A */
    objDriverCall(2);                                /* 1000:EC7E */
}


/* --- 1000:DC1F type15_tail_scan: the type-15 rows' tail -----------------
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
 * same window has its first member (`byte[+5]`) resolved to a record, which is
 * dissolved through `1000:FA13` before the entry itself takes the carried form.
 * The original writes that form into the entry's `+0xE`, which is the *next*
 * entry's first word (and past the table for the last one); the port stores
 * everything inside the entry and skips that one word, noted below.
 */
static void objCarryForm(uint8_t *rec, unsigned phase, int entry)
{
    uint16_t v = (uint16_t)(0xF0u + phase);

    wr16(rec, 4u, 0x0055u);                      /* 1000:DC9A */
    wr16(rec, 8u, 0x0010u);
    wr16(rec, 0xAu, 0u);
    wr16(rec, 0xCu, 0u);
    /* 1000:DCB0/DCBA store and then ADD into `[BP+0xE]` with no width test.  For
     * a placed record that is its last word; for a 0x0E-byte group entry it is
     * the *next* entry's first word, and for the last entry (0x6F22 + 0x70) it
     * is 0x6F92 - the span table's first word, i.e. the span's owner/free flag.
     * Both landings are reproduced, not skipped. */
    if (!entry) {
        wr16(rec, 0xEu, v);
    } else if (rec + 0xEu + 1 < (uint8_t *)objRegister + sizeof objRegister) {
        wr16(rec + 0xEu, 0u, v);
    } else {
        wr16((uint8_t *)objSpan[0], 0u, v);      /* 0x6F92 */
    }
    objDriverCall(8);                            /* 1000:DCBF */
}

static void objCmdType15Tail(uint8_t *actor)
{
    uint8_t *node = objLinkRecord(rd16(actor, 0xEu));
    int ax, bx;
    unsigned phase;

    if (!node) return;                           /* 1000:DC1F (DI = the node) */
    ax = (int)rd16(node, 0u) + 8;                /* 1000:DC32 */
    bx = (int)rd16(node, 2u) + 0x10;             /* 1000:DC34 */
    phase = objFrame & 0x3Fu;                    /* 1000:DC3D -> [0x4230] */
    if (rd16(node, 8u) != 0u) {                  /* 1000:DC22 */
        for (unsigned i = 0; i < 8u; i++) {      /* 1000:DC3A: CX = 7 */
            uint8_t *r = object_slot(i);
            int d;

            if (!r || r == actor) continue;      /* 1000:DC4B */
            d = (int)(int16_t)(uint16_t)(rd16(r, 2u) + 0x18u) - bx;
            if (d < 0) d = -d;
            if (d > 8) continue;                 /* 1000:DC72 */
            d = (int)(int16_t)rd16(r, 0u) - ax;
            if (d < 0) d = -d;
            if (d > 1) continue;                 /* 1000:DC83 */
            if (rd16(r, 8u) > 2u) continue;      /* 1000:DC8A */
            if ((int)i == object_player()) goto give;   /* 1000:DC91 */
            objCarryForm(r, phase, 0);           /* 1000:DC9A */
            return;
        }
    }
    for (unsigned g = 0; g < 8u; g++) {          /* 1000:DCD0 */
        uint8_t *e = (uint8_t *)objRegister[g];
        int d;

        if (rd16(e, 0u) & 0x8000u) continue;     /* 1000:DCEC */
        d = (int)(int16_t)(uint16_t)(rd16(e, 2u) + 0x18u) - bx;
        if (d < 0) d = -d;
        if (d > 8) continue;
        d = (int)(int16_t)(uint16_t)(rd16(e, 0u) + 8u) - ax;
        if (d < 0) d = -d;
        if (d > 1) continue;                     /* 1000:DD0F */
        if ((unsigned)e[5] == (unsigned)object_player()) goto give;  /* 1000:DD27 */
        object_groupDissolveAt(g);               /* 1000:DD2E */
        objCarryForm(e, phase, 1);               /* 1000:DC9A with BP = the entry */
        return;
    }
    return;

give:                                            /* 1000:DD3C */
    {
        unsigned cls = actor[7];
        unsigned slot;
        uint8_t *rec;

        actor[7] = 0u;
        if (!cls) return;
        if (cls < sizeof objSlotClass) objSlotClass[cls] &= 0x7Fu;
        slot = 8u + cls;                         /* 0x3F17 + 0x12*(cls-1) */
        if (slot >= OBJ_SLOTS) return;
        rec = objArray + slot * OBJ_STRIDE;
        wr16(rec, 0xAu, 1u);
        wr16(rec, 0xEu, 0xFFFFu);
        rec[6] &= 0x7Fu;                         /* 1000:DD63 */
        wr16(actor, 0u, rd16(rec, 0u));
        wr16(actor, 2u, (uint16_t)(rd16(rec, 2u) - 8u));
        wr16(actor, 4u, 0x0103u);
        wr16(actor, 8u, 0x001Eu);
        wr16(actor, 0xAu, 6u);
        wr16(actor, 0xCu, 0u);
        wr16(actor, 0xEu, 0x000Au);
    }
}

/* 1000:EA0F / 1000:EA4D: the two "ride" commands.  Both require the record's
 * behaviour state to be 2 (1000:F2CF) and the acting entity not to be in a
 * stack (1000:EA33); they then set a frame (0xAB / 0xAD) with a three-frame
 * hold and hand 1000:FC81 the x offset and the bit argument. */
static void objCmdRide(uint8_t *actor, unsigned frame, unsigned xoff, unsigned bit)
{
    if (!objStateIs(actor, 2u)) return;                   /* 1000:EA15 */
    if (objStackMasked()) return;                         /* 1000:EA1A */
    wr16(actor, 4u, (uint16_t)frame);
    wr16(actor, 0xAu, 3u);
    objSelType = bit;                                     /* 1000:EA29 / 1000:EA67 */
    objScanPoint(actor, xoff);                            /* 1000:FC81 */
}

/* 1000:ED4F / 1000:ED69 / 1000:ED83 / 1000:ED9D: the four one-step animation
 * nudges.  Each waits on the 25-frame cadence (1000:F9E4) and then moves the
 * frame one step toward its bound unless it is already there.  With [0x6F1D]
 * set, the handler is the enter-the-building sequence (1000:EE3B) instead. */
static void objCmdFrameStep(uint8_t *actor, unsigned bound, int up)
{
    unsigned f;

    if (objLat) { objCmdEnter(actor); return; }           /* 1000:ED56 */
    if (objFrameCadence(actor)) return;                   /* 1000:ED5C */
    f = rd16(actor, 4u);
    if (f == bound) return;
    wr16(actor, 4u, (uint16_t)(up ? f + 1u : f - 1u));
}

/* 1000:EDB7 (and its thunks 0xF0C3 / 0xF0C6): put the record into one of its
 * two home states.  Types 0x12/0x17/0x1F become frame 0, type 0; types
 * 0x14/0x18/0x20 become frame 8, type 1; anything else is left alone.  Either
 * way the pending words are cleared, the latch is released and [0x13A4] is set
 * to 0xFF (the ACTION press stays consumed). */
static int objCmdHomeState(uint8_t *actor)
{
    unsigned t = rd16(actor, 8u);

    if (t == 0x12u || t == 0x17u || t == 0x1Fu) {
        wr16(actor, 4u, 0u);                              /* 1000:EDD9 */
        wr16(actor, 8u, 0u);
    } else if (t == 0x14u || t == 0x18u || t == 0x20u) {
        wr16(actor, 4u, 8u);                              /* 1000:EDE5 */
        wr16(actor, 8u, 1u);
    } else {
        return 0;                                         /* 1000:EDD8 */
    }
    wr16(actor, 0xAu, 0u);                                /* 1000:EDEF */
    wr16(actor, 0xCu, 0u);
    wr16(actor, 0xEu, 0u);
    wr16(actor, 0x10u, 0u);
    objLat = 0;                                           /* 1000:EDFD */
    object_setActionLatched(1);                            /* 1000:EE01 */
    return 1;                                             /* AX = 0: converted */
}

/* 1000:EE07 (and 0xEFD1 / 0xEFD4, its jump-table aliases): advance the record's
 * direction pointer while the ACTION press is not being consumed.  The first
 * pass latches [0x6F1D] and clears the pointer; `+0xE` then walks 0..0x1FF in
 * steps of four (a 128-entry circle of four-word vectors).  `[0x6916]` becomes 4
 * unless the previous dispatch code was already 4 (the debounce byte 0x197A),
 * and 0x1987 is armed. */
static void objCmdTurn(uint8_t *actor)
{
    if (object_actionLatched()) return;                    /* 1000:EE0C */
    if (!objLat) {
        objLat = 0xFF;                                    /* 1000:EE16 */
        wr16(actor, 0xEu, 0u);
    }
    wr16(actor, 0xEu, (uint16_t)((rd16(actor, 0xEu) + 4u) & 0x1FFu));
    if (objDebounce != 4u) objOutState = 4;               /* 1000:EE30 */
    /* 1000:EE35 arms [0x1987]; the original's pending-event 4 (0x36DB, the
     * jump/throw charge loop) reads it at 0x370C and draws the charge bar.
     * The port has no event-4 handler yet (port_deviations.md D-68), so the
     * flag is currently written and dropped. */
}

static void objCmdFall(uint8_t *actor, int x, int y, unsigned frame);

/* 1000:EFE4: the *leave* sequence - what 1000:EFD7 jumps to when the latch is
 * set (the enter sequence's counterpart, reached from the type-23/24 rows'
 * idle command).  It releases the latch, derives the animation index from the
 * frozen direction counter (`+0xE`, folded at 0x100 and divided by 16), sets a
 * three-frame hold, bumps the type by 2 and the frame by 1, nudges the record
 * 4 px (right and down for frame 0xBB, else left) and runs the fall machine
 * with the (0/0 | 8/1) landing pair.  When the record did not land (`+0x10`
 * still set) the nudged position stays; otherwise the position is restored and
 * the landing pair is written back. */
static void objCmdLeave(uint8_t *actor)
{
    unsigned cx = rd16(actor, 0xEu);
    int ax = 4, bx = 2;
    int down = 1;

    objLat = 0;                                      /* 1000:EFE4 */
    if (rd16(actor, 4u) != 0x00BBu) {                /* 1000:EFF5 */
        ax = -4;
        bx = -2;
        down = 0;
    }
    wr16(actor, 0x10u, (uint16_t)(int16_t)bx);       /* 1000:F005 */
    wr16(actor, 0xEu, 0u);
    if (cx >= 0x100u) cx = 0x1FFu - cx;              /* 1000:F00D */
    cx >>= 4;                                        /* 1000:F019 */
    wr16(actor, 0xCu, (uint16_t)(0xFu - cx));        /* 1000:F021 */
    wr16(actor, 0xAu, 3u);                           /* 1000:F029 */
    wr16(actor, 8u, (uint16_t)(rd16(actor, 8u) + 2u));   /* 1000:F02E */
    wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + 1u));   /* 1000:F032 */
    {
        uint16_t sx = rd16(actor, 0u), sy = rd16(actor, 2u);

        wr16(actor, 0u, (uint16_t)(sx + (uint16_t)(int16_t)ax));
        wr16(actor, 2u, (uint16_t)(sy - 4u));        /* 1000:F03C */
        objDriverCall(2);                            /* 1000:F041 */
        objCmdFall(actor, down ? 0 : 8, down ? 0 : 1, down ? 0x10u : 0u);
        if (rd16(actor, 0x10u) == 0u) {              /* 1000:F04A */
            wr16(actor, 0u, sx);                     /* 1000:F051: restore */
            wr16(actor, 2u, sy);
            wr16(actor, 4u, objSelXoff);
            wr16(actor, 8u, objSelFrame);
        }
    }
}

/* 1000:EFD7: release the record's direction pointer while the latch is clear
 * (otherwise the enter sequence runs). */
static void objCmdTurnIdle(uint8_t *actor)
{
    if (objLat) { objCmdLeave(actor); return; }           /* 1000:EFDC -> 0xEFE4 */
    wr16(actor, 0xEu, 0u);
}

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
static const int8_t objFallStep[0x19] = {    /* 0x6EF3, 0x18 + the clamp entry */
    -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 1, 1,
     1, 1, 1, 2, 2, 0, 0, 0, 0,
};
/* 0x6EF2..0x6F14: the feet-cell comparison byte, indexed by the *type* (the
 * code reads 0x6F0B + (type - 0x19), which for types 0..0x18 wraps back into
 * 0x6EF3's tail; the flat table below is the same bytes). */
static const uint8_t objTypeTile[35] = {
    0x96, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x02, 0x00, 0x00, 0x49, 0xFA, 0x64, 0xFA, 0x78, 0xFA, 0x78, 0xFA,
};

static void objFallMachine(uint8_t *actor)
{
    int ax, bx, cx, d;
    unsigned rowY, fine, page, item, mask, lo, hi;
    unsigned probeY, above, below;
    unsigned anim, vy;

    if (!objSettleGate(actor)) {                     /* 1000:F6F6 */
        wr16(actor, 4u, objSelXoff);
        wr16(actor, 8u, objSelFrame);
        wr16(actor, 0xAu, 0u);
        return;
    }
    if (rd16(actor, 0xAu) == 1u)                     /* 1000:F70D DEC / JNZ */
        wr16(actor, 4u, (uint16_t)(rd16(actor, 4u) + 1u));
    wr16(actor, 0xAu, (uint16_t)(rd16(actor, 0xAu) - 1u));

    anim = rd16(actor, 0xCu);                        /* 1000:F715 */
    if (anim >= 0x18u) {
        vy = 3u;
    } else {
        vy = (unsigned)(uint8_t)objFallStep[anim];   /* sign-extended below */
        anim++;
    }
    wr16(actor, 0xCu, (uint16_t)anim);               /* 1000:F728 */
    wr16(actor, 0xEu, (uint16_t)(rd16(actor, 0xEu) + (uint16_t)vy));

    ax = (int)(int16_t)rd16(actor, 0u) + (int)(int16_t)rd16(actor, 0x10u);
    bx = (int)(int16_t)rd16(actor, 2u) + (int)(int16_t)(uint16_t)vy;
    cx = ax + 8;                                     /* 1000:F736: the probe x */
    probeY = (unsigned)(bx + 0x18);                  /* 1000:F739 */
    fine = (unsigned)cx & 0xFu;                      /* 1000:F753 */
    rowY = probeY & ~0xFu;                           /* 1000:F758: CX = the row */

    above = objMapAtAbs(cx, (int)rowY - 0x10, NULL); /* 1000:F761: [DI-0x20] */
    if (above >= 0xF0u) {                            /* 1000:F766 */
        /* 1000:F772: AX is the record.x + [+0x10] captured at 1000:F736, not
         * the probe x in CX (`ADD AX,8` then `AND AX,0xFFF0`). */
        unsigned gx = (unsigned)(ax + 8) & ~0xFu;

        if (above != 0xF1u) {
            gx -= 4u;                                /* 1000:F778 */
            if (above != 0xF2u) gx += 8u;
            wr16(actor, 0u, (uint16_t)gx);           /* 1000:F784 */
        }
        wr16(actor, 2u, (uint16_t)(rowY - 0x1Cu));   /* 1000:F786 */
        wr16(actor, 4u, 0x00CDu);
        wr16(actor, 8u, 0x001Cu);
        wr16(actor, 0xCu, 0u);
        wr16(actor, 0xEu, 0u);
        wr16(actor, 0xAu, 9u);
        objDriverCall(6);                            /* 1000:F7A6 */
        return;
    }
    if (bx >= 0x128) goto place;                     /* 1000:F7AE */
    below = objMapAtAbs(cx, (int)probeY, NULL);      /* 1000:F7B9: the feet */
    page = level_pageByte(below, fine);              /* 1000:F7F3 */
    if (rd16(actor, 0x10u) != 0u) {                  /* 1000:F7BB */
        unsigned want = objTypeTile[rd16(actor, 8u) < 35u ? rd16(actor, 8u) : 0u];

        if (below == want) {                         /* 1000:F7CD */
            unsigned b2 = objMapAtAbs(cx, (int)probeY + 0x10, NULL);

            if (level_pageByte(b2, 0u) == 0u) goto land_at_y;   /* 1000:F7EE */
        }
    }
    if (page == 0u) goto zero_case;                  /* 1000:F80A */
    if (page & 0x80u) {                              /* 1000:F80C JNS */
        unsigned b2;

        cx += 0x10;                                  /* 1000:F80E */
        b2 = objMapAtAbs(cx, (int)probeY + 0x10, NULL);
        page = level_pageByte(b2, fine);
        if (page & 0x80u) goto item_word;            /* 1000:F82F -> F8EC */
    }
    goto snap;                                       /* 1000:F832 -> F87F */

zero_case:
    {                                                /* 1000:F834 */
        unsigned a2 = objMapAtAbs(cx, (int)rowY - 0x10, NULL);
        unsigned want = objTypeTile[rd16(actor, 8u) < 35u ? rd16(actor, 8u) : 0u];
        /* 1000:F83A/F84C park these two in [0x422C]/[0x422E] only to compare
         * them; the port compares the cells directly. */
        if (want == a2) goto land_at_y;              /* 1000:F856 */
        if (level_pageByte(a2, fine) & 0x80u) goto snap;
        cx -= 0x10;                                  /* 1000:F86F */
        page = level_pageByte(a2, fine);
    }
snap:                                                /* 1000:F87F */
    objSelType = (uint16_t)(int16_t)(int8_t)(uint8_t)page;
    d = (int)(rowY + (unsigned)(int16_t)(int8_t)(uint8_t)page) - 0x18 - bx;
    if (d >= 0) goto item_word;                      /* 1000:F891 */
    d = -d;
    if (d > 3) goto item_word;
    bx -= d;                                         /* 1000:F89A */
    wr16(actor, 0u, (uint16_t)ax);
    wr16(actor, 2u, (uint16_t)bx);
    {
        int speed = (int16_t)rd16(actor, 0xEu);

        if (speed < 0) goto land_pair;               /* 1000:F8A6 */
        if (speed >= 0x30) goto hard_landing;        /* 1000:F8AB */
        level_set_offsetWord((uint16_t)((unsigned)speed >> 3));  /* 1000:F8CE */
    }
land_pair:                                           /* 1000:F8D2 */
    wr16(actor, 4u, objSelXoff);
    wr16(actor, 8u, objSelFrame);
    wr16(actor, 0xAu, 0u);
    objDriverCall(5);
    return;

item_word:
    {                                                /* 1000:F8EC */
        int px = ax + (int)(int16_t)objSelFrame;
        int py = bx + 0x18;
        unsigned tile = objMapAtAbs(px, py, NULL);

        item = level_itemWord(tile, (unsigned)((unsigned)py & 0xFu));
        objSelType = item;
        lo = 0xFFFFu;
        hi = 0u;
        if (!(rd16(actor, 0x10u) & 0x8000u)) {
            lo = 0u;
            hi = 0xFFFFu;
        }
        for (unsigned i = 0; i < fine; i++) {        /* 1000:F952 */
            unsigned carry = lo & 1u;

            lo >>= 1;
            hi = (hi >> 1) | (carry << 15);
        }
        mask = hi;
        if ((objSelType & mask) != 0u) goto land_at_y;   /* 1000:F964 -> F975 */
    }
place:                                               /* 1000:F966 */
    wr16(actor, 0u, (uint16_t)ax);
    wr16(actor, 2u, (uint16_t)bx);
    if (bx >= 0x140) goto hard_landing;
    return;

hard_landing:                                        /* 1000:F8AD */
    wr16(actor, 0u, (uint16_t)(rd16(actor, 0u) - 8u));
    wr16(actor, 4u, 0x0028u);
    wr16(actor, 8u, 0x0016u);
    wr16(actor, 0xAu, 5u);
    objDriverCall(4);
    return;

land_at_y:                                           /* 1000:F975 */
    wr16(actor, 2u, (uint16_t)bx);
    wr16(actor, 0x10u, 0u);
    if (rd16(actor, 0xCu) < 0x10u) wr16(actor, 0xCu, 0x0010u);
}

/* 1000:F067 / 0xF078: the two fall-machine entries.  The first lands with
 * frame 0 / type 0 and a 0x10 x offset, the second with frame 8 / type 1. */
static void objCmdFall(uint8_t *actor, int x, int y, unsigned frame)
{
    objSide = (uint16_t)(int16_t)x;
    objSelXoff = (uint16_t)y;
    objSelFrame = frame;
    objFallMachine(actor);
}

/* 1000:F08D / 1000:F0A2: the two landing placements - a frame/type pair and a
 * scratch point, then 1000:F23A. */
static void objCmdPlace(uint8_t *actor, int x, int y, unsigned frame)
{
    objSide = (uint16_t)(int16_t)x;                       /* 1000:F08D */
    objSelXoff = (uint16_t)y;
    objSelFrame = frame;
    objPlaceAt(actor);                                    /* 1000:F23A */
}

/* 1000:F0B3: the "stand" form - frame 0x10, type 2, a one-frame hold. */
static void objCmdStand(uint8_t *actor)
{
    wr16(actor, 4u, 0x0010u);
    wr16(actor, 8u, 0x0002u);
    wr16(actor, 0xAu, 0x0001u);
}

/* 1000:E7C0 stream_script_bytes_timed.  The dispatch's one-shot [0x6DEA]
 * gates it; when the hold 0x6DEC expires, fetch a frame/hold pair, set the
 * actor's +4 frame and the next hold, and wrap at 0xFF. */
static void objCmdStreamTimed(uint8_t *actor)
{
    const uint8_t *p;

    if (objCmdTimer != 0u) return;
    if (objStreamCount != 0u) { objStreamCount--; return; }
    objStreamCount = 0u;
    p = objStreamPtr;
    wr16(actor, 4u, p[0]);              /* MOV [SI+4], AX */
    objStreamCount = p[1];              /* [0x6DEC] */
    p += 2;
    if (*p == 0xFFu) p = objAnimStream; /* 1000:E7FA */
    objStreamPtr = p;                   /* [0x6DEE] */
}

/* 1000:EBCA: type 15 command 8 clears the record's +0xA hold. */
static void objCmdClearCountdown(uint8_t *actor)
{
    wr16(actor, 0xAu, 0u);
}

/* Test seams for the two handler bodies (the row selector's one-shot gate
 * keeps 1000:E7C0 from being driven directly in the unit test). */
void object_streamCommand(uint8_t *actor) { objCmdStreamTimed(actor); }
void object_clearCountdown(uint8_t *actor) { objCmdClearCountdown(actor); }
void object_setCommandTimer(unsigned v) { objCmdTimer = (uint16_t)v; }

static int objCmdCall(uint8_t *actor, unsigned addr)
{
    switch (addr) {
    case 0xD9C7u: return 1;                      /* the common tail: a bare RET */
    case 0xE4F8u: (void)object_stepDown(actor); return 1;
    case 0xE609u: (void)object_stepUp(actor); return 1;
    case 0xE72Au: objCmdIdleA(actor); return 1;
    case 0xE790u: objCmdIdleB(actor); return 1;
    case 0xE860u: (void)object_stackStepOff(actor); return 1;
    case 0xE8DDu: (void)object_stackLeave(actor); return 1;
    case 0xE963u: (void)object_stackSnap(actor); return 1;
    case 0xE99Du: (void)object_stackStand(actor); return 1;
    case 0xE93Du: (void)object_selectorWalk(actor, 0); return 1;
    case 0xE952u: (void)object_selectorWalk(actor, 1); return 1;
    /* the rest of the rows' handlers (see the block above) */
    case 0xE6FBu: objWalk(actor, 1); return 1;
    case 0xE764u: objWalk(actor, 0); return 1;
    case 0xEB78u: objCmdPathStep(actor, 0); return 1;
    case 0xEBA0u: objCmdPathStep(actor, 1); return 1;
    case 0xEBD0u: objPathAdvance(actor); return 1;
    case 0xDC1Fu: objCmdType15Tail(actor); return 1;   /* the type-15 rows' tail */
    /* the rows' tails (the +0xC word): 0xD9C7 is the bare RET above */
    case 0xD9C8u: objTailFrameBound(actor, 0x112u); return 1;
    case 0xD9CDu: objTailFrameBound(actor, 0x116u); return 1;
    case 0xD9E8u: objTailType3(actor); return 1;
    case 0xDB48u: objTailSpanOwner(actor, 0u, 0u, 0xABu); return 1;
    case 0xDB61u: objTailSpanOwner(actor, 8u, 1u, 0xADu); return 1;
    case 0xDD93u: objTailType16(actor); return 1;
    case 0xDDDAu: objTailType29(actor); return 1;
    case 0xE802u: objTailSpecialTile(actor, 4u, 0x16, 0); return 1;
    case 0xE831u: objTailSpecialTile(actor, 5u, 0x18, 1); return 1;
    case 0xEAACu: objTailType11(actor); return 1;
    case 0xEAE6u: objTailType12(actor); return 1;
    case 0xEB5Cu: objTailWalkCountdown(actor, 1); return 1;
    case 0xEB6Au: objTailWalkCountdown(actor, 0); return 1;
    case 0xECB7u: objTailType17(actor); return 1;
    case 0xEF0Au: return 1;                            /* a bare RET */
    case 0xEF0Bu: return 1;                            /* a bare RET */
    case 0xF067u: objCmdFall(actor, 0, 0, 0x10u); return 1;
    case 0xF078u: objCmdFall(actor, 8, 1, 0u); return 1;
    case 0xF0C9u: objTailType33(actor); return 1;
    case 0xDBCDu: objTailType28(actor); return 1;
    case 0xDE7Eu: objTailType30(actor); return 1;
    case 0xDED1u: objTailType34(actor); return 1;
    case 0xEF66u: objTailType22(actor); return 1;
    case 0xEA0Fu: objCmdRide(actor, 0xABu, 0xCu, 9u); return 1;
    case 0xEA4Du: objCmdRide(actor, 0xADu, 2u, 0xAu); return 1;
    case 0xEC99u: objWalk(actor, 0); objNeighbourPass(actor, 0xDu, 0xCu, 0xA); return 1;
    case 0xECA8u: objWalk(actor, 1); objNeighbourPass(actor, 3u, 0xCu, -0xA); return 1;
    case 0xED4Fu: objCmdFrameStep(actor, 0x8Eu, 1); return 1;
    case 0xED69u: objCmdFrameStep(actor, 0x8Cu, 0); return 1;
    case 0xED83u: objCmdFrameStep(actor, 0x91u, 0); return 1;
    case 0xED9Du: objCmdFrameStep(actor, 0x93u, 1); return 1;
    case 0xEDB7u: objCmdHomeState(actor); return 1;
    case 0xEE07u: objCmdTurn(actor); return 1;
    case 0xEE3Bu: objCmdEnter(actor); return 1;
    case 0xEFD1u: objCmdTurn(actor); return 1;
    case 0xEFD4u: objCmdTurn(actor); return 1;
    case 0xEFD7u: objCmdTurnIdle(actor); return 1;
    case 0xF08Du: objCmdPlace(actor, -8, 8, 1u); return 1;
    case 0xF0A2u: objCmdPlace(actor, 0x18, 0, 0u); return 1;
    case 0xF0B3u: objCmdStand(actor); return 1;
    case 0xF0C3u: objCmdHomeState(actor); return 1;
    case 0xF0C6u: objCmdHomeState(actor); return 1;
    case 0xE7C0u: objCmdStreamTimed(actor); return 1;
    case 0xEBCAu: objCmdClearCountdown(actor); return 1;
    case 0xEA0Eu: return 1;                            /* bare RET */
    default: return 0;
    }
}

/* 1000:E4B0..E4F7: run one command for the acting record.  The type row's
 * handler runs first, and when it changes the record's type the tail is skipped
 * (1000:E4CC); otherwise the row's +0xA hook (1000:D8BD, the terrain settle)
 * and then the tail handler run. */
int object_commandRun(uint8_t *actor, uint8_t input)
{
    ObjCommand c = object_commandSelect(actor, input);
    unsigned before = rd16(actor, 8u);
    int ran = OBJ_CMD_NONE;


    if (c.handler) {
        if (!objCmdCall(actor, c.handler)) return OBJ_CMD_UNPORTED;
        ran = OBJ_CMD_HANDLER;
        if (rd16(actor, 8u) != before) goto out;   /* 1000:E4CC */
    }
    if (c.hook) objCmdSettle(actor);               /* 1000:D8BD */
    if (c.tail) {
        if (!objCmdCall(actor, c.tail)) return OBJ_CMD_UNPORTED;
        ran = OBJ_CMD_TAIL;
    }
out:
    if (objOutState != 0u) {                       /* 1000:E4E4 */
        objDebounce = (uint8_t)objPending;         /* [0x197A] := the old [0x1979] */
        objPending = objOutState;
        objCursor = objOutStateHi;
    }
    return ran;
}

/* 1000:E4F8 (handler 1): the **step-down** state.  The cell at the feet
 * (`x + 8`, `y + 0x16`) decides: a special tile (field K -> `[0x6F20]`/`[0x6F21]`)
 * snaps the actor to the tile grid, a `0x6F92` span at that column takes it
 * onto the span, else the grab (1000:E58B) runs.  Returns 1 tile rule,
 * 2 span, 3 grabbed, 4 registered, -1 nothing. */
int object_stepDown(uint8_t *actor)
{
    int x = (int)(int16_t)rd16(actor, 0u);
    int y = (int)(int16_t)rd16(actor, 2u);
    unsigned sub, cell = objMapAtAbs(x + 8, y + 0x16, &sub);

    if (cell == level_specialTile(0u) || cell == level_specialTile(1u)) {
        wr16(actor, 0u, (uint16_t)((x + 8) & 0xFFF0));
        wr16(actor, 2u, (uint16_t)(y - 1));
        wr16(actor, 4u, 0x0023u);
        wr16(actor, 8u, 0x0004u);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        return 1;
    }
    for (unsigned i = 0; i < 2u; i++) {
        uint16_t *s = objSpan[i];
        int sx = (int16_t)s[1], sy = (int16_t)s[2], sh = (int16_t)s[3], d;

        if (rd16((uint8_t *)s, 0u) & 0x8000u) continue;
        d = sx - (x + 8);
        if (d < 0) d = -d;
        if (d >= 8) continue;                        /* |span.x - (x+8)| < 8 */
        d = sy - y;
        if (d >= 0 && d >= 0x18) continue;           /* span.y - y < 0x18 */
        if (sy + sh < y) continue;                   /* the span covers y */
        wr16(actor, 2u, (uint16_t)(y - 1));
        wr16(actor, 0u, (uint16_t)(sx - 8));
        wr16(actor, 4u, 0x0052u);
        wr16(actor, 8u, 0x000Bu);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        wr16(actor, 0xEu, (uint16_t)((uintptr_t)s - (uintptr_t)objSpan));
        objStackUnmark(actor);                       /* 1000:F3CA */
        return 2;
    }
    {
        int r = object_stackGrab(actor);

        return r == 1 ? 3 : (r == 0 ? 4 : -1);
    }
}

/* 1000:E609 (handler 2): the **walk-up** state.  Same three-way split, with
 * `x + 8, y + 0x19` as the feet cell; the span test uses `y + 0x18` as the line
 * and a 0x10-px column window, and the fall-through is the push (1000:E697).
 * Returns 1 tile rule, 2 span, 3 pushed, 4 nothing. */
int object_stepUp(uint8_t *actor)
{
    int x = (int)(int16_t)rd16(actor, 0u);
    int y = (int)(int16_t)rd16(actor, 2u);
    unsigned sub, cell = objMapAtAbs(x + 8, y + 0x19, &sub);

    if (cell == level_specialTile(0u) || cell == level_specialTile(1u)) {
        wr16(actor, 0u, (uint16_t)((x + 8) & 0xFFF0));
        wr16(actor, 2u, (uint16_t)(y + 1));
        wr16(actor, 4u, 0x0025u);
        wr16(actor, 8u, 0x0005u);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        return 1;
    }
    for (unsigned i = 0; i < 2u; i++) {
        uint16_t *s = objSpan[i];
        int sx = (int16_t)s[1], sy = (int16_t)s[2], sh = (int16_t)s[3], d;
        int line = y + 0x18;

        if (rd16((uint8_t *)s, 0u) & 0x8000u) continue;
        d = sx - (x + 8);
        if (d < 0) d = -d;
        if (d >= 0x10) continue;                     /* |span.x - (x+8)| < 0x10 */
        if (sy >= line) continue;
        if (sy + sh < line) continue;
        wr16(actor, 2u, (uint16_t)(y + 1));
        wr16(actor, 0u, (uint16_t)(sx - 8));
        wr16(actor, 4u, 0x0054u);
        wr16(actor, 8u, 0x000Cu);
        wr16(actor, 0xAu, 0x0000u);
        wr16(actor, 0xCu, 0x0000u);
        wr16(actor, 0xEu, (uint16_t)((uintptr_t)s - (uintptr_t)objSpan));
        objStackUnmark(actor);
        return 2;
    }
    return object_stackPush(actor) ? 3 : 4;
}

/* 1000:E93D / 1000:E952: the two wrappers that set the selector's constants and
 * jump into it - left: xoff -1, frame 8, type 1; right: xoff 1, frame 0, type 0. */
int object_selectorWalk(uint8_t *actor, int right)
{
    return right ? object_selector(actor, 1u, 0u, 0u)
                 : object_selector(actor, 0xFFFFu, 8u, 1u);
}

/* 1000:F17C script_op_select_on_zero: the object selector, and the stack's
 * "walk off" path.  `xoff` = [0x4234], `frame` = [0x4238], `type` = [0x423C] -
 * the two wrappers 1000:E93D (8/1) and 1000:E952 (0/0) set them and jump here.
 * Returns 0 = done, -1 = nothing found (the original's `JS`); the terrain
 * branch (`1000:F1D7`'s probe, `1000:F223`'s special-tile test) is in. */
int object_selector(uint8_t *actor, uint16_t xoff, uint16_t frame, uint16_t type)
{
    uint16_t ax;
    uint8_t *below, *entry;
    int r;

    objSelXoff = xoff;
    objSelFrame = frame;
    objSelType = type;
    ax = (uint16_t)(objSelXoff + rd16(actor, 0u));          /* 1000:F185 */
    r = object_stackPop(ax, &below, &entry);
    if (r < 0) return -1;                                  /* 1000:F18D */
    if (r == 0) {
        if (rd16(actor, 4u) != 0x20u) {                    /* 1000:F1F2 */
            int d = (int)(int16_t)rd16(actor, 4u) - (int)(int16_t)objSelFrame;

            if (d < 0 || d >= 8) {
                wr16(actor, 4u, objSelFrame);              /* 1000:F20C */
                return 0;
            }
            objWalk(actor, objSelType != 0u);              /* 1000:E6FB / 0xE764 */
            return 0;
        }
        wr16(actor, 4u, objSelFrame);                      /* 1000:F191 */
        return 0;
    }
    if (below) {                                           /* 1000:F198 */
        wr16(below, 2u, (uint16_t)(rd16(below, 2u) - 8u));
        wr16(below, 4u, 0x0020u);
    }
    wr16(actor, 4u, frame);
    wr16(actor, 8u, type);
    /* 1000:F1B2 also writes [0x6FE6] = 0xFF; that byte has no decoded reader. */
    {
        unsigned bit = objIndexBit((unsigned)(actor - objArray) / OBJ_STRIDE);

        if ((objStackMask[0] | objStackMask[1]) & bit) return 0;  /* 1000:F1CB */
    }
    /* 1000:F1D7: the step probe's distance test.  A blocked probe, or a step
     * of 10 px or more, goes to 1000:F223; a small step does nothing. */
    {
        unsigned cell = 0xFFu;
        int bx = objStepProbe(actor, &cell), d;

        if (bx >= 0) {
            d = bx - 0x18 - (int)(int16_t)rd16(actor, 2u);
            if (d < 0) d = -d;
            if (d - 0xA < 0) return 0;                 /* 1000:F1F1 */
        }
        /* 1000:F223: DI is the cell 1000:E389 ended on; when it is one of the
         * level's two special tiles (field K -> [0x6F20]/[0x6F21]) the actor
         * stays put, otherwise it takes the found entry's x. */
        if (cell == level_specialTile(0u) || cell == level_specialTile(1u)) return 0;
        if (entry) wr16(actor, 0u, rd16(entry, 0u));
        return 0;
    }
}

/* 1000:FADA: the per-member restore inside a dissolve. */
static void objGroupRestoreMember(uint8_t *rec)
{
    wr16(rec, 4u, 0x0010u);                  /* frame 0x10 */
    wr16(rec, 8u, 0x0003u);                  /* type 3     */
    wr16(rec, 0xAu, 0x0001u);
    wr16(rec, 0xCu, 0x0004u);
    wr16(rec, 0xEu, 0x0000u);
}

/* 1000:FA13 object_group_dissolve: free the entry, then give the owner (n = 0)
 * its standing human form, or each member (n >= 1) the FADA form.  `n` is the
 * member-list length - 1, exactly `(run length from +5) - 2` as the code
 * computes it; the `n >= 2` handler's second `XCHG CX,BX` at FA78 leaves
 * BX = n, so its loop runs `n - 1` times after the first member: n calls in
 * all, one per member. */
void object_groupDissolveAt(unsigned slot)
{
    if (slot < 8u) objGroupDissolve((uint8_t *)objRegister[slot]);
}

static void objGroupDissolve(uint8_t *e)
{
    uint8_t *bp;
    unsigned n;
    uint8_t *last;                 /* the topmost list byte's record */

    e[0] = 0xFF; e[1] = 0xFF;                /* +0/+2 = 0xFFFF: free */
    bp = e + 5;
    while (!(bp[0] & 0x80u)) bp++;            /* past the terminator */
    n = (unsigned)(bp - (e + 5)) - 1u;        /* list length - 1     */
    if (n == 0u) {
        last = object_slot(bp[-1]);           /* the owner byte      */
        if (!last) return;
        wr16(last, 4u, 0x0010u);              /* frame 0x10          */
        wr16(last, 8u, 0x0002u);              /* type 2, a human     */
        return;
    }
    last = object_slot(bp[-1]);               /* n = 1: the only member */
    if (!last) return;
    objGroupRestoreMember(last);
    for (unsigned k = 1u; k < n; k++) {       /* the members below it */
        uint8_t *m = object_slot(bp[-1 - (int)k]);
        if (m) objGroupRestoreMember(m);
    }
}

/* 1000:F453 scan_groups_for_record: the first 0x6F22 entry whose member list
 * (from +5) contains `recIndex` gets dissolved.  Returns 1 when one was. */
int object_stackScanDissolve(unsigned recIndex)
{
    for (unsigned i = 0; i < 8u; i++) {
        uint8_t *e = (uint8_t *)objRegister[i];
        if (e[1] & 0x80u) continue;                       /* +0 bit 15 set = free */
        for (uint8_t *p = e + 5; !(p[0] & 0x80u); p++)
            if (p[0] == (uint8_t)recIndex) { objGroupDissolve(e); return 1; }
    }
    return 0;
}

/* 0xEA71 (op 8): on odd frames reset the span entry of the acting record.
 * On even frames the original also clears [0x13A4]; the port's equivalent is
 * game_main's one-action-per-press latch (poll_input masks the ACTION bit
 * while [0x13A4] is set and clears it on release, 1000:9998). */
static void objOpResetSpan(void)
{
    if (!(objFrame & 1u)) return;
    for (unsigned i = 0; i < 2u; i++) {
        uint16_t *e = objSpan[i];
        if (e[0] != objEntity) continue;
        if (e[4] != 0u && e[3] > e[4]) { e[4] = 0u; e[9] = 0u; }
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
void object_pendingRun(void)
{
    if (objPending == 5u) {
        uint8_t *p = objPrograms[(objProgramIndex - 1u) % 6u];
        p[1] = (uint8_t)objCursor;
    }
    objPending = 0u;
}

/* script_op_dispatch: one ACTION press. */
void object_actionRun(void)
{
    const uint8_t *p = objPrograms[(objProgramIndex - 1u) % 6u];
    uint8_t *actor = objArray + objEntity * OBJ_STRIDE;
    unsigned op = p[p[1] + 1u];

    switch (op) {
    case OP_REGISTER: objOpRegister(actor); break;
    case OP_COLLIDE_A:
    case OP_COLLIDE_B: objOpCollide(actor); break;
    case OP_SETFRAME: objOpSetFrame(actor); break;
    case OP_CLAIM_OR_TYPE:     /* 0x39E4: the player claims, others walk */
        if (actor == objArray + objPlayer * OBJ_STRIDE)
            (void)object_claimBegin(2u, (int)objEntity);
        else
            objOpMarkWalk(actor, rd16(actor, 8u) ? 0u : 0x10u);
        break;
    case OP_CLAIM3: (void)object_claimBegin(3u, (int)objEntity); break;
    case OP_CLAIM0: (void)object_claimBegin(0u, (int)objEntity); break;
    case OP_CLAIM1: (void)object_claimBegin(1u, (int)objEntity); break;
    case OP_TYPE_A:            /* 0x3A05 -> 0xEFA1 / 0xEFB9          */
        if (rd16(actor, 8u) <= 2u) objOpSetState(actor, rd16(actor, 8u) ? 1u : 0u);
        break;
    case OP_RESET:             /* 0x39CC -> 0xEA71                   */
        objOpResetSpan();
        break;
    case OP_SELECT:            /* 0x3A1A -> 0xECFE                   */
        /* 0xECFE: needs a class, then page_offset_to_ptr (0xF123) on the map
         * cell under the record and on the cell one to the right (type 0) or
         * left; only when both are free does it set type 0x12/frame 0x8C
         * (type 0) or type 0x14/frame 0x91.  The 16-bit `DI += CX` can walk
         * off the map, so both indices are bounds-checked. */
        {
            unsigned t = rd16(actor, 8u);
            unsigned step = (t == 0u) ? 1u : 0xFFFFu;
            unsigned frame = (t == 0u) ? 0x8Cu : 0x91u;
            const uint8_t *map = level_map();
            int col = (int16_t)rd16(actor, 0u) >> 4;
            int row = (int16_t)rd16(actor, 2u) >> 4;
            int cell = row * 32 + col;
            int next = cell + (int)(int16_t)step;

            if (!actor[7u] || t > 2u || !map) break;
            if (cell < 0 || cell >= 640) break;
            if (!level_pageFree(map[cell])) break;
            if (next < 0 || next >= 640) break;
            if (!level_pageFree(map[next])) break;
            wr16(actor, 8u, (uint16_t)((t == 0u) ? 0x12u : 0x14u));
            wr16(actor, 4u, (uint16_t)frame);
            /* [0x6F1D] = 0 at 0xED49 (the dialog flag) — no port equivalent */
        }
        break;
    case OP_TYPE_B:            /* 0x39CF -> 0xEA0F / 0xEA4D          */
        /* Dispatched, and recorded — but its two targets are not callable, so
         * there is nothing to emulate beyond the dispatch itself:
         *  - type != 0 -> 0xEA4D is the operand byte 0xA2 of the JNS at
         *    0xEA4C: the store `MOV [0xE8C3], AL` followed by the CALL's own
         *    bytes as instructions;
         *  - type == 0 -> 0xEA0F is six bytes inside the 0xE9F0 loop, and that
         *    loop is never called anywhere (no caller in the image).  Reached
         *    from here its prologue reads the *acting record* as the argument
         *    list: DI = the record's frame (e.g. 0x0010), BP = its type
         *    (0..2) — then it writes `[BP] = AL`, i.e. into the interrupt
         *    vector table at linear 0-2, and walks 12 records from DI.
         * A faithful port of that is a guaranteed wild write, so the port
         * records the event instead of corrupting memory.  The shipped data
         * never produces it (cursor 1 -> prog[2]); reaching it means a cursor
         * > 1, which only the undecoded overlay 0FE7:0188 (level_run's
         * save-restore call at 0x4498) could supply.  Inert arms live in
         * re_plan.md, "op 7 (0x39CF) — revisit condition". */
        objFault = OP_TYPE_B;
        if (objOsys && objOsys->fatalError) {
            /* Reaching this means a cursor above 1 (the shipped data never
             * produces one).  The original's targets are wild writes, so the
             * port stops here, through the arch's own fatal route. */
            objOsys->fatalError(
                "HUMANS: ACTION op 7 (1000:39CF) dispatched — it needs a "
                "program cursor above 1, which the shipped data never "
                "produces.  Its targets are not callable (0xEA4D is the "
                "operand byte of a JNS; 0xEA0F writes [type] = AL into the "
                "interrupt vector table).  See re_map.md, 'op 7'.");
        }
        break;
    default:
        break;
    }
}

/* --- SAVE GAME state ranges (re_formats.md §10/§12.3) ------------------ */

/* 0x3E20..0x407E: [0x3E20] placed humans, [0x3E22] entity index, the 29
 * fixed pointers, the 20 class bytes, [0x3E73] live count and the 29 0x12
 * records at 0x3E75.  The pointer table is the image's constant run. */
void object_saveTable(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x25Fu);
    wr16(dst, 0u, objPlacedHumans);
    wr16(dst, 2u, objEntity);
    for (unsigned i = 0; i < OBJ_SLOTS; i++)
        wr16(dst, 4u + 2u * i, (uint16_t)(0x3E75u + 0x12u * i));
    for (unsigned i = 0; i < 20u; i++) dst[0x3Eu + i] = objSlotClass[i];
    dst[0x53] = (uint8_t)objLiveCount;          /* [0x3E73] */
    memcpy(dst + 0x55u, objArray, OBJ_SLOTS * OBJ_STRIDE);
}

void object_loadTable(const uint8_t *src)
{
    if (!src) return;
    objPlacedHumans = rd16(src, 0u);
    objEntity = rd16(src, 2u);
    if (objEntity >= OBJ_SLOTS) objEntity = 0u;
    memset(objSlotClass, 0, sizeof objSlotClass);
    for (unsigned i = 0; i < 20u; i++) objSlotClass[i] = src[0x3Eu + i];
    objLiveCount = src[0x53];
    if (objLiveCount > OBJ_SLOTS) objLiveCount = OBJ_SLOTS;
    memcpy(objArray, src + 0x55u, OBJ_SLOTS * OBJ_STRIDE);
}

/* 0x5B37..0x5B56: the six overlapping programs and [0x5B56].  The port keeps
 * six 12-byte copies; the original region is the 0x20-byte union, so save the
 * union and overlay each program's cursor byte at its table start. */
static const uint8_t objProgramBase[0x20] = {
    0x02,0x01,0x02,0x01,0x04,0x01,0x03,0x04,0x05,0x06,0x04,0x01,
    0x03,0x07,0x08,0x06,0x04,0x01,0x03,0x05,0x09,0x06,0x04,0x01,
    0x09,0x0A,0x0B,0x0C,0x01,0x01,0x06,0x01,
};
static const unsigned objProgramStart[6] = { 0u, 4u, 10u, 16u, 22u, 28u };
static uint8_t objProgramRegion[0x20];

void object_savePrograms(uint8_t *dst)
{
    if (!dst) return;
    memcpy(dst, objProgramBase, 0x20u);
    for (unsigned i = 0; i < 6u; i++)
        dst[objProgramStart[i] + 1u] = objPrograms[i][1];
    dst[0x1F] = (uint8_t)objProgramIndex;
}

void object_loadPrograms(const uint8_t *src)
{
    if (!src) return;
    memcpy(objProgramRegion, src, 0x20u);
    for (unsigned i = 0; i < 6u; i++) {
        unsigned start = objProgramStart[i];
        unsigned n = 0x20u - start;

        if (n > 12u) n = 12u;
        memcpy(objPrograms[i], objProgramRegion + start, n);
        /* n < 12 only for the last program: the save ends at 0x5B56, so its
         * tail is not in the file and the port keeps the static tail. */
    }
    objProgramIndex = src[0x1F];
    if (objProgramIndex < 1u || objProgramIndex > 6u) objProgramIndex = 1u;
}

/* 0x649A..0x64C2: the two TER.DAT slots (2 x 0x14). */
void object_saveTer(uint8_t *dst)
{
    if (!dst) return;
    memcpy(dst, objTerSlot[0], 0x14u);
    memcpy(dst + 0x14u, objTerSlot[1], 0x14u);
}

void object_loadTer(const uint8_t *src)
{
    if (!src) return;
    memcpy(objTerSlot[0], src, 0x14u);
    memcpy(objTerSlot[1], src + 0x14u, 0x14u);
}

/* 0x6F20..0x82EA: the special tiles, the 8 group templates at 0x6F22 and the
 * two 0x1C spans at 0x6F92; the image is zero elsewhere, and so is this. */
void object_savePool(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x13CAu);
    wr16(dst, 0u, (uint16_t)level_specialTile(0u));   /* [0x6F20] */
    wr16(dst, 2u, (uint16_t)level_specialTile(1u));   /* [0x6F21] */
    memcpy(dst + 2u, objRegister, sizeof objRegister);
    memcpy(dst + 0x72u, objSpan, sizeof objSpan);
}

void object_loadPool(const uint8_t *src)
{
    if (!src) return;
    memcpy(objRegister, src + 2u, sizeof objRegister);
    memcpy(objSpan, src + 0x72u, sizeof objSpan);
}

/* 0x6F92..0x6FE6's second copy: the two spans, then zero. */
void object_saveSpans(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x54u);
    memcpy(dst, objSpan, sizeof objSpan);
}

void object_loadSpans(const uint8_t *src)
{
    if (!src) return;
    memcpy(objSpan, src, sizeof objSpan);
}


/* 0x67FF: the pending-move (offset, divider) table; the word after each
 * entry is the next offset, so 0xFFFF ends the run and resets the cursor. */
static const uint16_t pendOff[17] = {
    1, 2, 3, 5, 2, 3, 5, 6, 2, 1, 2, 3, 2, 3, 4, 5, 0xFFFF,
};
static const uint16_t pendWait[16] = {
    0x28, 0x14, 0x0F, 0x05, 0x0A, 0x14, 0x1E, 0x19,
    0x08, 0x0C, 0x0F, 0x12, 0x32, 0x17, 0x3C, 0x0A,
};

/* 1000:D6B2 phases 1/2: walk the pending-move table, raising a record's
 * +4 tag 0x21 -> 0x22 for 0x28 ticks, then putting it back. */
static void objPendingMovesTick(void)
{
    unsigned placed = object_placedHumans();

    if (pendTag == 0u) {
        if (pendDiv-- > 0) return;
        if (pendCursor / 4u < 16u && placed != 0u) {
            unsigned idx = pendIndex + pendOff[pendCursor / 4u];

            while (idx >= placed) idx -= placed;
            pendDiv = pendWait[pendCursor / 4u];
            pendIndex = (uint16_t)idx;
            pendCursor = (uint16_t)(pendCursor + 4u);
            if (pendOff[pendCursor / 4u] == 0xFFFFu) pendCursor = 0u;
            {
                uint8_t *r = object_slot(idx);

                if (r && rd16(r, 4u) == 0x21u) {
                    wr16(r, 4u, 0x22u);
                    pendTag = 1u;
                    pendCount = 0x28u;
                    pendSlot = (uint16_t)idx;
                }
            }
        }
    } else if (pendCount-- == 0u) {
        uint8_t *r = object_slot(pendSlot);

        if (r && rd16(r, 4u) == 0x22u) wr16(r, 4u, 0x21u);
        pendTag = 0u;
        pendCount = 0u;
    }
}

/* --- 1000:D6B2 phase 3: the group-member animator ---------------------- */
/* 0x6F22 entry: word0 = base x (bit 15 free), +4 = direction<<4 | count,
 * +5.. = member list (bit-7 terminated).  Every 6th call the count steps
 * and each member record whose frame is 0x20..0x22 gets
 * x = group.word0 + objAnimX[length-3][count][member]. */
void object_animatorTick(void)
{
    objPendingMovesTick();

    if (objAnimDiv-- <= 0) objAnimDiv = 6;

    for (unsigned gi = 0; gi < 8u; gi++) {
        uint8_t *g = (uint8_t *)objRegister[gi];
        unsigned len = 0, limit, c;
        uint8_t dl;

        if (rd16(g, 0u) & 0x8000u) continue;
        while ((g[5u + len] & 0x80u) == 0) len++;
        if (len < 3u || len > 8u) continue;
        limit = 2u * (len - 1u);
        c = g[4] & 0x0Fu;
        /* 1000:D795: the state byte's high nibble is carried in DL (bits 4..7,
         * the 0x80 sentinel included) and only bit 4 is the direction. */
        dl = (uint8_t)(g[4] & 0xF0u);
        if (objAnimDiv == 0) {
            if (!(dl & 0x10u)) {
                if (c == 0u) dl ^= 0x10u;
                else c--;
            } else {
                if (c >= limit) { c = limit; dl ^= 0x10u; }
                else c++;
            }
        }
        if (c > limit) c = limit;
        /* 1000:D81D..D850: the member list is walked BACKWARDS from the
         * terminator while the offset run is read forwards, so the last (top)
         * member takes the run's first offset and the owner takes the last -
         * which is 0 in every shipped run.  `objAnimX[..][c][j]` is the run's
         * word j+1, so the list index m takes column `len - 1 - m`. */
        for (unsigned m = 0; m < len; m++) {
            uint8_t *r = object_slot(g[5u + m]);
            unsigned frame;

            if (!r) continue;
            frame = rd16(r, 4u);
            if (frame < 0x20u || frame > 0x22u) continue;
            wr16(r, 0u, (uint16_t)(rd16(g, 0u) +
                                   (uint16_t)objAnimX[len - 3u][c][len - 1u - m]));
        }
        g[4] = (uint8_t)(dl | (c & 0x0Fu));   /* 1000:D813 OR DL,CL */
    }
}

uint8_t *object_groupAt(unsigned i)
{
    return i < 8u ? (uint8_t *)objRegister[i] : NULL;
}
void object_animatorReset(void)
{
    objAnimDiv = 0;
    pendTag = pendCount = pendSlot = pendIndex = pendCursor = 0u;
    pendDiv = 0;
}
