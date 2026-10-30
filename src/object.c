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
 *  - the per-record behaviour script (`0xF453`, run by the overlap fixup) —
 *    the object-side script runner;
 *  - the 23 tile handlers of `load_terrain_b`'s walk beyond the human/spear
 *    (the other levels' placed objects, e.g. `0xA9`, `0xB6`, `0xB7`, `0xB8`);
 *  - the claim's dirty-cell reveal (`init_dirty_cell_list` 1000:3E73, then
 *    `flush_dirty_cell_list` 1000:3F4A looping `queue_dirty_cell_packed`
 *    1000:3FB3 until `[0x1B02]`: 0x100 queued cells over the 0x2C0-byte list
 *    at 0x48BC) and its `claim_pan_view_two_frames` (1000:3D04);
 *  - the class/behaviour overlays of `1000:653C` (`DS:0x3566`/`0x368A`/
 *    `0x3766`, reached only when a record's `+7 != 0`), the `[0x1996]` half of
 *    `1000:3600`, and the mover frames (`0x12/0x14/0x8C/0x91`) which are not
 *    in the sprite table.
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
static uint16_t objRegister[7][7];       /* 0x6F22, stride 0xE, bit15 = free     */
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
    objFault = 0u;
    memset(objRegister, 0xFF, sizeof objRegister);   /* 0x6F22, all free  */
    memset(objSpan, 0, sizeof objSpan);
    for (unsigned i = 0; i < 2u; i++) objSpan[i][0] = 0xFFFFu;  /* 0x6F92 */
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
    case CLAIM_PAN:
        if (++claimTimer < 2u) break;
        claimTimer = 0u;
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
static void objPlaceHuman(unsigned x, unsigned y)
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
        return;
    }
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

unsigned object_loadTerrainObjects(const uint8_t *map)
{
    if (!map) return objLiveCount;
    objLiveCount = OBJ_LIVE0;                    /* 1000:516F */
    memset(objSlotClass, 0, 20u);                /* 1000:5175: 0x14 bytes */
    for (unsigned t = 0; t < 25u; t++) {
        uint8_t tile = objTiles[t];
        if (tile != 0xAAu && tile != 0xABu) continue;   /* not ported */
        for (unsigned row = 0; row < OBJ_ROWS; row++)
            for (unsigned col = 0; col < OBJ_COLS; col++) {
                if (map[row * OBJ_COLS + col] != tile) continue;
                /* live-confirmed (2026-10-30): level 1's records read
                 * (48,25) (208,25) (256,137) (464,265) = row*16 - 7, exactly
                 * what 0x524B writes. */
                if (tile == 0xAAu)
                    objPlaceHuman(col * 16u, row * 16u - 7u);
                else
                    objAppendSpear(col * 16u, row * 16u);
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
 * The frame addresses come from the runtime descriptor table `DS:0x39A8`,
 * whose entries are LINEAR ADDRESSES HALVED: `data = desc.offset*2 - K` with
 * `K = image_base - bank*0x20000` (live: K = 0x15790, bank 2).  The table is
 * built at load, so these values were read from the running game and are
 * recorded -- the *relation* is derived, the numbers are measured:
 *
 *   frame 0x10 -> 0x4800, h 24   the caveman    (verified 294/294 px live)
 *   frame 0x38 -> 0x0650, h 16   the stone
 *   frame 0x60 -> 0x10A0, h 16   the spear
 *   frame 0x96 -> 0x20B0, h  6   the campfire
 *   frame 0x9C -> 0x2560, h 12   the torch
 *
 * All five render as those objects, which is what confirms the law for the
 * non-24-row classes.
 */
typedef struct { uint8_t frame, rows; uint16_t off; } ObjSprite;

static const ObjSprite objSprites[] = {
    { 0x10u, 24u, 0x4800u },      /* the placed human         */
    { 0x38u, 16u, 0x0650u },      /* the stone                */
    { 0x60u, 16u, 0x10A0u },      /* the spear                */
    { 0x96u,  6u, 0x20B0u },      /* the campfire             */
    { 0x9Cu, 12u, 0x2560u },      /* the torch                */
};

static const uint8_t *objBank;            /* APL\\SPRITES.DAT, the sprite image */
static uint32_t objBankSize;

void object_publishBank(const uint8_t *bank, uint32_t size)
{
    objBank = bank;
    objBankSize = size;
}

/* 1000:653C's base blit: the sprite at `src`, top-left (x,y), 0 = transparent. */
static void blitSprite(uint8_t *page, const uint8_t *src, unsigned rows,
                       int x, int y)
{
    unsigned block = rows * 4u;

    for (unsigned row = 0; row < rows; row++) {
        int py = y + (int)row;
        if (py < 0 || py >= 200) continue;
        for (unsigned col = 0; col < 16u; col++) {
            uint8_t v = src[(col & 3u) * block + row * 4u + (col >> 2)];
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
        unsigned frame = rd16(r, 4u);
        const ObjSprite *sp = NULL;

        if (r[6] & 0x80u) continue;                 /* 1000:794C */
        for (unsigned k = 0; k < sizeof objSprites / sizeof objSprites[0]; k++)
            if (objSprites[k].frame == frame) { sp = &objSprites[k]; break; }
        if (!sp) continue;
        if ((uint32_t)sp->off + 4u * sp->rows * 4u > objBankSize) continue;
        blitSprite(page, objBank + sp->off, sp->rows,
                   (int)(int16_t)rd16(r, 0u) - scrollX,
                   (int)(int16_t)rd16(r, 2u) - scrollY);
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
 *   1  0x3A26 register the object in the 0x6F22 table (7 slots, stride 0xE), frame 0x20, type 8
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
    for (unsigned i = 0; i < 7u; i++) {
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
 * runs 0xF453 (the record's behaviour script, DX = 5) before the conversion;
 * that is the entity bytecode machine, which the port does not have. */
static void objFindOverlap(uint8_t *actor)
{
    int ax = (int16_t)rd16(actor, 0u);
    int bx = (int16_t)rd16(actor, 2u);

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
        /* 0xF453(r, DX = 5): the record's behaviour script — not ported */
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
