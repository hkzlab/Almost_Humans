/* scene.c — the .ANI/.RSC interpreter (game_structure.md §1 subsystem 11,
 * re_formats.md §8.5-8.7a): the playlist script, the display list, the
 * frame-keyed event cues.
 *
 * scene_playLevelIntro(introIndex) ports cd_file_op (1000:AFF8) as a
 * per-tick state machine: the first call for an introIndex loads
 * ANM\ANI<idx>.ANI from the table-selected disc, draws the scoops.apl
 * background over the daily.bit screen, sets the palette, and dwells 210
 * ticks; each further call plays the next scene object (6 ticks each) and
 * runs the chain table (202F:0x6293).  Returns 1 while the intro is
 * playing, 0 when it is finished (or introIndex == 0xFF: no cutscene).
 * Any key/fire skips or quits, per the original's three [0x48A3] checks.
 *
 * The AL expander (blit_background_data, 1000:B2EB) and the LZW machine
 * (decode_bitstream, 1000:91B9, via res.c) are real and byte-verified
 * against the validated python decoder (tools/ani_lzw.py).
 */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANI_NAME "ANM\\ANI%02u.ANI"
#define PAGE_SIZE (320u * 200u)
#define OBJ_SEG_SIZE 65536u   /* the original decodes objects into a 64K segment */

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)(unsigned)p[0] | ((uint32_t)(unsigned)p[1] << 8) |
           ((uint32_t)(unsigned)p[2] << 16) | ((uint32_t)(unsigned)p[3] << 24);
}

/* --- AL expander: blit_background_data (1000:B2EB) --------------------
 *
 * Source = AL stream, destination = page.  Grammar (from the listing):
 *
 *   01..7F   copy CL bytes literally from the stream
 *   00       [count][byte]: store byte COUNT times
 *   81..FF   skip (CL - 0x80) bytes in the page
 *   80       16-bit control word CW:
 *             CW > 0     skip CW bytes
 *             CW == 0    END
 *             CW < 0     CX = CW & 0x7FFF bytes;
 *                        (CH & 0x7F) < 0x40  -> copy CX bytes from stream
 *                        (CH & 0x7F) >= 0x40 -> fill (CX - 0x4000) bytes
 *                          with the next stream byte (SUB CH,0x40 at b365)
 */
static void scene_al_expand(const uint8_t *src, uint32_t src_size,
                            uint8_t *dst, uint32_t dst_size)
{
    uint32_t si = 0, di = 0;

    while (si < src_size) {
        uint8_t cl = src[si++];

        if (cl == 0) {                                    /* [count][byte] run */
            if (si + 1 >= src_size) break;
            uint8_t count = src[si++];
            uint8_t fill = src[si++];
            for (uint32_t i = 0; i < count && di < dst_size; i++)
                dst[di++] = fill;
            continue;
        }

        if (cl < 0x80) {                                  /* literal copy */
            for (uint32_t i = 0; i < cl && si < src_size && di < dst_size; i++)
                dst[di++] = src[si++];
            continue;
        }

        if (cl == 0x80) {                                 /* 16-bit control word */
            if (si + 1 >= src_size) break;
            uint16_t cw = (uint16_t)(src[si] | (src[si + 1] << 8));
            si += 2;
            if (cw == 0) break;                           /* END */
            if (!(cw & 0x8000u)) {                        /* skip CW bytes */
                di += cw;
                if (di > dst_size) di = dst_size;
                continue;
            }
            /* CW < 0: bit 15 set */
            uint16_t cx = (uint16_t)(cw & 0x7fff);
            uint8_t ch = (uint8_t)((cw >> 8) & 0x7f);     /* high byte, bit 15 cleared */
            if (ch < 0x40) {                              /* copy CX bytes from stream */
                for (uint32_t i = 0; i < cx && si < src_size && di < dst_size; i++)
                    dst[di++] = src[si++];
            } else {                                      /* fill (CX - 0x4000) bytes */
                if (si >= src_size) break;
                uint8_t fill = src[si++];
                uint32_t run = (uint32_t)cx - 0x4000u;    /* the listing's SUB CH,0x40 (b365) */
                for (uint32_t i = 0; i < run && di < dst_size; i++)
                    dst[di++] = fill;
            }
            continue;
        }

        /* 0x81..0xFF: skip (CL - 0x80) bytes in the page */
        di += (uint32_t)(cl - 0x80);
        if (di > dst_size) di = dst_size;
    }
}

/* --- ANI image handling ------------------------------------------------

 * The ANI chunk image is:
 *   u32[0] = table_size_bytes (also entry[0])
 *   u32[1..n-1] = byte offsets of blobs 1..n-1 into the image
 *   blob k = [entry[k], entry[k+1])
 *
 * Each blob is an LZW object: [u16 decompressed_size][u16 checksum]
 * [u16 bit buffer][LZW bits].  Object 0 is the script object.
 */

typedef struct {
    uint8_t  *image;
    uint32_t  image_size;
    uint32_t  table_size;
    unsigned  n_entries;
    uint32_t  *entries;        /* copy of the u32 offset table           */
} Ani;

static int ani_load(Ani *a, unsigned ani_idx)
{
    char name[32];
    snprintf(name, sizeof name, ANI_NAME, ani_idx);

    uint16_t chk = 0;
    (void)chk;
    a->image = res_loadChunk(name, &a->image_size, &chk);
    if (!a->image) return -1;

    a->table_size = rd_le32(a->image);
    a->n_entries = a->table_size / 4;
    a->entries = malloc(a->n_entries * sizeof(uint32_t));
    if (!a->entries) {
        free(a->image);
        a->image = NULL;
        return -1;
    }
    for (unsigned i = 0; i < a->n_entries; i++)
        a->entries[i] = rd_le32(a->image + i * 4u);
    return 0;
}

/* Decode the LZW object at table entry `idx` into `out` (capacity
 * `out_size`).  Returns the decompressed size, or -1 on failure.
 * *chkOut receives the object's checksum accumulator (0 = the game's
 * scene-object test passes, 1000:B444). */
static int ani_decode_object(const Ani *a, unsigned idx,
                             uint8_t *out, uint32_t out_size, uint16_t *chkOut)
{
    if (idx >= a->n_entries) return -1;
    return res_decode_lzw_mem(a->image, a->image_size,
                              a->entries[idx], out, out_size, chkOut);
}

/* --- scene playback ----------------------------------------------------- */

/* The scene chain table, ported from 202F:0x6293 (exe bytes): chain[scene]
 * = the next scene played after it; self = replay until a key; 4 = the
 * tail loop (scenes 4 and 8 ping-pong until a key). */
static const uint8_t sceneChainTable[10] = { 0, 1, 2, 3, 8, 5, 6, 7, 4, 9 };

/* Background-object table, ported from 202F:0x629e: bgTable[movie] = the
 * 0xE00-byte apl\scoops.apl block drawn as the cutscene background. */
static const uint8_t sceneBgTable[10] = { 1, 2, 3, 4, 5, 6, 8, 0, 5, 7 };

typedef enum {
    SCENE_IDLE,        /* not started                                */
    SCENE_DWELL,       /* the 210-tick pre-scene dwell               */
    SCENE_PLAY,        /* decode + draw the next object (instant)    */
    SCENE_WAIT,        /* per-object 6-tick wait                     */
    SCENE_CHAIN,       /* decide the next scene (instant)            */
    SCENE_CHAIN_WAIT,  /* wait up to 210 ticks for a key (replay)    */
    SCENE_RESTART,     /* chain advance: reload ANI (instant)        */
    SCENE_DONE
} ScenePhase;

static const OSystem *scene_osys;
static ScenePhase scene_phase = SCENE_IDLE;
static unsigned scene_level = 0xFFFFu;   /* the level the intro is for */

static uint8_t  *aniData;
static Ani       ani;
static uint8_t  *scriptBuf;         /* decoded script object                */
static unsigned   plays;
static const uint16_t *indices;     /* into scriptBuf + 2 + 768             */
static uint8_t   *objBuf;           /* 64K segment model                    */
static uint8_t   *scoopsBuf;
static uint32_t   scoopsSize;

static uint8_t   curScene;
static uint16_t  dwellTicks;
static uint16_t  objWait;
static uint16_t  chainWait;
static unsigned  objIndex;

int scene_init(const OSystem *osys)
{
    scene_osys = osys;
    return 0;
}

/* Any key or fire: the original's [0x48A3] / +0x5ACE bit-7 checks. */
static int scene_keyHeld(void)
{
    InputState in;
    scene_osys->eventsPoll(&in);
    return in.lastKey != 0 || (in.joyBits & 0x80u) != 0;
}

static void scene_unload(void)
{
    free(aniData); aniData = NULL;
    free(scriptBuf); scriptBuf = NULL; indices = NULL; plays = 0;
    free(objBuf); objBuf = NULL;
    free(scoopsBuf); scoopsBuf = NULL; scoopsSize = 0;
    memset(&ani, 0, sizeof ani);
}

/* Load ANI<index>.ANI + the background assets, draw the background and
 * the palette onto page 0.  Returns 0 on success. */
static int scene_start(unsigned index)
{
    scene_unload();
    curScene = (uint8_t)index;
    res_setDisc(res_movieDisc(index));

    char name[32];
    snprintf(name, sizeof name, ANI_NAME, index);
    if (ani_load(&ani, index)) return -1;

    scriptBuf = malloc(OBJ_SEG_SIZE);
    objBuf = malloc(OBJ_SEG_SIZE);
    if (!scriptBuf || !objBuf) { scene_unload(); return -1; }

    /* lbm\daily.bit -> the page: [64000-byte screen][768-byte palette] */
    {
        uint32_t dailySize = 0;
        uint8_t *daily = res_loadChunk("LBM\\DAILY.BIT", &dailySize, NULL);
        uint8_t *page = video_page(0);
        if (!daily) { scene_unload(); return -1; }
        memcpy(page, daily, dailySize < PAGE_SIZE ? dailySize : PAGE_SIZE);
        if (dailySize > PAGE_SIZE)
            scene_osys->videoSetPalette(daily + PAGE_SIZE, 0, 256);
        free(daily);
    }

    /* apl\scoops.apl -> the background block, overlaid onto the page */
    scoopsBuf = res_loadChunk("APL\\SCOOPS.APL", &scoopsSize, NULL);
    if (scoopsBuf) {
        uint8_t *page = video_page(0);
        uint32_t srcOff = 0x400u + (uint32_t)sceneBgTable[index] * 0xE00u;
        if (srcOff + 0xE00u <= scoopsSize) {
            const uint8_t *src = scoopsBuf + srcOff;
            for (unsigned row = 0; row < 0x38u; row++) {       /* 56 rows   */
                memcpy(page + 0x8270u + row * 0x100u, src, 0x40u);  /* 64 B */
                src += 0x40u;
            }
        }
        free(scoopsBuf);
        scoopsBuf = NULL;
    }

    scene_osys->videoPresent(0);
    return 0;
}

int scene_playLevelIntro(unsigned introIndex)
{
    if (introIndex == 0xFFu) return 0;        /* no cutscene for this level */

    if (introIndex != scene_level) {          /* (re)start on level change   */
        scene_level = introIndex;
        if (scene_start(introIndex)) { scene_phase = SCENE_DONE; return 0; }

        /* the script object (entry 0): [u16 plays-1][768 palette][indices] */
        uint16_t chk = 0;
        int script_size = ani_decode_object(&ani, 0, scriptBuf, OBJ_SEG_SIZE, &chk);
        if (script_size < 0 || chk != 0) { scene_unload(); scene_phase = SCENE_DONE; return 0; }
        plays = (unsigned)rd_le16(scriptBuf) + 1u;
        if (2u + 768u + plays * 2u > (uint32_t)script_size) {
            scene_unload();
            scene_phase = SCENE_DONE;
            return 0;
        }
        indices = (const uint16_t *)(scriptBuf + 2 + 768);
        objIndex = 0;
        scene_phase = SCENE_DWELL;
        dwellTicks = 210u;
    }

    if (scene_phase == SCENE_DONE) return 0;

    int keyHeld = scene_keyHeld();

    for (;;) {   /* instant phases resolve within one tick */
        switch (scene_phase) {
        case SCENE_DWELL:
            if (keyHeld) dwellTicks = 0;      /* any key skips the remainder */
            if (dwellTicks > 0) { --dwellTicks; return 1; }
            if (keyHeld) { scene_phase = SCENE_DONE; return 0; }
            scene_phase = SCENE_PLAY;
            objIndex = 0;
            break;                             /* play the first object now */

        case SCENE_PLAY:
            if (objIndex >= plays) { scene_phase = SCENE_CHAIN; break; }
            {
                uint16_t chk = 0;
                int sz = ani_decode_object(&ani, indices[objIndex], objBuf, OBJ_SEG_SIZE, &chk);
                if (sz < 0 || chk != 0) {      /* place_scene_obj: JZ / abort */
                    scene_phase = SCENE_CHAIN;
                    break;
                }
                scene_al_expand(objBuf, (uint32_t)sz, video_page(0), PAGE_SIZE);
                scene_osys->videoPresent(0);
                objWait = 6u;                  /* wait_flag_timeout, [0x627c] clear */
                objIndex++;
                scene_phase = SCENE_WAIT;
                return 1;
            }

        case SCENE_WAIT:
            if (keyHeld) { scene_phase = SCENE_DONE; return 0; }
            if (--objWait == 0) scene_phase = SCENE_PLAY;
            return 1;

        case SCENE_CHAIN: {
            uint8_t nxt = sceneChainTable[curScene];
            if (keyHeld) { scene_phase = SCENE_DONE; return 0; }
            if (nxt == curScene || nxt == 4u) {  /* replay / tail loop */
                scene_phase = SCENE_CHAIN_WAIT;
                chainWait = 210u;
                return 1;
            }
            curScene = nxt;                      /* chain advance */
            scene_phase = SCENE_RESTART;
            break;
        }

        case SCENE_CHAIN_WAIT:
            if (keyHeld) { scene_phase = SCENE_DONE; return 0; }
            if (--chainWait == 0) scene_phase = SCENE_PLAY;   /* replay it */
            return 1;

        case SCENE_RESTART:                       /* chain advance: no dwell */
            if (scene_start(curScene)) { scene_phase = SCENE_DONE; return 0; }
            scene_phase = SCENE_PLAY;
            objIndex = 0;
            break;

        case SCENE_DONE:
        case SCENE_IDLE:
            return 0;
        }
    }
}

/* --- the boot intro: load_intro_scene (1000:B63B) ----------------------
 *
 * Plays INTRO\INTRO.RSC's display list over a horizontally scrolling
 * INTRO\BAC.BIT background, with the treetop/treebot/apple sprites riding
 * the scroll.  Per tick:
 *   1. script_fire(frame) — the cues (the verbatim 202F:0x843C list)
 *   2. the scroll render (copy_rows_0x140, 1000:B9F8): the bac screen
 *      wrapped horizontally by the scroll position, 184 rows in 92 pairs
 *   3. the sprites (blit_intro_trees/apple, 1000:BA6E/BA4B via
 *      blit_sprite_transparent 1000:BAA5): their screen X = world X -
 *      the scroll, so they ride the background
 *   4. the display-list group (blit_display_list, 1000:B955): transparent
 *      overlays at the records' destinations; at the group terminator the
 *      dwell counts down and the next group + frame follow
 *   5. present, then the 2-tick frame wait
 * The quit cue (frame 385) ends it; the frame counter ran 0..386 live.
 * No key skip: the loop ends on the quit cue alone.
 *
 * Shortcut: the unidentified helpers around the load (b7df after
 * words.bit, bdbc at b685) and the fade calls (c0af/c0b8/c0c6) are not
 * ported — the words.bit display role and the fade sequencing land with
 * the palette/video work. */

typedef enum {
    BOOT_START,
    BOOT_DWELL,     /* the 280-tick initial dwell                    */
    BOOT_FRAME,     /* cues + scroll + sprites + display-list blit   */
    BOOT_WAIT,      /* the 2-tick frame wait                         */
    BOOT_DONE
} BootPhase;

static BootPhase boot_phase = BOOT_START;
static CueScript boot_script;
static uint8_t  *bootBac;         /* intro\bac.bit — scroll source + palette */
static uint8_t  *bootTreetop;     /* intro\treetop.bit 140x113               */
static uint8_t  *bootTreebot;     /* intro\treebot.bit 21x59                 */
static uint8_t  *bootApple;       /* intro\apple.spr 16x16                   */
static uint8_t  *bootRsc;         /* intro\intro.rsc — the display list      */
static uint32_t  bootRscSize;
static uint32_t *bootEntries;
static unsigned  bootEntryCount;
static uint8_t  *bootListBuf;     /* the decoded display-list object         */
static uint32_t  bootListSize;    /* its decoded size                        */
static uint8_t  *bootObj;         /* the records' decoded overlay objects    */
static uint32_t  bootListStart;   /* 0x6FEA: the records' start              */
static uint32_t  bootListPos;     /* [0x6468]: the current group             */
static unsigned  bootFrame;       /* [0x6466]: the display-list group index  */
static unsigned  bootGroupDwell;  /* [0x6460]: frames per group              */
static unsigned  bootDwellCount;  /* [0x6462]: the group dwell countdown     */
static unsigned  bootScrollPos;   /* [0x6473]: the horizontal scroll         */
static uint16_t  bootWaitTicks;

static void boot_unload(void)
{
    free(bootBac); bootBac = NULL;
    free(bootTreetop); bootTreetop = NULL;
    free(bootTreebot); bootTreebot = NULL;
    free(bootApple); bootApple = NULL;
    free(bootRsc); bootRsc = NULL; bootRscSize = 0;
    free(bootEntries); bootEntries = NULL; bootEntryCount = 0;
    free(bootListBuf); bootListBuf = NULL;
    free(bootObj); bootObj = NULL;
}

/* blit_sprite_transparent (1000:BAA5): a width x height sprite at page
 * (worldX - scroll, pageYOff); 0 bytes are transparent; the sprite clips
 * at the page's right edge and is off-screen once X wraps. */
static void boot_blit_sprite(const uint8_t *spr, unsigned width, unsigned height,
                             unsigned worldX, unsigned pageYOff, uint8_t *page)
{
    uint32_t x = ((uint32_t)worldX - bootScrollPos) & 0xFFFFu;
    if (x >= 320u) return;                        /* CMP AX,0x140 / JC */
    uint32_t visible = 320u - x;
    if (visible > width) visible = width;
    uint8_t *dst = page + pageYOff + x;
    const uint8_t *src = spr;
    for (unsigned row = 0; row < height; row++) {
        for (unsigned i = 0; i < visible; i++) {
            uint8_t b = src[i];
            if (b) dst[i] = b;                    /* 0 = transparent */
        }
        dst += 0x140u;                            /* the 320-byte row stride */
        src += width;                             /* the clipped part skipped */
        if (dst + visible > page + PAGE_SIZE) return;
    }
}

/* copy_rows_0x140 (1000:B9F8): the scroll render — the bac screen wrapped
 * horizontally by `scroll`: each of the 184 rows shows
 * bac[row][scroll..320) then bac[row][0..scroll), in 92 two-row pairs
 * (each pair: two DX-byte copies + two BP advances; DX + BP = 320). */
static void boot_scroll_render(uint8_t *page, const uint8_t *bac,
                               uint32_t bacSize, unsigned scroll, unsigned destOff)
{
    unsigned ax = scroll % 0x140u;
    unsigned dx, bp, si, di = destOff;
    if (di != 0) { dx = 0x140u - di; bp = di; si = 0; }
    else         { dx = 0x140u - ax; bp = ax;      si = ax; }
    for (unsigned i = 0; i < 0x5cu; i++) {        /* 92 two-row pairs */
        for (unsigned j = 0; j < dx; j++) {
            if (si >= bacSize || di >= PAGE_SIZE) return;
            page[di++] = bac[si++];
        }
        di += bp; si += bp;
        for (unsigned j = 0; j < dx; j++) {
            if (si >= bacSize || di >= PAGE_SIZE) return;
            page[di++] = bac[si++];
        }
        di += bp; si += bp;
    }
}

/* One display-list record: the stored index INCs (16-bit — a 0xFFFF index
 * wraps to object 0, the display-list object itself), the object LZW-
 * decodes to [u16 width][u16 height][bytes] and transparent-blits at
 * `dest` with the 320-byte row stride (1000:B955's inner blit). */
static void boot_blit_record(const uint8_t *rsc, uint32_t rscSize,
                             const uint32_t *entries, unsigned entryCount,
                             uint8_t *objBuf, unsigned storedIdx, uint16_t dest,
                             const uint8_t *listObj, uint32_t listObjSize,
                             uint8_t *page)
{
    unsigned idx = (storedIdx + 1u) & 0xFFFFu;    /* INC AX: 16-bit wrap */
    const uint8_t *src;
    uint32_t srcSize;
    uint16_t chk = 0;
    int sz;
    if (idx == 0) {                               /* object 0: the list itself */
        src = listObj;
        srcSize = listObjSize;
    } else {
        if (idx >= entryCount) return;
        sz = res_decode_lzw_mem(rsc, rscSize, entries[idx],
                                objBuf, OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) return;           /* 1000:B444: skip */
        src = objBuf;
        srcSize = (uint32_t)sz;
    }
    unsigned width = rd_le16(src);
    unsigned height = rd_le16(src + 2);
    const uint8_t *body = src + 4;
    uint32_t bodySize = srcSize - 4u;
    uint32_t di = dest;
    for (unsigned r = 0; r < height; r++) {
        for (unsigned i = 0; i < width && di + i < PAGE_SIZE; i++) {
            uint32_t so = (uint32_t)r * width + i;
            uint8_t b = so < bodySize ? body[so] : 0u;   /* past the object: 0 */
            if (b) page[di + i] = b;              /* 0 = transparent */
        }
        di += 0x140u;
        if (di >= PAGE_SIZE) return;
    }
}

/* blit_display_list (1000:B955): blit the current group's records; returns
 * the position after the group's 0xFFFF terminator. */
static uint32_t boot_blit_group(const uint8_t *rsc, uint32_t rscSize,
                                const uint32_t *entries, unsigned entryCount,
                                uint8_t *objBuf, uint32_t listPos, uint8_t *page)
{
    uint32_t si = listPos;
    for (;;) {                                     /* the group's records */
        if (si + 4 > rscSize) break;
        uint16_t dest = rd_le16(rsc + si);
        uint16_t idx  = rd_le16(rsc + si + 2);
        si += 4;
        boot_blit_record(rsc, rscSize, entries, entryCount,
                         objBuf, idx, dest, bootListBuf, bootListSize, page);
        if (si + 2 > rscSize) break;
        if (rd_le16(rsc + si) == 0xFFFFu) { si += 2; break; }   /* the group end */
    }
    return si;
}

/* The seek (cmd 4): walk `frame` 0xFFFF group terminators from the list
 * start — the group for `frame`.  Returns the group's offset. */
static uint32_t boot_seek(const uint8_t *rsc, uint32_t rscSize,
                          uint32_t listStart, unsigned frame)
{
    uint32_t si = listStart;
    for (unsigned g = 0; g < frame && si + 2 <= rscSize; ) {
        if (rd_le16(rsc + si) == 0xFFFFu) { si += 2; g++; }
        else si += 4;                              /* a [dest][idx] record */
    }
    return si;
}

int scene_playBootIntro(void)
{
    if (boot_phase == BOOT_DONE) return 0;

    switch (boot_phase) {
    case BOOT_START:
        boot_unload();
        res_setDisc(0u);                   /* the boot assets ship on disc 0 */
        bootBac = res_loadChunk("INTRO\\BAC.BIT", NULL, NULL);
        bootTreetop = res_loadChunk("INTRO\\TREETOP.BIT", NULL, NULL);
        bootTreebot = res_loadChunk("INTRO\\TREEBOT.BIT", NULL, NULL);
        bootApple = res_loadChunk("INTRO\\APPLE.SPR", NULL, NULL);
        bootRsc = res_loadChunk("INTRO\\INTRO.RSC", &bootRscSize, NULL);
        if (!bootBac || !bootTreetop || !bootTreebot || !bootApple || !bootRsc) {
            fprintf(stderr, "DBG boot_start: load failed (bac=%p tt=%p tb=%p ap=%p rsc=%p)\n",
                    (void*)bootBac, (void*)bootTreetop, (void*)bootTreebot,
                    (void*)bootApple, (void*)bootRsc);
            boot_unload();
            boot_phase = BOOT_DONE;
            return 0;
        }
        bootListBuf = malloc(OBJ_SEG_SIZE);
        bootObj = malloc(OBJ_SEG_SIZE);
        if (!bootListBuf || !bootObj) { boot_unload(); boot_phase = BOOT_DONE; return 0; }

        bootEntryCount = bootRscSize / 4u;
        bootEntries = malloc(bootEntryCount * sizeof(uint32_t));
        if (!bootEntries) { boot_unload(); boot_phase = BOOT_DONE; return 0; }
        for (unsigned i = 0; i < bootEntryCount; i++)
            bootEntries[i] = rd_le32(bootRsc + i * 4u);

        /* bac.bit = [58880-byte screen][768-byte palette]: its palette
         * drives the whole intro (palette_load_6bit at b6bc, SI=0xE600) */
        scene_osys->videoSetPalette(bootBac + 0xE600u, 0, 256);
        boot_phase = BOOT_DWELL;
        dwellTicks = 280u;                 /* 0x118: the initial dwell */
        return 1;

    case BOOT_DWELL:
        if (--dwellTicks > 0) return 1;    /* no key skip in the original */
        /* decode the display-list object (entry 0): [u16 ?][records...] —
         * the records start at object+2 ([0x6468] init 0x6FEA = 0x6FE8+2) */
        {
            uint16_t chk = 0;
            int sz = res_decode_lzw_mem(bootRsc, bootRscSize, bootEntries[0],
                                        bootListBuf, OBJ_SEG_SIZE, &chk);
            if (sz < 0 || chk != 0) { boot_unload(); boot_phase = BOOT_DONE; return 0; }
            bootListSize = (uint32_t)sz;
            bootListStart = 2u;
        }
        bootListPos = bootListStart;
        bootFrame = 0u;
        bootGroupDwell = 1u;
        bootDwellCount = 1u;
        bootScrollPos = 0u;
        script_introStart(&boot_script);
        boot_phase = BOOT_FRAME;
        return 1;

    case BOOT_FRAME: {
        script_fire(&boot_script, bootFrame);
        if (script_quit(&boot_script) || script_done(&boot_script)) {
            boot_unload();
            boot_phase = BOOT_DONE;
            return 0;
        }
        if (script_seekPending(&boot_script)) {
            bootListPos = boot_seek(bootRsc, bootRscSize, bootListStart,
                                    script_seekFrame(&boot_script));
            bootFrame = script_seekFrame(&boot_script);
            script_seekDone(&boot_script);
        }
        /* update_scene (1000:B9B5): the scroll render + the sprites; the
         * scroll position advances after the render (when scroll is on) */
        {
            uint8_t *page = video_page(0);
            boot_scroll_render(page, bootBac, 0xE600u, bootScrollPos, 0u);
            boot_scroll_render(page, bootBac, 0xE600u, bootScrollPos,
                               0x140u - (bootScrollPos % 0x140u));
            boot_blit_sprite(bootTreetop, 140u, 113u, 0x5A2u, 0x140u, page);
            boot_blit_sprite(bootTreebot, 21u, 59u, 0x608u, 0x8E80u, page);
            if (bootFrame < 331u)          /* the apple leaves at frame 331 */
                boot_blit_sprite(bootApple, 16u, 16u, 0x5B5u, 0x3700u, page);
            if (script_scrollOn(&boot_script))
                bootScrollPos += script_scrollDelta(&boot_script);
        }
        /* blit_display_list: the group's overlays; the dwell counts down
         * per frame — when it expires, the next group + frame */
        {
            uint8_t *page = video_page(0);
            uint32_t next = boot_blit_group(bootRsc, bootRscSize, bootEntries,
                                            bootEntryCount, bootObj, bootListPos,
                                            page);
            if (--bootDwellCount == 0) {
                bootDwellCount = bootGroupDwell;
                bootListPos = next;
                bootFrame++;
            }
        }
        scene_osys->videoPresent(0);
        bootWaitTicks = 2u;                /* the 2-tick frame wait */
        boot_phase = BOOT_WAIT;
        return 1;
    }

    case BOOT_WAIT:
        if (--bootWaitTicks == 0) boot_phase = BOOT_FRAME;
        return 1;

    case BOOT_DONE:
        return 0;
    }
    return 0;
}
