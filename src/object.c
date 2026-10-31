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
 *                                from the recipe (4F43..4F4C -> 5049..5061)
 *
 * The placed records are built by `load_terrain_b`'s tile walk
 * (`object_loadTerrainObjects`: the map's `0xAA` humans + `0xAB` spears), their
 * sprites come from the measured storage law (`object_render`), and the claim
 * (`object_claimBegin`/`object_claimTick`) runs the whole flash/blink/animate/
 * append/settle sequence.  `[0x1AF2]` is the claimed record, `[0x180A]` the
 * player's own (`resolve_object_sprite` `1000:4147` gives it the program index).
 *
 * The ACTION machine (`script_op_dispatch` 1000:3979, `object_actionRun`) is
 * ported with ops 1, 2/6, 3, 4, 5, 8, 9 and 10/11/12, the collision pass with
 * both direction actions (the pickup writes the class — level 1's win), the
 * object selector and the pending-action arm; op 7 is dead code and halts.
 *
 * Not ported, each its own sub-machine:
 *  - the group's *push* (`1000:E697`) and the stack's remaining verbs — the
 *    scan and the dissolve (`1000:F453` / `1000:FA13`) are in;
 *  - the 23 tile handlers of `load_terrain_b`'s walk beyond the human/spear
 *    (the other levels' placed objects, e.g. `0xA9`, `0xB6`, `0xB7`, `0xB8`);
 *  - the claim's dirty-cell reveal (`init_dirty_cell_list` 1000:3E73, then
 *    `flush_dirty_cell_list` 1000:3F4A looping `queue_dirty_cell_packed`
 *    1000:3FB3 until `[0x1B02]`: 0x100 queued cells over the 0x2C0-byte list
 *    at 0x48BC) and its `claim_pan_view_two_frames` (1000:3D04);
 *  - the class/behaviour overlays of `1000:653C` (`DS:0x3566`/`0x368A`/
 *    `0x3766`, reached only when a record's `+7 != 0`) and the `[0x1996]` half
 *    of `1000:3600` (the frame table itself is complete now — see below — so the
 *    movers' and the stack's frames all draw);
 *  - the **human stack** `0x6F22` (8 x 0x0E, decoded 2026-10-30): op 1 already
 *    registers the base, but push (`1000:E697`), pop (`1000:FDED`), leave
 *    (`1000:E860`), snap (`1000:E963`), dissolve (`1000:FA13` + the `0x6F0D`
 *    table) and the animator (`1000:D76F` + `0x66B3`) are not implemented — and
 *    their verbs are reached from the `0xF453` scan and the dissolve it calls
 *    (both ported below); what is missing is the push that fills a member list.
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
static uint8_t objStackMask[2];         /* 0x64AD / 0x64C1: in-a-stack bits */
static int16_t objSide;                 /* [0x4230]: which side the settle stepped from */
static int objStepProbeAt(const uint8_t *actor, int dx, int dy, unsigned *cellOut);
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
    statusbar_setActionGroup(index);
}
static unsigned objPending;              /* [0x1979]                             */
static unsigned objCursor = 1;           /* [0x1988]                             */
static unsigned objFault;                /* op 7 was dispatched (see below)      */
static const OSystem *objOsys;           /* the fatal halt's route (osystem.h)   */
static uint16_t objRegister[8][7];       /* 0x6F22: 8 x 0xE (clear is 0x70 bytes at
                                          * 0x60F4; op 1 walks AX = 7..0), bit15 = free */
static uint16_t objSpan[2][14];          /* 0x6F92, 2 x 0x1C: {id, .. +6, +8, +0x12} */

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
    objPending = 0u;                 /* [0x1979]                          */
    objCursor = 1u;                  /* [0x1988]                          */
    objSetProgramIndex(1u);          /* [0x5B56]                          */
    objCmdTimer = 0u;                /* [0x6DEA]                          */
    objStackMask[0] = objStackMask[1] = 0u;
    objFault = 0u;
    memset(objRegister, 0xFF, sizeof objRegister);   /* 0x6F22, all free  */
    memset(objSpan, 0xFF, sizeof objSpan);   /* 0x6F92: 0x38 bytes at 0x60FF */
    for (unsigned i = 0; i < 4u; i++) objUses[i] = objRecipes[i][0];
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
 * [0x17F2] through the CS word table at 0x35F4.  Five are ported here; only
 * 0x3600 is left (it needs the 140-tick counter at [0x1976] and the [0x1996]
 * byte its second test reads).
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
static unsigned objMapCell(const uint8_t *rec)
{
    const uint8_t *map = level_map();
    unsigned x, y, cell;

    if (!map) return 0xFFFFu;
    x = (unsigned)rd16(rec, 0u) + 8u;
    y = (unsigned)rd16(rec, 2u) + 0xCu;
    if (y < 16u) return 0xFFFFu;
    cell = 32u * (y / 16u) + (x / 16u);
    return cell < 640u ? map[cell] : 0xFFFFu;
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
 * reads **the map cell under the record** (`0xF761` with AX = 8, BX = 0xC,
 * see `objMapCell`) and, if that is not the goal tile `0xA8`, the `[0x1996]`
 * byte (`[0x1976] + 0x20`).  A miss *clears* the counter `[0x1976]`; a hit
 * counts it up and at `0x8C` (140) the goal is met (-1).
 *
 * `[0x1996]`'s writer is still unknown, so only the map-cell half is
 * evaluated here.  SI comes from the caller (the dispatch's `0x8DF4`), i.e.
 * the acting entity's record — see `object_current`.
 */
int object_lowPhaseMatch(void)
{
    static unsigned lastFrame = ~0u;

    if (rd16(objActor(), 8u) > 2u) return 0;
    if (lastFrame == objFrame) return 0;            /* once per frame */
    lastFrame = objFrame;
    if (objMapCell(objActor()) != 0xA8u) {
        lowPhaseCount = 0u;                         /* 4A2C */
        return 0;
    }
    if (++lowPhaseCount != 0x8Cu) return 0;
    return -1;
}

/* --- the claim: the entity bytecode's adjacent-object op (1000:3A6A) ---
 *
 * The op runs with SI = the **acting entity's** record (`0x8DF4`: the current
 * entity index `[0x3E22]` through the pointer table `[0x3E24]`), and is
 * dispatched from the jump table at `CS:0x399C`; the three handlers at
 * 4E5A/4E5F/4E64 pass AX = 1, 0 and 3, the recipe index.  The claimed
 * record `[0x1AF2]` is only ever *written* here.
 *
 * The gate and the scan (4E6D..4ED9): the actor's type must be <= 2 and the
 * recipe (`202F:0x19BA + 12*AX`, `[recipe+0]` = the remaining uses) must
 * still have a use; then the other eight records are scanned in array order
 * for `|dx| <= 0x20`, `|dy| <= 8`, `type <= 2`, `+6` bit 7 clear.  The first
 * hit is the neighbour.
 *
 * The writes (4EDC..4F1E): the neighbour's source slot `[0x3E5F+class-1]` is
 * cleared and its live record (`0x3F17 + 0x12*(class-1)`) gets
 * `+6 = 0xFFFF`; `[0x1AF2]` becomes the neighbour with frame `0x103` and
 * flags 0; the **actor** gets frame `0x20`; the status bar goes off
 * (`[0x5B12] = 0`); the fade ramp `202F:0x48BC` is filled.
 *
 * The sequence (4F33..50F0), one port tick per original frame (`0x9F56` = one
 * vsync, `0x54CA`/`0x9BF8` = a presented frame):
 *
 *   red     entries 1..0x7F -> (0x3F,0,0), div 5, then -> the level div 0x1E
 *   white   1..0x7F -> (0x3F,0x3F,0x3F), div 5, then `0x5104`'s two-frame pan
 *           (**not ported**: the port's view is unscrolled), then -> the
 *           level div 0x19
 *   frame   one `0x9F56`
 *   blinks  10x: the 0x80 bank -> white div n, -> the level div n, n = 10..1
 *           (2 * (10+...+1) = 110 frames)
 *   anim    6 passes of 8 waits + the frame that does the `frame++` (4FE9),
 *           then two frames = 56 frames; the neighbour goes 0x103 -> 0x109
 *   append  (4FF9..5064) x from the neighbour, y = neighbour + 9, the stashed
 *           recipe fields, `[0x3E73]++`, the behaviour id into the source
 *           slots
 *   reveal  `init_dirty_cell_list` (0x5273) then `flush_dirty_cell_list`
 *           (0x534A) looping `queue_dirty_cell_packed` (0x53B3) until
 *           `[0x1B02]`: 0x100 dirty cells queued from the 0x1B76 coordinate
 *           table over the 0x48BC list, shaded from 0x19EA — **not ported**
 *           (the port redraws the page flat)
 *   hold    41 frames, then 1..0xBF -> white div 5, one frame, the neighbour's
 *           `+6 = 0xFFFF`, `[0xC] = [0x5B12] = 0xFF`, two page frames, then
 *           1..0xBF -> the level div 0x28
 */
enum {
    CLAIM_OFF,
    CLAIM_TO_RED,        /* 4F56  1..0x7F -> red, div 5                */
    CLAIM_RED_BACK,      /* 4F68  -> the level palette, div 0x1E       */
    CLAIM_TO_WHITE,      /* 4F82  1..0x7F -> white, div 5              */
    CLAIM_PAN,           /* 4F91  0x5104: the two-frame pan            */
    CLAIM_WHITE_BACK,    /* 4F97  -> the level palette, div 0x19       */
    CLAIM_FRAME,         /* 4FA9  one 0x9F56 frame, then the blinks    */
    CLAIM_BLINK_WHITE,   /* 4FB3  the 0x80 bank -> white, div n        */
    CLAIM_BLINK_LEVEL,   /* 4FC3  the 0x80 bank -> the level, div n    */
    CLAIM_ANIM,          /* 4FD6  6 x 9 frames, `frame++` per pass     */
    CLAIM_HOLD,          /* 507D  41 frames                            */
    CLAIM_WHITE_OUT,     /* 5088  1..0xBF -> white, div 5              */
    CLAIM_FRAME2,        /* 509F  one frame, then the remove           */
    CLAIM_FLIP,          /* 50B5  the two page frames                  */
    CLAIM_SETTLE         /* 50DE  1..0xBF -> the level, div 0x28       */
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

/* The scan at 4E91..4ED9, for the acting record `actor`. */
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

        if ((int)i == actor) continue;               /* 4E9C */
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
    if (!objUses[recipe]) return -1;                 /* 4E89: the use counter */

    n = object_findNeighbour(actor);
    if (n < 0) return -1;

    rec = objArray + n * OBJ_STRIDE;
    cls = rec[7u];
    if (cls) {                                       /* 4EE2..4F00 */
        unsigned slot = cls - 1u;
        objSlotClass[cls] = 0u;                      /* 0x3E5F + slot */
        live = objArray + (OBJ_LIVE0 + slot) * OBJ_STRIDE;
        wr16(live, 6u, 0xFFFFu);                     /* 0x3F17 + 0x12*slot */
    }
    object_setCurrent(n);                            /* 4F03 */
    wr16(rec, 4u, 0x0103u);                          /* 4F07 */
    wr16(rec, 6u, 0u);                               /* 4F0D */
    wr16(objArray + actor * OBJ_STRIDE, 4u, 0x0020u);/* 4F13 */
    statusbar_setActive(0);                          /* 4F1E: [0x5B12] = 0 */

    /* 4F34..4F46: spend the use and stash the recipe's fields (the recipe
     * itself is no longer readable once its use is gone) */
    objUses[recipe]--;
    claimBehav = objRecipes[recipe][1];
    claimSprite = (unsigned)((objRecipes[recipe][2] << 8) | objRecipes[recipe][3]);
    for (unsigned k = 0; k < 4u; k++)
        claimParams[k] = rd16(objRecipes[recipe], 4u + 2u * k);

    claimNeighbour = n;
    claimBlinks = 10u;                               /* 4FAC */
    claimTimer = 0u;
    claimFillRamp(claimRed, 0x3Fu, 0u, 0u);          /* 4F4F: AX=0, BL=0x3F */
    claimFillRamp(claimWhite, 0x3Fu, 0x3Fu, 0x3Fu);  /* 4F7A: AX=0x3F3F   */
    claimPhase = CLAIM_TO_RED;
    palette_buildFade(1u, 0x7Fu, 5u, claimRed + 3u); /* 4F56 */
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
    case CLAIM_TO_RED:                               /* 4F56 */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_RED_BACK;
        palette_buildFade(1u, 0x7Fu, 0x1Eu, levelPal + 3u);  /* 4F68 */
        break;
    case CLAIM_RED_BACK:                             /* 4F68 */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_TO_WHITE;
        palette_buildFade(1u, 0x7Fu, 5u, claimWhite + 3u);   /* 4F82 */
        break;
    case CLAIM_TO_WHITE:                             /* 4F91: 0x5104 */
        if (palette_fadeActive()) return;
        claimTimer = 0u;
        claimPhase = CLAIM_PAN;                      /* two frames */
        break;
    case CLAIM_PAN:                                  /* 4F91 -> 1000:3D04 */
        if (claimTimer == 0u) {
            /* 1000:3D41: SI = [0x180A] (the player), DI = [0x1AF2] (the claimed
             * record); AX/BX become min of the two x/y and 1000:3DC0 stores them
             * as [0x1AEA]/[0x1AEC] - the view's world origin. */
            uint8_t *pl = object_slot((unsigned)objPlayer);
            uint8_t *cl = object_slot((unsigned)objCurrent);

            if (pl && cl) {
                unsigned px = rd16(pl, 0u), cx = rd16(cl, 0u);
                unsigned py = rd16(pl, 2u), cy = rd16(cl, 2u);

                level_setView(px < cx ? px : cx, py < cy ? py : cy);
            }
        }
        if (++claimTimer < 2u) break;
        claimTimer = 0u;
        /* 1000:3DC0 leaves the origin set; in the original update_view
         * (1000:80F1) walks it back on the next frame.  That routine is not
         * ported, so the play origin is restored here rather than left stale. */
        level_setView(0u, 0u);
        claimPhase = CLAIM_WHITE_BACK;
        palette_buildFade(1u, 0x7Fu, 0x19u, levelPal + 3u);  /* 4F97 */
        break;
    case CLAIM_WHITE_BACK:                           /* 4FA9 */
        if (palette_fadeActive()) return;
        claimTimer = 0u;
        claimPhase = CLAIM_FRAME;
        break;
    case CLAIM_FRAME:
        if (++claimTimer <= 1u) break;               /* 4FA9's 0x9F56 */
        claimTimer = 0u;
        claimPhase = CLAIM_BLINK_WHITE;
        palette_buildFade(0x80u, 0x40u, claimBlinks, claimWhite); /* 4FB3 */
        break;
    case CLAIM_BLINK_WHITE:                          /* 4FB3 */
        if (palette_fadeActive()) return;
        claimPhase = CLAIM_BLINK_LEVEL;
        palette_buildFade(0x80u, 0x40u, claimBlinks,
                          levelPal + 0x80u * 3u);   /* 4FC3 */
        break;
    case CLAIM_BLINK_LEVEL:                          /* 4FC3 */
        if (palette_fadeActive()) return;
        if (--claimBlinks == 0u) {
            claimTimer = 0u;
            claimPhase = CLAIM_ANIM;
            break;
        }
        claimPhase = CLAIM_BLINK_WHITE;
        palette_buildFade(0x80u, 0x40u, claimBlinks, claimWhite);
        break;
    case CLAIM_ANIM:                                 /* 4FD6..4FF6 */
        if (++claimTimer % 9u == 0u)                 /* the 4FE9 frame++ */
            wr16(nrec, 4u, (uint16_t)(rd16(nrec, 4u) + 1u));
        if (claimTimer < 56u) break;
        claimTimer = 0u;
        /* 4FF9..5064: the append from the stashed fields.  The live record is
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
            /* 1000:5070 calls init_dirty_cell_list (1000:3E73), whose tail jumps
             * to 1000:3DC0 with (x - 0x20, y - 0x20) of the appended record: the
             * view scrolls onto it.  The 0x48BC cell list that routine also
             * builds is a redraw optimisation - the port composes the frame, so
             * only the view set has a visible effect. */
            level_setView((unsigned)(rd16(nr, 0u) - 0x20u),
                          (unsigned)(rd16(nr, 2u) - 0x20u));
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
    case CLAIM_HOLD:                                 /* 507D: 0x28+1 */
        if (++claimTimer < 41u) break;
        claimTimer = 0u;
        claimPhase = CLAIM_WHITE_OUT;
        palette_buildFade(1u, 0xBFu, 5u, claimWhite + 3u);   /* 5090 */
        break;
    case CLAIM_WHITE_OUT:                            /* 5088 */
        if (palette_fadeActive()) return;
        claimTimer = 0u;
        claimPhase = CLAIM_FRAME2;
        break;
    case CLAIM_FRAME2:                               /* 509F */
        if (++claimTimer <= 1u) break;               /* one 0x9F56 */
        claimTimer = 0u;
        wr16(nrec, 6u, 0xFFFFu);                     /* 50A6: removed */
        statusbar_setActive(1);                      /* 50B0: [0x5B12] */
        claimPhase = CLAIM_FLIP;
        break;
    case CLAIM_FLIP:                                 /* 50B5: LOOP CX=2 */
        if (++claimTimer < 2u) break;
        claimTimer = 0u;
        claimPhase = CLAIM_SETTLE;
        palette_buildFade(1u, 0xBFu, 0x28u, levelPal + 3u);  /* 50DE */
        break;
    case CLAIM_SETTLE:                               /* 50DE */
        if (palette_fadeActive()) return;
        /* 1000:3E73 scrolled the view onto the appended record; in the original
         * update_view (1000:80F1) re-centres it on the player on the next
         * frame.  That routine is not ported, so the play origin is restored. */
        level_setView(0u, 0u);
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
                           unsigned frame, unsigned behaviour, unsigned f8, unsigned fc)
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
        /* 0xAA/0xAB and the 0xF0..0xF2 decorations are ported; the rest of the
         * 25 are not (re_map.md has each one's body summary). */
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
                    objAppendStyle(map, col, row, 0x96u, 7u, 0xFFFFu, 0u);
                else if (tile == 0xADu)
                    objAppendStyle(map, col, row, 0x9Cu, 6u, 0xFFFFu, 0u);
                else if (tile == 0xAEu)
                    objAppendStyle(map, col, row, 0x38u, 4u, 0x0000u, 0xFFFFu);
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
#define OBJ_FRAME_SLOTS 0x110u

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

};

static const uint8_t *objBank;            /* APL\\SPRITES.DAT, the sprite image */
static uint32_t objBankSize;

void object_publishBank(const uint8_t *bank, uint32_t size)
{
    objBank = bank;
    objBankSize = size;
}

/* 1000:653C's base blit: the sprite at `src`, top-left (x,y), 0 = transparent.
 * `cols` is 16 for mode 1 (`1000:66EC`, block rows*4, row 4) and 32 for mode 2
 * (`1000:6C74`, block rows*8, row 8) — the only difference between the two
 * (the frame stride confirms it: mode-2 frames advance rows*32 bytes). */
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

/* render_objects' object pass: every live placed record, the base sprite only
 * (the class/behaviour overlays of 1000:653C are not ported — the placed
 * humans have +7 == 0 and never reach them). */
void object_render(uint8_t *page, int scrollX, int scrollY)
{
    if (!page || !objBank) return;

    for (unsigned i = 0; i < objLiveCount && i < OBJ_SLOTS; i++) {
        const uint8_t *r = objArray + i * OBJ_STRIDE;
        unsigned frame = rd16(r, 4u), cols;
        const ObjSprite *sp = NULL;

        if (r[6] & 0x80u) continue;                 /* 1000:794C */
        if (frame >= OBJ_FRAME_SLOTS) continue;
        sp = &objSprites[frame];
        if (!sp->rows || sp->mode > 2u) continue;   /* blank / mode 0 or 3: not drawn */
        cols = sp->mode == 2u ? 32u : 16u;          /* 1000:6C74 vs 1000:66EC */
        if ((uint32_t)sp->off + sp->rows * cols > objBankSize) continue;
        blitSprite(page, objBank + sp->off, sp->rows, cols,
                   (int)(int16_t)rd16(r, 0u) - scrollX,
                   (int)(int16_t)rd16(r, 2u) - scrollY + (int)sp->ybias);
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
 * direction the collision pass dispatches on).  DS:0x5B8A, the collision
 * pass's program id ([0x423C]), is only consumed by 0x990A, which is not
 * ported. */
static const uint8_t objBehProgram[8] = { 0, 0, 0, 0, 5, 1, 3, 2 };
static const uint8_t objBehDir[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 1, 1 };
/* DS:0x6EC3, 16 bytes: object_find_overlap's "this type does not take part"
 * flag.  Types 0..2 and 8 are 0 (they *do*), the rest 1.  Types >= 0x10 read
 * past the table in the original; the port treats them as "no". */
static const uint8_t objTypeOverlaps[16] = {
    0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1
};

unsigned object_actionEntity(void) { return objEntity; }
void object_setActionEntity(unsigned i) { objEntity = i < OBJ_STATIC ? i : objEntity; }
unsigned object_actionProgram(void) { return objProgramIndex; }
unsigned object_actionFault(void) { return objFault; }

/* `NULL` detaches the halt (the headless tests exercise op 7 on purpose). */
void object_attachOsys(const OSystem *osys) { objOsys = osys; }
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

/* 0x9C1B (ops 2/6): the collision pass over the live records. */
static void objOpCollide(uint8_t *actor)
{
    unsigned type = rd16(actor, 8u);
    int ax, bx;
    unsigned live;

    if (!(type == 0x0Fu || type <= 2u)) return;
    if (actor[7]) return;                        /* the class path (0x9D5D): not decoded */
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
        unsigned beh, dir;

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
        objPending = 5u;                         /* [0x1979] = 5 */
        objCursor = 1u;                          /* [0x1988] = 1 */
        objSetProgramIndex(1u);                  /* [0x5B56] = 1 */
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

static const uint8_t objCmdRows[35][32] = {
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
        di = e + 6;
        while (!(di[0] & 0x80u) && di[0] != (uint8_t)idx) di++;
        if (di[0] & 0x80u) continue;
        if ((uint16_t)(ey - 0x18u) != rd16(actor, 2u)) {
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
            objFrameStep(actor, 0x23u, 0x25u);             /* 1000:E934 */
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

    *sub = (unsigned)x & 0xFu;
    if (!map) return 0xFFu;
    cell = 32u * (((unsigned)y & 0xFFF0u) >> 4) + ((unsigned)x >> 4);
    return cell < 640u ? map[cell] : 0xFFu;
}

/* 1000:E389: the step test.  The cell ahead (x + 8, y + 0x18) and its 0x92A0
 * byte decide the y the walk lands on: a flat cell keeps y, a slope adds its
 * low bits, a solid cell takes the slope of the row below (blocked = -1 when
 * that is solid too) or, for a zero byte, the row above. */
static int objStepProbeAt(const uint8_t *actor, int dx, int dy, unsigned *cellOut)
{
    int x = (int)(int16_t)rd16(actor, 0u) + dx;
    int y = (int)(int16_t)rd16(actor, 2u) + dy;
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

static int objStepProbe(const uint8_t *actor, unsigned *cellOut)
{
    return objStepProbeAt(actor, 8, 0x18, cellOut);
}

/* 1000:F25F: whether the walk can land - the step probe's result must not drop
 * more than 8 px, and the item record's bit for the sub-cell 0x10 below the
 * actor's y must be clear.  Returns 0 = free, 0xFFFF = blocked. */
static unsigned objWalkTest(const uint8_t *actor)
{
    unsigned sub, id, bit, cell;
    int x, y, d;
    int bx = objStepProbe(actor, &cell);

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

/* 1000:F423: does the actor (BX = x, CX = y) fall inside the 0x6F92 span?  A
 * match does `OR byte[span+0x13], bit` and returns -1. */
static int objSpanRect(uint16_t *span, int bx, int cx, uint8_t bit)
{
    int d;

    if (rd16((uint8_t *)span, 0x10u) & 0x8000u) return 0;   /* free */
    d = (int16_t)rd16((uint8_t *)span, 0u) - 0x10 - bx;
    if (d >= 0) return 0;
    d += 0x1A;
    if (d < 0) return 0;
    d = (int16_t)rd16((uint8_t *)span, 2u) - cx;
    if (d < 0) return 0;
    d -= 5;
    if (d < 0 || d > 4) return 0;
    ((uint8_t *)span)[0x13] |= bit;
    return -1;
}

/* 1000:F3ED: clear the stack bits, then - unless the actor's type is marked at
 * 0x6EBE - let the two 0x6F92 spans claim it.  Returns 1 when the settle may
 * run (the original's ZF). */
static int objSettleGate(uint8_t *actor)
{
    unsigned t = rd16(actor, 8u);
    uint8_t bit = (uint8_t)(1u << (7u - (t & 7u)));

    objStackUnmark(actor);                                   /* 1000:F3CA */
    if (objTypeGate[(t >> 3) & 0x1Fu] & bit) return 1;       /* 1000:F450 */
    if (objSpanRect(objSpan[0], (int16_t)rd16(actor, 0u), (int16_t)rd16(actor, 2u), bit))
        return 0;
    if (objSpanRect(objSpan[1], (int16_t)rd16(actor, 0u), (int16_t)rd16(actor, 2u), bit))
        return 0;
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
 * and caches the settled position at 0x6847. */
static void objCmdSettle(uint8_t *actor)
{
    int x = (int16_t)rd16(actor, 0u);
    int y = (int16_t)rd16(actor, 2u);
    int ax = x + 8, line = y + 0x18, bx, d;
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
    d = (y + 0x18) - bx;
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
    default: return 0;
    }
}

/* [0x6916] / [0x6917]: the per-call output state the handlers may set; 1000:E4E7
 * copies it to [0x1979] / [0x1988] when it is non-zero.  No ported handler sets
 * it yet. */
static uint8_t objOutState, objOutStateHi;

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
 * Returns 0 = done, -1 = nothing found (the original's `JS`), -2 = the branch
 * needs the terrain step test (1000:E389 / 1000:F25F), which is not ported. */
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
