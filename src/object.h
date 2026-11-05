/* object.h — the interface of src/object.c: the placed-object array (0x3E75)
 *
 * 8 records of 0x12 bytes (check_highscore 1000:41D7 walks all eight); the
 * fields and the remaining machine are in object.c's header.  The predicates
 * are object_test_dispatch's (1000:35D6): -1 = true, 0 = false.
 */

#ifndef HUMANSRE_OBJECT_H
#define HUMANSRE_OBJECT_H

#include "osystem.h"

/* 1000:E4B0's outcome for one command. */
#define OBJ_CMD_NONE     0 /* neither a handler nor a tail ran          */
#define OBJ_CMD_HANDLER  1 /* the type row's handler ran                */
#define OBJ_CMD_TAIL     2 /* the row's tail handler ran                */
#define OBJ_CMD_UNPORTED (-1) /* no port */

/* The adjacent-object op of the entity bytecode (1000:3A6A): the gate and the
 * scan around the *acting* record (`actor`, the entity's own slot), then the
 * red/white flash, the blinks, the animation, the append and the settle.
 * `recipe` = 1/0/3 for the three handlers.  object_tick drives the sequence
 * and supplies the level's palette (the fades return to it). */
/* 1000:E3F6's command dispatch: the acting record's handler for one input word
 * (`[0x5ACE]`).  `handler` is the in-segment address the original calls
 * (0 = none, so the row's `tail` applies); `command` is the 0..7 number the
 * type row was indexed by.  Index 2 is 1000:E609, whose tail holds the human
 * stack's push.  Every handler body is ported (the former `0xE7C0` and
 * `0xEBCA` gaps were closed). */
typedef struct {
    uint32_t handler;
    uint32_t tail;
    uint32_t hook; /* the row's +0xA: run 1000:D8BD before the tail */
    uint32_t command;
} ObjCommand;

/* the program/group header ([0x5B2B] -> 0x5B37): {count, cursor, icon...} */
const uint8_t *object_programHeader(uint32_t group);
const uint8_t *object_actionHeader(void);
void object_stepActionCell(void); /* 1000:3941: SPACE steps the cursor */
/* DEFS field G: the per-level 3-layer 0x100 sprites the frame slots 0xC1+4L+i
 * point at (level_setup_screen writes their far pointers into DS:0x3CAC). */
void object_publishGSprites(const uint8_t *data, uint32_t blocks, uint32_t n);
/* DEFS fields E and H: the 0xA9 log sprite (frames 0x33..0x37) and the two
 * sprites the frame slots 0x26/0x27 point at. */
void object_publishDEHSprites(const uint8_t *e, const uint8_t *h);
/* 1000:C295 update_list: the placed records' behaviour walk (torch, stepper,
 * sequence, wanderer). */
void object_updateList(void);
/* 1000:C2CE count_tile_collisions: the goal scan; sets [0x6530] when the count
 * of records standing on a 0xBA tile reaches the level's goal byte. */
void object_goalScan(void);
int32_t object_goalFlag(void); /* [0x6530] */
/* 1000:D860 collision_scan: the static records' per-frame row pass. */
void object_rowPass(void);
/* [0x6531], menu_flag_tick's mode flag (the marker's 1000:C541 arm). */
void object_setMarkerMode(int32_t on);
int32_t object_markerMode(void);

void object_init(void);
void object_blitSprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                       uint32_t cols, int32_t x, int32_t y);
/* 1000:5C2F draw_objects_pass2: the two TER.DAT slots at DS:0x649A, disabled by
 * object_array_init (0x4CAB) through their +0x10 flag words. */
void object_terSlotInit(void);
uint8_t *object_terSlotAt(uint32_t i); /* NULL past the two slots */
void object_publishTer(const uint8_t *bank, uint32_t size);
void object_renderTer(uint8_t *page, int32_t scrollX, int32_t scrollY);
uint8_t *object_slot(uint32_t i); /* NULL past object_slots()      */
uint32_t object_slots(void);
uint32_t object_frameSlots(void); /* the sprite table bound (0x11E) */
uint32_t object_frameCounterProbe(void); /* [0x4103] */
void object_frameForProbe(uint32_t frame, uint32_t *off, uint32_t *rows);
/* the 0x32B9 background-sprite queue (1000:5F54's input) */
uint32_t object_bgCount(void);
void object_bgAt(uint32_t i, uint32_t *x, uint32_t *y);
/* the SAVE GAME state ranges (re_formats.md §10/§12.3): object table
 * (0x3E20, 0x25F), pools (0x6F20, 0x13CA), programs (0x5B37, 0x20),
 * TER slots (0x649A, 0x28) and the 0x6F92 copy (0x54). */
void object_saveTable(uint8_t *dst);
void object_loadTable(const uint8_t *src);
void object_savePool(uint8_t *dst);
void object_loadPool(const uint8_t *src);
void object_savePrograms(uint8_t *dst);
void object_loadPrograms(const uint8_t *src);
void object_saveTer(uint8_t *dst);
void object_loadTer(const uint8_t *src);
void object_saveSpans(uint8_t *dst);
void object_loadSpans(const uint8_t *src);
int32_t object_current(void); /* [0x1AF2], -1 = none           */
void object_setCurrent(int32_t slot);
int32_t object_player(void); /* [0x180A], -1 = none           */
void object_setPlayer(int32_t slot);
int32_t object_noneType1d(void); /* 1000:3640                     */
int32_t object_classMatches(uint32_t want); /* 1000:3659                     */
int32_t object_typeEquals(uint32_t type); /* 1000:3678                     */
int32_t object_entitiesFinished(void); /* 1000:3688 (the entity table)  */
int32_t object_mapCellEquals(uint32_t cell); /* 1000:36AB (the map cell)     */
uint32_t object_staticSlots(void); /* the 8 the predicates scan     */
uint32_t object_liveCount(void); /* [0x3E73]; 9 at the level start */
uint8_t object_slotClass(uint32_t slot); /* [0x3E5E + slot]               */
uint8_t *object_linkForProbe(uint32_t off); /* the +0xE/+0xC link encoding */
uint8_t *object_spanAtForProbe(uint32_t i); /* the 0x6F92 span entry      */
void object_spanSeedForProbe(uint32_t i, uint32_t top); /* its seed row   */
int32_t object_landYForProbe(int32_t x, int32_t y, uint32_t *cellOut);
uint16_t object_spanFlagForProbe(int32_t d, uint32_t frame);
int32_t object_throwDxForProbe(uint32_t type); /* word[0x5BA4 + 2*type]  */
uint16_t object_recipeParamForProbe(uint32_t recipe, uint32_t k);
uint16_t object_pendOffForProbe(uint32_t i); /* the 0x67FF step table  */
uint16_t object_pendWaitForProbe(uint32_t i); /* and its waits         */
uint8_t object_dsTail5105(uint32_t off); /* 0x5105..0x5114 (objCmdEnter)  */
void object_setSlotClassForProbe(uint32_t slot, uint8_t cls);
void object_setSlotClass(uint32_t slot, uint32_t cls);
uint8_t object_behStateForProbe(uint32_t beh); /* DS:0x6EB6 window       */
int32_t object_testDispatch(uint16_t mode); /* 1000:35D6: 1 = goal met       */
/* render_objects' object pass (1000:5D56 -> 0x653C): the base sprite of every
 * live record, drawn at (x - scrollX, y - scrollY). */
void object_publishBank(const uint8_t *bank, uint32_t size);
/* load_terrain_b's per-tile object walk (1000:5168): [0x3E73] = 8, the 20
 * behaviour bytes cleared, then every map cell of the 25 table tiles in
 * table order (0xAA the human into the first free static slot, frame 0x10,
 * type 2; 0xAB the spear, appended, frame 0x60, +8 = 0xFFFF, behaviour 5;
 * the rest per re_map.md); returns [0x3E73]. */
uint32_t object_loadTerrainObjects(uint8_t *map);
uint32_t object_placedHumans(void); /* [0x3E20]                       */
/* the map cell under a static record (0xF761 with AX = 8, BX = 0xC); the
 * score's object bonus reads it: 0xA8 -> +1000, 0xA7 -> +2000 (1000:55FA). */
uint32_t object_recordMapCell(uint32_t slot);
void object_render(uint8_t *page, int32_t scrollX, int32_t scrollY);
ObjCommand object_commandSelect(uint8_t *rec, uint8_t input);
/* 1000:E4B0..E4F7: select and run one command for the acting record.  Only the
 * ported handlers are dispatched (the two movement states, the leaves, the snap,
 * the selector wrappers and the bare-RET tail). */
int32_t object_commandRun(uint8_t *actor, uint8_t input);
/* handler test seams: the two bodies and the one-shot gate. */
void object_streamCommand(uint8_t *actor);
void object_clearCountdown(uint8_t *actor);
void object_setCommandTimer(uint32_t v);
void object_animatorTick(void); /* 1000:D6B2 phase 3 */
uint8_t *object_groupAt(uint32_t i); /* 0x6F22 entry */
void object_animatorReset(void);

/* The human stack (`0x6F22`, 8 entries x 0x0E: +0/+2 x/y, bit 15 = free, +4 the
 * animator state, +5 the owner index, +6ff the bit-7-terminated member list).
 * `object_stackPush` is the push block of 1000:E609 (1000:E697) and
 * `object_stackGrab` the one of 1000:E4F8 (1000:E58B), `object_stackPop` is
 * 1000:FDED, the leaves are 1000:E860 / 1000:E8DD, the snap 1000:E963, the
 * selector 1000:F17C, and `object_stackScanDissolve` is 1000:F453 (the scan that
 * finds the group holding a record and dissolves it).  The `0x64AD`/`0x64C1`
 * in-a-stack masks (1000:F38F/F3CA) are written by the two pushes and read by
 * the selector. */
int32_t object_stackPush(uint8_t *actor); /* 1 = a group took it      */
int32_t object_stackGrab(uint8_t *actor); /* 1 = grabbed, 0 = registered */
/* -1 / 0 / 1 as 1000:FDED; `*below` = the record it leaves in DI and `*entry` =
 * the 0x6F22 entry it was scanning (1000:FDF7's DX, which 1000:F223 reads). */
int32_t object_stackPop(uint16_t ax, uint8_t **below, uint8_t **entry);
int32_t object_stackStepOff(uint8_t *actor); /* 1000:E860: 1/0/-1        */
int32_t object_stackLeave(uint8_t *actor); /* 1000:E8DD: 1/0/-1        */
int32_t object_stackSnap(uint8_t *actor); /* 1000:E963: 1/0           */
int32_t object_stackStand(uint8_t *actor); /* 1000:E99D: 1/0           */
/* 1000:F17C: 0 = done, -1 = nothing found.  The terrain branch is in
 * (1000:F1D7's step probe, 1000:F223's field-K special-tile test). */
int32_t object_selector(uint8_t *actor, uint16_t xoff, uint16_t frame,
                        uint16_t type);
int32_t
object_stackScanDissolve(uint32_t recIndex); /* 1 = a group was dissolved */
/* 1000:FA13 object_group_dissolve, by group slot (the 0x6F22 entry the tail holds
 * in BP when it calls it). */
void object_groupDissolveAt(uint32_t slot);
/* 1000:E4F8 / 1000:E609: the two movement states that feed the stack - the feet
 * cell's special tile (field K), a 0x6F92 span, else the grab / the push.
 * Codes: 1 tile rule, 2 span, 3 grabbed/pushed, 4 registered/nothing, -1 nothing. */
int32_t object_stepDown(uint8_t *actor);
int32_t object_stepUp(uint8_t *actor);
/* 1000:E93D (left) / 1000:E952 (right): the selector with the wrappers' constants. */
int32_t object_selectorWalk(uint8_t *actor, int32_t right);

int32_t object_claimBegin(uint32_t recipe, int32_t actor);
int32_t object_claimActive(void);
int32_t object_claimPanActive(void); /* the 1000:3D04 recolour frames */
void object_claimTick(const uint8_t *levelPal);
/* the scan at 4E91: the first record beside `actor` (|dx| <= 0x20,
 * |dy| <= 8, type <= 2, active) or -1. */
int32_t object_findNeighbour(int32_t actor);
/* one level frame of the object machine (the frame counter + the claim) */
void object_tick(const uint8_t *levelPal);
/* update_world's animator/bg tails (1000:80CC/0x80CF) and the claim's own
 * frame (frame counter + claim), separately for level_tick's [0xC] gate. */
void object_worldTick(void);
uint32_t object_worldTicksProbe(void);
void object_claimFrame(const uint8_t *levelPal);
int32_t object_lowPhaseMatch(void); /* 1000:3600: -1 when met         */
/* 1000:F423's window, against the type row 1000:E467 hands the hook as DI:
 * the row's own words are the "rect", so it never matches on shipped data -
 * the test asserts that over every type and both halves. */
int32_t object_rowWindowMatches(uint32_t type, uint32_t half, int32_t x,
                                int32_t y);
/* The ACTION program machine (script_op_dispatch, 1000:3979): ACTION with no
 * direction held runs the acting record's program.  The acting record is
 * [0x3E22]; object_actionRun() performs one press, object_cycleEntity(±1) is
 * the object selector (1000:410A/4147). */
void object_actionRun(void);
void object_pendingRun(void);
/* one frame of the event-4 charge loop (1000:3702); call after the frame */
int32_t object_chargeTick(void); /* the [0x1979] dispatch, per frame */
/* the charge loop is running (1000:3702): the status bar must not repaint
 * the band/action row while it is (D-81). */
int32_t object_chargeActive(void);
int32_t object_cycleEntity(int32_t step);
/* 1000:38C3: select record `index` (F1..F8 = scancode-0x3B), skipping a
 * disabled slot or type 0x1D.  Returns 1 when selected. */
int32_t object_selectByIndex(uint32_t index);
/* 1000:3933 (SPACE): home-state the acting record, else step the action row's
 * selected cell.  Returns 1 (the make is consumed). */
int32_t object_spaceAction(void);
uint32_t object_actionEntity(void);
/* level_fade_in's slot-8 init (1000:3344): frame 0x17, type 0x1A. */
void object_levelStartRecord(void);
/* 1000:D64F: the active-human marker (record 8) follows the acting record,
 * bobs through its type word and expires; update_world calls it every frame. */
void object_markerTick(void);
void object_setActionEntity(uint32_t index);
uint32_t object_actionProgram(void);
uint32_t object_actionPending(void);
void object_actionSetCursor(uint32_t cursor);
void object_actionSetProgram(uint32_t index); /* [0x5B56], 1..6        */
uint32_t object_actionFault(void);
/* 1000:83B7 driver_call: the last sfx number a handler asked for (the audio
 * surface has no sfx entry until M6, so the request is only recorded). */
uint32_t object_lastSfx(void);
/* [0x6FE6]: the marker 1000:EAE6/0xF1B2 write (no ported reader yet). */
int32_t object_flag6fE6(void);
void object_driverCall(uint32_t n); /* 1000:83B7 from another module */
/* [0x13A4]: the ACTION press's latch as the command handlers see it. */
int32_t object_actionLatched(void);
void object_setActionLatched(int32_t on); /* 7 = op 7 was dispatched */

/* The arch's fatal route for the op-7 halt (NULL detaches it, the tests do
 * that on purpose); level_init attaches the game's OSystem. */
void object_attachOsys(const OSystem *osys);

#endif /* HUMANSRE_OBJECT_H */
