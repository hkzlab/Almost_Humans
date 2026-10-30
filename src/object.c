/* object.c — the placed-object array (0x3E75) and object_test_dispatch's
 * predicates (re_map.md §"`1000:3A6A` — the script op 10/11/12 spawn
 * sequence").
 *
 * The array is **8 records of 0x12 bytes at 0x3E75**: `check_highscore`
 * walks all eight (`1000:41D7`, AX = 7 down to 0) and the claim walks the
 * ones beside the current object (`1000:3A6A`).  Fields verified from those
 * two consumers:
 *
 *   +0x0  u16 x                  the claim's window is |dx| <= 0x20
 *   +0x2  u16 y                                and |dy| <= 8
 *   +0x4  u16 frame / sprite id  the claim sets the neighbour's to 0x103 and
 *                                the current object's to 0x20 (`1000:3B1E`)
 *   +0x6  u16 class<<8 | flags   low byte bit 7 = inactive (`TESTB $0x80,0x6`);
 *                                the high byte is the 0x3E5E slot index
 *                                (`MOV 0x7(%si),%al`), i.e. the class
 *   +0x8  u16 type               0x1D/0x1E are the "not a human" markers
 *                                (`check_highscore`); < 3 gates the claim
 *   +0xA..+0x11                  not decoded
 *
 * `[0x1AF2]` is the claimed (current) record and `[0x180A]` the player's own
 * (`1000:556C` gives it the action group 5).  Not here yet, each needing
 * another piece of the machine: the records' creation (`get_object_ptr`
 * `1000:79F4` from the entity/object ids, and the spawn recipes at
 * `202F:0x19BA`, appended at `0x3F17 + 0x12*([0x3E73]-8)`), the linked
 * live-record tables (`0x3E5E`/`0x3F1D`), the claim's flash/animate tail and
 * the object renderer (`render_objects` `1000:5D56`).  Until the spawn lands
 * the array is all zeros — the level start's state.
 */
#include "game.h"
#include <string.h>

#define OBJ_STATIC 8u                    /* the records 0x3640/check_highscore scan */
#define OBJ_SLOTS  16u                   /* + the live ones the spawn appends     */
#define OBJ_STRIDE 0x12u
#define OBJ_LIVE0  9u                    /* [0x3E73] = 9 at the level start      */

static uint8_t objArray[OBJ_SLOTS * OBJ_STRIDE];
static int objCurrent = -1;              /* [0x1AF2], -1 = none */
static int objPlayer = -1;               /* [0x180A], -1 = none */
static unsigned objLiveCount = OBJ_LIVE0;/* [0x3E73]: the array's used slots     */

/* [0x3E5E + b] — the class byte of the object in source slot b (0 = none).
 * `pred_sprite_class_match` (1000:3659) reads it as `[0x3E5E + rec[+7]] & 0x3F`
 * and the claim frees the slot (`[0x3E5E + rec[7]] = 0`, 1000:3B1E); the spawn
 * writes the behaviour id at `[0x3E5F + n]`, i.e. slot (n+1). */
static uint8_t objSlotClass[0x40];

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

void object_init(void)
{
    memset(objArray, 0, sizeof objArray);
    memset(objSlotClass, 0, sizeof objSlotClass);
    objCurrent = -1;
    objPlayer = -1;
    objLiveCount = OBJ_LIVE0;
    for (unsigned i = 0; i < 4u; i++) objUses[i] = objRecipes[i][0];
}

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
 * Six handlers are reachable from the level's goal-test byte stream at
 * [0x17F2] through the CS word table at 0x35F4.  Four are pure functions of
 * the array or the entity table and are ported here; 0x3600 and 0x36AB need
 * the random generator 0xF761 (0x3600 also keeps a 140-tick counter at
 * [0x1972]) and belong to the test-stream pass.
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

    if (objCurrent < 0) return 0;
    b = objArray[objCurrent * OBJ_STRIDE + 7u];
    if (!b) return 0;
    return ((objSlotClass[b] & 0x3Fu) == (uint8_t)want) ? -1 : 0;
}

/* 1000:3678 — the current record's type equals the test's word. */
int object_typeEquals(unsigned type)
{
    if (objCurrent < 0) return 0;
    return rd16(objArray + objCurrent * OBJ_STRIDE, 8u) == type ? -1 : 0;
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

/* --- the claim's neighbour scan and writes (1000:3A6A) ----------------
 *
 * SI = the current record; the scan takes the other records with
 * |dx| <= 0x20, |dy| <= 8, type < 3 and the flags' bit 7 clear.  The claim
 * then writes the neighbour's frame 0x103 and clears its flags word, and
 * gives the current record frame 0x20 (`1000:3B1E`).  The freed-slot
 * bookkeeping (0x3E5E/0x3F1D), the palette flash, the 5x7 animation and the
 * recipe spawn that follow are the object-machine pass. */
int object_findNeighbour(void)
{
    const uint8_t *cur;
    int cx, cy;

    if (objCurrent < 0) return -1;
    cur = objArray + objCurrent * OBJ_STRIDE;
    cx = (int16_t)rd16(cur, 0u);
    cy = (int16_t)rd16(cur, 2u);

    for (unsigned i = 0; i < OBJ_SLOTS; i++) {
        const uint8_t *r = objArray + i * OBJ_STRIDE;
        int dx, dy;

        if ((int)i == objCurrent) continue;
        if (r[6] & 0x80u) continue;                 /* inactive */
        if (rd16(r, 8u) >= 3u) continue;            /* type < 3 */
        dx = (int)(int16_t)rd16(r, 0u) - cx;
        dy = (int)(int16_t)rd16(r, 2u) - cy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= 0x20 && dy <= 8) return (int)i;
    }
    return -1;
}

int object_claimNeighbour(void)
{
    int n = object_findNeighbour();
    uint8_t *cur, *rec;

    if (n < 0) return -1;
    cur = objArray + objCurrent * OBJ_STRIDE;
    rec = objArray + n * OBJ_STRIDE;
    wr16(rec, 4u, 0x0103u);                 /* 1000:3B1E's writes */
    wr16(rec, 6u, 0x0000u);
    wr16(cur, 4u, 0x0020u);
    objSlotClass[rec[7u]] = 0u;             /* [0x3E5E + rec[7]] = 0 */
    return n;
}

/* object_test_dispatch (1000:35D6).  The level record's +0x04 word (the 13-byte
 * header copied to 202F:0x17F2) is the whole test: the low byte is the mode,
 * the high byte its argument (the handlers read DI = 0x17F3).  The mode's
 * low 7 bits index the CS word table at 0x35F4 = 0x3600, 0x3640, 0x3659,
 * 0x3678, 0x3688, 0x36AB; bit 7 first runs the entity scan (0x4A88 =
 * 1000:3688) whose result the dispatch discards — so it changes nothing and
 * the port masks it off.  Modes 0 and 5 need the random table 0xF761 and are
 * not evaluated: they report "not met".
 *
 * Returns 1 = the goal is met (the original's -1). */
int object_testDispatch(uint16_t mode)
{
    unsigned op = mode & 0x7Fu;
    unsigned arg = (mode >> 8) & 0xFFu;

    switch (op) {
    case 1: return object_noneType1d() == -1;         /* 1000:3640 */
    case 2: return object_classMatches(arg) == -1;    /* 1000:3659 */
    case 3: return object_typeEquals(arg) == -1;      /* 1000:3678 */
    case 4: return object_entitiesFinished() == -1;   /* 1000:3688 */
    default: return 0;                                /* 0/5: the RNG 0xF761 */
    }
}

/* The spawn (1000:3Bxx, the claim's tail): one of the four 0x19BA recipes,
 * one use per level, appended at the live records' end ([0x3E73]++), with the
 * recipe's behaviour id in the record's source-slot byte.  `x`/`y` are the
 * neighbour's, y + 9 (the claim's step).  -1 = the recipe is spent or the
 * array is full. */
int object_spawnRecipe(unsigned recipe, unsigned x, unsigned y)
{
    uint8_t *rec;

    if (recipe >= 4u || !objUses[recipe]) return -1;
    if (objLiveCount >= OBJ_SLOTS) return -1;
    rec = objArray + objLiveCount * OBJ_STRIDE;

    wr16(rec, 0u, (uint16_t)x);
    wr16(rec, 2u, (uint16_t)(y + 9u));
    /* the recipe stores the sprite word byte-swapped (re_map's table) */
    wr16(rec, 4u, (uint16_t)((objRecipes[recipe][2] << 8) |
                             objRecipes[recipe][3]));
    wr16(rec, 6u, 0u);
    wr16(rec, 8u, rd16(objRecipes[recipe], 4u));
    wr16(rec, 0xAu, rd16(objRecipes[recipe], 6u));
    wr16(rec, 0xCu, rd16(objRecipes[recipe], 8u));
    wr16(rec, 0xEu, rd16(objRecipes[recipe], 10u));
    objSlotClass[objLiveCount + 1u] = objRecipes[recipe][1];
    objUses[recipe]--;
    objLiveCount++;
    return (int)(objLiveCount - 1u);
}
