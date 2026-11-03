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
 * playing, 0 when it is finished (or introIndex == 0xFF: no cutscene, or
 * the ANIMATIONS board is off — [0x5E79] non-zero RETs at cd_file_op's
 * head).  Any key/fire skips or quits, per the original's three [0x48A3]
 * checks.
 *
 * The AL expander (blit_background_data, 1000:B2EB) and the LZW machine
 * (decode_bitstream, 1000:91B9, via res.c) are real and byte-verified
 * against the validated python decoder (tools/ani_lzw.py).
 */
#include "game.h"
#include "byteorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANI_NAME "ANM\\ANI%02u.ANI"
#define PAGE_SIZE (320u * 200u)
#define OBJ_SEG_SIZE 65536u   /* the original decodes objects into a 64K segment */

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
            uint16_t cw = rd_le_u16(src + si);
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

    a->table_size = rd_le_u32(a->image);
    a->n_entries = a->table_size / 4;
    a->entries = malloc(a->n_entries * sizeof(uint32_t));
    if (!a->entries) {
        free(a->image);
        a->image = NULL;
        return -1;
    }
    for (unsigned i = 0; i < a->n_entries; i++)
        a->entries[i] = rd_le_u32(a->image + i * 4u);
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

/* 202F:0x6304 — the panel's "scoop" headlines, one per scene index (the
 * 10th is empty).  Drawn centred at 0xA0, row 0x41, colour 0x10. */
static const char *const sceneHeadline[10] = {
    "HUMANS INVENT WEAPON! THE SPEAR!",
    "HUMANS RESCUE BABY DINOSAUR!",
    "REVOLUTION IN TRANSPORT! THE WHEEL!",
    "BRAVE HUMAN RESCUES THE QUEEN!",
    "SPECIAL REPORT ON NEW DISCOVERY! FIRE!",
    "UNIDENTIFIED FLYING HUMAN SPOTTED!",
    "HUMANS FAIL IN RACE FOR EVOLUTION!",
    "HUMANS WIN THE RACE FOR EVOLUTION!",
    "HUMANS RECOVER THE STOLEN IDOL!",
    ""
};

typedef enum {
    SCENE_IDLE,        /* not started                                */
    SCENE_PANEL_FADE,  /* the panel's first 0x20 entries fade in     */
    SCENE_DWELL,       /* the 210-tick pre-scene dwell               */
    SCENE_PANEL_OUT,   /* the 0x20-entry fade to black before frames */
    SCENE_PALFADE,     /* the fade to the script object's palette    */
    SCENE_QUIT_FADE,   /* key-skip: the 0x100-entry fade to black    */
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
static uint8_t   *textSpr;        /* apl\text.spr — the panel's font */

static uint8_t   curScene;
static int       chainReplay;       /* 210-tick wait ends in a self-replay */
static uint16_t  dwellTicks;
static uint8_t   panelPal[768];     /* daily.bit's palette (masked 6-bit) */
static int       panelPalValid;
/* c537/c563's target: [0x48BC] is the 256x3 scratch ramp, and every writer
 * restores it to zero — clear_fade_ramp (1000:B90F) at boot, the level's
 * discovery op (1000:3A6A) on every exit (1000:3CF9/3D01), run_screen_
 * transition (1000:B82E), and menu_loop's 0xC0 only (1000:051C) — so the
 * panel's pre-scene fade and the key exit are fades to black. */
static const uint8_t sceneBlackPal[768];
/* [0x627C]: boot_intro_sequence's fast mode — its only writer (set at
 * 1000:B59F, cleared at 1000:B623, entry's caller only).  play_scene_sequence
 * reads it twice: the pre-frame palette fade is 4 entries div 2 instead of
 * 0x100 div 0x19 (1000:B263), and wait_flag_timeout waits 1 tick instead of
 * 6 per object (1000:B2D3).  The level's .ANI always runs with it clear. */
static int sceneFast;

void scene_setFastMode(int on) { sceneFast = on ? 1u : 0u; }

/* Start a NEW cutscene on the next scene_playLevelIntro call: cd_file_op has
 * no cache of its own, so two consecutive levels with the same intro index
 * must reload it.  The per-tick player otherwise keeps the finished index
 * (a finished intro stays finished). */
void scene_resetIntro(void)
{
    scene_level = 0xFFFFu;
    scene_phase = SCENE_IDLE;
}

/* play_scene_sequence's pre-frame palette fade (1000:B263), shared by the
 * level's .ANI and the boot's logo: fast mode = 4 entries div 2, otherwise
 * all 256 entries div 0x19. */
static void scene_paletteFade(const uint8_t *target)
{
    if (sceneFast) palette_buildFade(0u, 4u, 2u, target);
    else           palette_buildFade(0u, 0x100u, 0x19u, target);
}

/* cd_file_op's key exit (1000:B157): a 0x100-entry fade to [0x48BC]
 * (black) before the function returns. */
static void scene_quitFade(void)
{
    palette_buildFade(0u, 0x100u, 0x19u, sceneBlackPal);
    scene_phase = SCENE_QUIT_FADE;
}
static uint16_t  objWait;
static uint16_t  chainWait;
static unsigned  objIndex;

int scene_init(const OSystem *osys)
{
    scene_osys = osys;
    return 0;
}

/* The boot's flag-style key tests (b2ba and c0c6) read the original's
 * level-triggered ISR flags ([0x48A3] / +0x5ACE bit-7) — not an event
 * queue.  They read the main loop's per-tick poll (scene_noteInput):
 * extra eventsPoll calls here would consume input events the foreground
 * owns (the null backend's script is call-indexed). */
static InputState scene_lastInput;

static int boot_keyState(void)
{
    return scene_lastInput.lastKey != 0u ||
           (scene_lastInput.joyBits & 0x80u) != 0u;
}

void scene_noteInput(const InputState *in) { scene_lastInput = *in; }

/* Any key or fire: the original's [0x48A3] / +0x5ACE bit-7 checks.  (The
 * level intro's skip polls — its contract predates the boot's flag-cache
 * model; the boot's waits use boot_keyState instead.) */
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
    if (textSpr) font_publish(NULL);        /* the glyph table dies with it */
    free(textSpr); textSpr = NULL;
    memset(&ani, 0, sizeof ani);
}

/* Load ANI<index>.ANI + the background assets, draw the background and
 * the palette onto page 0.  Returns 0 on success. */
static int scene_start(unsigned index, int initial)
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
        /* 1000:B0AF (palette_load_6bit, 1000:8D24): daily.bit's 768 bytes
         * are masked to 6 bits and staged at 0x4BBC, NOT installed — the
         * panel's first 0x20 entries FADE in (1000:B0DA) */
        panelPalValid = dailySize >= PAGE_SIZE + 768u;
        if (panelPalValid)
            for (unsigned i = 0; i < 768u; i++)      /* 1000:8D24's AND 0x3F */
                panelPal[i] = daily[PAGE_SIZE + i] & 0x3Fu;
        free(daily);
    }

    /* apl\text.spr (the original's second load, 1000:C491): the panel's
     * headline is drawn with these glyphs (blit_text_tile), so the scene
     * publishes them for its own frame */
    free(textSpr);
    textSpr = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(textSpr);

    /* apl\scoops.apl -> the background block, overlaid onto the page */
    scoopsBuf = res_loadChunk("APL\\SCOOPS.APL", &scoopsSize, NULL);
    if (scoopsBuf) {
        uint8_t *page = video_page(0);
        uint32_t srcOff = 0x400u + (uint32_t)sceneBgTable[index] * 0xE00u;
        if (srcOff + 0xE00u <= scoopsSize) {
            /* 1000:B1CF: 56 rows of 64 bytes at 0x8270 (= row 104, x 112 of
             * the 320-wide panel).  The original's row step is the REP
             * MOVSB advance (0x40) PLUS its `ADD DI,0x100`, i.e. 0x140 — the
             * panel's 320-byte row; taking only the 0x100 shears the
             * thumbnail 64 px left per row. */
            const uint8_t *src = scoopsBuf + srcOff;
            for (unsigned row = 0; row < 0x38u; row++) {
                memcpy(page + 0x8270u + row * 0x140u, src, 0x40u);
                src += 0x40u;
            }
        }
        free(scoopsBuf);
        scoopsBuf = NULL;
    }

    /* the panel's headline (1000:B0D4: blit_text_tile at centre 0xA0, row
     * 0x41, mask 0x10 — OR'd into the panel art, so it takes the panel's own
     * vertical ramp) — the index-th string of 202F:0x6304 */
    if (sceneHeadline[index][0])
        textpage_drawLineMask(video_page(0), sceneHeadline[index], 0x41u, 0x10u);

    scene_osys->videoPresent(0);

    /* 1000:B0DA: after the panel is drawn and flipped, its first 0x20 DAC
     * entries fade from the previous screen's palette to daily.bit's
     * (div 0x19); the 210-tick dwell starts only after that fade.  A chain
     * advance (C57A) skips the panel and its fade. */
    if (initial && panelPalValid)
        palette_buildFade(0u, 0x20u, 0x19u, panelPal);
    return 0;
}

int scene_playLevelIntro(unsigned introIndex)
{
    /* cd_file_op's head (1000:AFFD): the ANIMATIONS board (cell 5,
     * [0x5E79]) non-zero RETs before any file op — cutscenes are off. */
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u) return 0;
    if (introIndex == 0xFFu) return 0;        /* no cutscene for this level */

    if (introIndex != scene_level) {          /* (re)start on level change   */
        scene_level = introIndex;
        if (scene_start(introIndex, 1)) { scene_phase = SCENE_DONE; return 0; }

        /* the script object (entry 0): [u16 plays-1][768 palette][indices] */
        uint16_t chk = 0;
        int script_size = ani_decode_object(&ani, 0, scriptBuf, OBJ_SEG_SIZE, &chk);
        if (script_size < 0 || chk != 0) { scene_unload(); scene_phase = SCENE_DONE; return 0; }
        plays = (unsigned)rd_le_u16(scriptBuf) + 1u;
        if (2u + 768u + plays * 2u > (uint32_t)script_size) {
            scene_unload();
            scene_phase = SCENE_DONE;
            return 0;
        }
        indices = (const uint16_t *)(scriptBuf + 2 + 768);
        /* the script object (entry 0) carries the cutscene's 768-byte
         * 6-bit palette at +2, but it belongs to the FRAMES: the panel is
         * shown in daily.bit's palette for the whole dwell and only then
         * does the DAC fade to the script palette (1000:B263, below). */
        objIndex = 0;
        if (panelPalValid) {
            scene_phase = SCENE_PANEL_FADE;
        } else {
            scene_phase = SCENE_DWELL;
            dwellTicks = 210u;
        }
    }

    if (scene_phase == SCENE_DONE) return 0;

    int keyHeld = scene_keyHeld();

    for (;;) {   /* instant phases resolve within one tick */
        switch (scene_phase) {
        case SCENE_PANEL_FADE:
            if (palette_fadeActive()) return 1;
            /* 1000:B0EC: the 210-tick count starts at 0 AFTER the fade
             * returns, so this tick is not one of the 210 */
            dwellTicks = 210u;
            scene_phase = SCENE_DWELL;
            return 1;

        case SCENE_DWELL:
            if (keyHeld) dwellTicks = 0;      /* any key skips the remainder */
            if (dwellTicks > 0) { --dwellTicks; return 1; }
            if (keyHeld) { scene_quitFade(); return 1; }
            /* 1000:B13F: the panel's first 0x20 entries fade back to
             * [0x48BC] (black) over div 0x19; play_scene_sequence's own
             * head (B248) waits for that fade before the prologue frame. */
            palette_buildFade(0u, 0x20u, 0x19u, sceneBlackPal);
            scene_phase = SCENE_PANEL_OUT;
            return 1;

        case SCENE_QUIT_FADE:
            if (palette_fadeActive()) return 1;
            scene_phase = SCENE_DONE;
            return 0;

        case SCENE_PANEL_OUT:
            if (palette_fadeActive()) return 1;
            /* 1000:B24E..B260: the FIRST frame is decoded, drawn and flipped
             * BEFORE the palette fade, so the fade happens over the frame and
             * not over the panel (the loop then replays index 0).  A decode
             * failure unwinds the whole player (B22C: JMP B2C4). */
            {
                uint16_t chk = 0;
                int sz = ani_decode_object(&ani, indices[0], objBuf,
                                           OBJ_SEG_SIZE, &chk);
                if (sz < 0 || chk != 0) { scene_phase = SCENE_DONE; return 0; }
                scene_al_expand(objBuf, (uint32_t)sz, video_page(0), PAGE_SIZE);
                scene_osys->videoPresent(0);
            }
            /* 1000:B263: now the DAC fades to the script object's palette at
             * 0x6FEA (= scriptBuf+2): the whole 256 entries div 0x19 (or 4
             * entries div 2 when [0x627C] is set — the instant path), then
             * wait_palette_fade_done. */
            scene_paletteFade(scriptBuf + 2);
            scene_phase = SCENE_PALFADE;
            return 1;

        case SCENE_PALFADE:
            if (keyHeld) { scene_quitFade(); return 1; }
            if (palette_fadeActive()) return 1;
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
                objWait = sceneFast ? 1u : 6u; /* wait_flag_timeout 1000:B2D3 */
                objIndex++;
                scene_phase = SCENE_WAIT;
                return 1;
            }

        case SCENE_WAIT:
            if (keyHeld) { scene_quitFade(); return 1; }
            if (--objWait == 0) scene_phase = SCENE_PLAY;
            return 1;

        case SCENE_CHAIN: {
            uint8_t nxt = sceneChainTable[curScene];
            if (keyHeld) { scene_quitFade(); return 1; }
            /* 1000:B17E: `CMP BL,[BX+0x6293]` - a scene whose chain entry is
             * itself waits 210 ticks (1000:B1A6: also the key/fire poll) and
             * then replays from its first object (1000:B189 -> 0xB14D ->
             * 0xB211).  That is the ending's "loop until ESC" and the intros'
             * "runs until skipped".  A chain entry of 4 is the 8->4 tail
             * loop, which waits and then plays scene 4. */
            chainReplay = (nxt == curScene) ? 1 : 0;
            if (chainReplay) {
                scene_phase = SCENE_CHAIN_WAIT;
                chainWait = 210u;
                return 1;
            }
            curScene = nxt;                      /* chain advance */
            if (nxt == 4u) {                     /* 1000:B194: wait first */
                scene_phase = SCENE_CHAIN_WAIT;
                chainWait = 210u;
                return 1;
            }
            scene_phase = SCENE_RESTART;
            break;
        }

        case SCENE_CHAIN_WAIT:
            if (keyHeld) { scene_quitFade(); return 1; }
            if (--chainWait == 0) {
                if (chainReplay) {
                    objIndex = 0;                /* replay this scene */
                    scene_phase = SCENE_PLAY;
                } else {
                    scene_phase = SCENE_RESTART; /* play the chained scene */
                }
            }
            return 1;

        case SCENE_RESTART:                       /* chain advance: no dwell */
            if (scene_start(curScene, 0)) { scene_phase = SCENE_DONE; return 0; }
            scene_phase = SCENE_PLAY;
            objIndex = 0;
            break;

        case SCENE_DONE:
        case SCENE_IDLE:
            return 0;
        }
    }
}

/* --- the boot intro: boot_intro_sequence (1000:B590) --------------------
 *
 * The original's boot chain, four stages in this order:
 *   1. the Gametek logo (init_level_scene 1000:BEE9 + play_scene_sequence
 *      1000:B211): fade to black, then GAMETEK.RSC's display list — one
 *      group per tick, the DAC faded to the list's embedded palette in
 *      steps (entries 0-3 fast before the first group, then the 4-bank
 *      cycles and the 0x20-0x2F bank) — abortable by key, then the 0x1a4
 *      hold and the fade to the working palette
 *   2. the IDI card (present_scene2 1000:BD73): IDI.BIT through its own
 *      palette (fade BP=0x32), 0x1a4 hold, fade out
 *   3. the FIRST card (present_scene 1000:BD13): FIRST.BIT fades in bank
 *      by bank (7 x 16 entries at BP=0x23, 0x118 hold each), then fades out
 *   4. the ITB card + the scene (load_intro_scene 1000:B63B), below
 * The music init (8437/83f7/84a5, mus\title.ALB) sits between 1's fade and
 * the logo — deferred in the port.
 *
 * Stage 4 plays INTRO\INTRO.RSC's display list over a horizontally
 * scrolling INTRO\BAC.BIT background, with the treetop/treebot/apple
 * sprites riding the scroll.  Per tick:
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
 * The present (deplanarise_bitmap, 1000:BB05) shows the pool's rows 4-183
 * in the visible rows 0-179 — the port composes both stages into its flat
 * page.  The bar (the transition handler's words expansion, visible rows
 * 182-197, colours 0xE0-0xEF = bac's grey gradient) persists below every
 * present.
 *
 * The palette fades are ported through palette.c's build/step machine (the
 * ISR body's palette_fade_step 1000:8bfd): the card's own-palette fade-in
 * (b68f build + c0af wait), the post-dwell fade-out to the zeroed ramp and
 * the 0x8C-tick black hold (b6f2-b707), the transition's white flash with
 * the black 0xE0-0xEF bank + fade-back to the working palette
 * (run_screen_transition 1000:B82E) and the caption bank fade 0x28 ticks
 * after it (fade_countdown 1000:b7c0).  The ITB title card (present_frame
 * 1000:BDBC, shown through the initial 280-tick dwell) is ported. */

typedef enum {
    BOOT_START,
    BOOT_GT_PRE,        /* the boot fade-to-black wait; the list decode  */
    BOOT_GT_FADE0,      /* group 0 shown; the 4-entry fast fade + wait   */
    BOOT_GT_FRAME,      /* one group shown (b29b's LOOP)                 */
    BOOT_GT_FLIP,       /* the flip-apply tick (87c7's ISR sync)         */
    BOOT_GT_BANK_HOLD,  /* the 30-tick hold before each bank cycle       */
    BOOT_GT_BANK_A,     /* bank fade -> gtpal+0x48 (BP=3) + wait         */
    BOOT_GT_BANK_B,     /* bank fade -> gtpal+0x3C (BP=0x1e) + wait      */
    BOOT_GT_LAST_F,     /* entries 0x20-0x2F -> gtpal+0x60 (BP=0x32)     */
    BOOT_GT_HOLD,       /* the 0x1a4-tick hold                           */
    BOOT_GT_OUT,        /* fade all -> 0x4BBC (zeroed: nothing loaded yet
                         * — to black), BP=0x32 (bf60)                   */
    BOOT_ABORT,         /* c0c6 -> JMP b60a: a held key/fire unwinds the
                         * whole boot; the final fade (div 0x23) starts   */
    BOOT_ABORT_FADE,    /* its wait (8b8b — the non-aborting spin)       */
    BOOT_IDI_FADE,      /* idi.bit shown; fade to its own palette        */
    BOOT_IDI_HOLD,      /* the 0x1a4-tick hold                           */
    BOOT_IDI_OUT,       /* fade to the zeroed ramp + wait                */
    BOOT_FIRST_BANK,    /* bank fade (16 entries at BP=0x23) + wait      */
    BOOT_FIRST_HOLD,    /* the 0x118-tick hold per bank                  */
    BOOT_FIRST_LASTHOLD,/* bd50: the extra 0x118-tick hold after bank 7  */
    BOOT_FIRST_OUT,     /* fade to the zeroed ramp (BP=0x23) + wait      */
    BOOT_ITB_SHOW,      /* the ITB card present + its fade-in start      */
    BOOT_CARD_FADE,     /* its wait                                      */
    BOOT_DWELL,         /* the 280-tick initial dwell                    */
    BOOT_FADEOUT,       /* the post-dwell fade to the zeroed ramp (b6f2) */
    BOOT_BLACK,         /* the 0x8C-tick black hold (c0b8)               */
    BOOT_FRAME,         /* cues + scroll + sprites + display-list blit   */
    BOOT_WAIT,          /* the 2-tick frame wait                         */
    BOOT_SCENE_HOLD,    /* the scene's final 0x8C-tick hold (b7a3)       */
    BOOT_TRANS,         /* the transition's white-flash fade (b854)      */
    BOOT_TRANS_HOLD,    /* the words redraw + 0xa-tick hold (b866-b871)  */
    BOOT_OUTRO,         /* the scene-tail fade to black (b7a9, BP=0x1e)  */
    BOOT_DONE
} BootPhase;

static BootPhase boot_phase = BOOT_START;
static CueScript boot_script;
static uint8_t  *bootBac;         /* intro\bac.bit — scroll source + palette */
static uint8_t  *bootTreetop;     /* intro\treetop.bit 140x113               */
static uint8_t  *bootTreebot;     /* intro\treebot.bit 21x59                 */
static uint8_t  *bootApple;       /* intro\apple.spr 16x16                   */
static uint8_t  *bootWords;       /* intro\words.bit — the bar's 1-bit text  */
static uint8_t   boot_bar[16u * 320u];  /* the visible bar, rows 182-197     */
static int       boot_bar_arg = -1;     /* the transition the bar holds      */
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
static uint8_t   boot_ramp[768];  /* the fade-target scratch (202F:0x48BC)   */
static unsigned  bootTransTicks;  /* the 0xa-tick words hold (b869)          */
static unsigned  bootBankCountdown;  /* [0x644E]: the b7c0 bank-fade timer   */
static uint8_t  *bootGt;          /* intro\gametek.rsc — the logo display list */
static uint32_t  bootGtSize;
static uint32_t *bootGtEntries;   /* its u32 object table                    */
static unsigned  bootGtEntryCount;
static uint8_t  *bootGtList;      /* decoded object 0: [u16 count][768-byte  *
                                   * palette][u16 object index x count]      */
static unsigned  bootGtGroup;     /* the group loop counter (b29b's CX)      */
static unsigned  bootGtBank;      /* the 4-bank palette-cycle counter        */
static uint8_t  *bootItb;         /* intro\itb.bit held until its stage      */
static unsigned  bootHold;        /* the stages' hold counters (c0b8's CX)   */

/* One gametek group: place_scene_obj (1000:B3F9) decodes the object out of
 * the RSC's u32 table (the same machine as res_decode_lzw_mem — the b444
 * checksum gate) and the object's RLE stream expands over the whole page
 * (blit_background_data 1000:B2EB — scene_al_expand is that machine,
 * validated through the level-intro scenes). */
static int boot_gt_show(unsigned idx)
{
    if (idx >= bootGtEntryCount) return 0;
    uint16_t chk = 0;
    int sz = res_decode_lzw_mem(bootGt, bootGtSize, bootGtEntries[idx],
                                bootObj, OBJ_SEG_SIZE, &chk);
    if (sz < 0 || chk != 0) return 0;                    /* 1000:B444: skip */
    scene_al_expand(bootObj, (uint32_t)sz, video_page(0), 64000u);
    return 1;
}

/* present_frame (1000:BDBC) for the idi/first cards: load, the file's own
 * palette into the working palette (8d24, RAM only), the image on the page.
 * The DAC moves when the caller's fade targets the working palette. */
static int boot_card_show(const char *name)
{
    uint32_t size = 0;
    uint8_t *img = res_loadChunk(name, &size, NULL);
    if (!img || size < 64000u) {
        free(img);
        return 0;
    }
    memcpy(video_page(0), img, 64000u);
    palette_loadWorking(img + 64000u);
    free(img);
    return 1;
}

/* c0c6 (input_abort_check): every c0af/c0b8 wait polls the key each tick;
 * a held key or fire bit jumps b60a — the whole remaining boot (the cards,
 * the caveman scene) is skipped, the final fade runs, done. */
static int boot_abort_poll(void)
{
    if (!boot_keyState()) return 0;
    boot_phase = BOOT_ABORT;
    return 1;
}

static void boot_unload(void)
{
    free(bootBac); bootBac = NULL;
    free(bootTreetop); bootTreetop = NULL;
    free(bootTreebot); bootTreebot = NULL;
    free(bootApple); bootApple = NULL;
    free(bootWords); bootWords = NULL;
    free(bootRsc); bootRsc = NULL; bootRscSize = 0;
    free(bootEntries); bootEntries = NULL; bootEntryCount = 0;
    free(bootListBuf); bootListBuf = NULL;
    free(bootObj); bootObj = NULL;
    free(bootGt); bootGt = NULL; bootGtSize = 0;
    free(bootGtEntries); bootGtEntries = NULL; bootGtEntryCount = 0;
    free(bootGtList); bootGtList = NULL;
    free(bootItb); bootItb = NULL;
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
    unsigned width = rd_le_u16(src);
    unsigned height = rd_le_u16(src + 2);
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
 * the position after the group's 0xFFFF terminator.  The records are the
 * decoded object 0's OWN TAIL — §8.7a: nobody builds the list at runtime,
 * `MOV SI,[0x6468]` ([0x6468] = obj0 base+2) walks the scratch segment, so
 * the records read from the DECODED buffer and bounds against its size.
 * The objects themselves still LZW-decode out of the raw RSC's table. */
static uint32_t boot_blit_group(const uint8_t *listBuf, uint32_t listSize,
                                const uint8_t *rsc, uint32_t rscSize,
                                const uint32_t *entries, unsigned entryCount,
                                uint8_t *objBuf, uint32_t listPos, uint8_t *page)
{
    uint32_t si = listPos;
    for (;;) {                                     /* the group's records */
        if (si + 4 > listSize) break;
        uint16_t dest = rd_le_u16(listBuf + si);
        uint16_t idx  = rd_le_u16(listBuf + si + 2);
        si += 4;
        boot_blit_record(rsc, rscSize, entries, entryCount,
                         objBuf, idx, dest, bootListBuf, bootListSize, page);
        if (si + 2 > listSize) break;
        if (rd_le_u16(listBuf + si) == 0xFFFFu) { si += 2; break; }   /* the group end */
    }
    return si;
}

/* The seek (cmd 4): walk `frame` 0xFFFF group terminators from the list
 * start — the group for `frame`.  The terminators live in the decoded
 * object 0's tail (§8.7a: the list is consumed straight out of the decoded
 * object, [0x6468] runs in the scratch segment), so the walk bounds
 * against the DECODED size, not the raw RSC file.  Returns the group's
 * offset in the decoded buffer. */
static uint32_t boot_seek(const uint8_t *listBuf, uint32_t listSize,
                          uint32_t listStart, unsigned frame)
{
    uint32_t si = listStart;
    for (unsigned g = 0; g < frame && si + 2 <= listSize; ) {
        if (rd_le_u16(listBuf + si) == 0xFFFFu) { si += 2; g++; }
        else si += 4;                              /* a [dest][idx] record */
    }
    return si;
}

/* the bar text (run_screen_transition's builder, 1000:B89D -> B8A8): the
 * words' 1-bit block at [0x8689 + arg*0x258] — 16 rows × 40 bytes, each
 * bit = one pixel, MSB first, colour 0xE0+row (bac's grey gradient bank)
 * or black — written into the visible bar, rows 182-197, once per
 * transition; the per-frame present never touches those rows so the bar
 * persists until the next transition. */
static void boot_bar_draw(unsigned arg)
{
    for (unsigned r = 0; r < 16u; r++) {
        uint8_t *dst = boot_bar + r * 320u;
        const uint8_t *src = bootWords + arg * 600u + r * 40u;
        for (unsigned x = 0; x < 320u; x++) {
            uint8_t b = src[x / 8u];
            dst[x] = ((b >> (7u - (x % 8u))) & 1u) ? (uint8_t)(0xE0u + r) : 0u;
        }
    }
}

/* deplanarise_bitmap (1000:BB05) — the present.  The visible page (chain-4
 * mode 13h, pitch 80 bytes/row/plane) is filled from the pool's rows 4-183
 * (SI+0x500, 180 rows): visible row v = pool row v+4.  The visible rows
 * 180-199 are outside every present — the bar (run_screen_transition's
 * direct A000 writes, rows 182-197, colours 0xE0-0xEF = bac's grey
 * gradient) persists there until the next transition redraws it; rows
 * 180-181 and 198-199 stay black.  The port keeps one flat page, so the
 * compose folds both stages into it. */
static void boot_compose_present(uint8_t *page)
{
    memmove(page, page + 0x500u, 180u * 320u);   /* pool rows 4-183 */
    memset(page + 180u * 320u, 0, 2u * 320u);    /* rows 180-181    */
    memcpy(page + 182u * 320u, boot_bar, 16u * 320u);
    memset(page + 198u * 320u, 0, 2u * 320u);    /* rows 198-199    */
}

int scene_playBootIntro(void)
{
    if (boot_phase == BOOT_DONE) { sceneFast = 0; return 0; }   /* 1000:B623 */

    switch (boot_phase) {
    case BOOT_START:
        sceneFast = 1;                     /* 1000:B59F */
        boot_unload();
        res_setDisc(0u);                   /* the boot assets ship on disc 0 */
        bootGt = res_loadChunk("INTRO\\GAMETEK.RSC", &bootGtSize, NULL);
        bootBac = res_loadChunk("INTRO\\BAC.BIT", NULL, NULL);
        bootTreetop = res_loadChunk("INTRO\\TREETOP.BIT", NULL, NULL);
        bootTreebot = res_loadChunk("INTRO\\TREEBOT.BIT", NULL, NULL);
        bootApple = res_loadChunk("INTRO\\APPLE.SPR", NULL, NULL);
        bootWords = res_loadChunk("INTRO\\WORDS.BIT", NULL, NULL);
        bootRsc = res_loadChunk("INTRO\\INTRO.RSC", &bootRscSize, NULL);
        bootItb = res_loadChunk("INTRO\\ITB.BIT", NULL, NULL);
        if (!bootGt || !bootBac || !bootTreetop || !bootTreebot ||
            !bootApple || !bootWords || !bootRsc || !bootItb) {
            boot_unload();
            boot_phase = BOOT_DONE;
            return 0;
        }
        bootEntryCount = bootRscSize / 4u;
        bootEntries = malloc(bootEntryCount * sizeof(uint32_t));
        bootGtEntryCount = bootGtSize / 4u;
        bootGtEntries = malloc(bootGtEntryCount * sizeof(uint32_t));
        bootListBuf = malloc(OBJ_SEG_SIZE);
        bootObj = malloc(OBJ_SEG_SIZE);
        bootGtList = malloc(OBJ_SEG_SIZE);
        if (!bootEntries || !bootGtEntries || !bootListBuf || !bootObj ||
            !bootGtList) {
            boot_unload();
            boot_phase = BOOT_DONE;
            return 0;
        }
        for (unsigned i = 0; i < bootEntryCount; i++)
            bootEntries[i] = rd_le_u32(bootRsc + i * 4u);
        for (unsigned i = 0; i < bootGtEntryCount; i++)
            bootGtEntries[i] = rd_le_u32(bootGt + i * 4u);

        /* the words stash (b7df: 4160 bytes copied to 202F:0x8689 — the
         * bar's 1-bit text, consumed by the transition handler) */

        /* b5d9-b5e8: the boot's fade to the zeroed ramp (to black), BP=2.
         * The music init (8437/83f7/84a5, mus\title.ALB) is deferred. */
        memset(boot_ramp, 0, sizeof(boot_ramp));
        bootBankCountdown = 0u;
        bootGtGroup = 0u;
        bootGtBank = 0u;
        palette_buildFade(0u, 256u, 2u, boot_ramp);
        boot_phase = BOOT_GT_PRE;
        return 1;

    case BOOT_GT_PRE:
        if (palette_fadeActive()) return 1;
        /* b24b: decode the display-list object (entry 0) and relocate it:
         * [u16 group count][768-byte 6-bit palette][u16 object index per
         * group] — the original's fade targets (0x6FEA/0x7026/0x7032/
         * 0x704A) and idx array (0x72EA) are all offsets into this buffer */
        {
            uint16_t chk = 0;
            uint32_t off0;
            memcpy(&off0, bootGt, 4);
            int sz = res_decode_lzw_mem(bootGt, bootGtSize, off0,
                                        bootGtList, OBJ_SEG_SIZE, &chk);
            if (sz < 0 || chk != 0) {
                /* b3cc's b446->b2c4: the failed decode unwinds
                 * play_scene_sequence — init_level_scene still runs the
                 * bank cycles (toward unrelocated-garbage palette bytes,
                 * exactly as the asm reads 202F:0x6FE8's garbage) */
                bootHold = 30u;
                boot_phase = BOOT_GT_BANK_HOLD;
                return 1;
            }
            (void)sz;
        }
        /* b24e-b260: the first group's frame shown, then the fade into the
         * embedded palette — the b26a branch ([0x627C] set for the boot):
         * DAC entries 0-3 only at BP=2.  The frames use entries 0-3 until
         * obj 28/29, whose 4-19 (TEK, blues) and 32-46 (Presents, gold)
         * pixels stay black until the bank cycles and the 0x20-0x2F fade
         * colour them — the original's sequence.  (The b27b branch — all
         * 256 entries at BP=0x19 — would show the 4-19 dark reds/teals
         * instead: the wrong colours.) */
        {
            unsigned idx = rd_le_u16(bootGtList + 0x302u);
            boot_gt_show(idx);
        }
        /* b26a: the boot's first fade — the fast branch ([0x627C] is set for
         * the whole boot), i.e. entries 0-3 at BP=2; the b27b branch (all
         * 256 at BP=0x19) would show the 4-19 dark reds/teals */
        scene_paletteFade(bootGtList + 2u);
        boot_phase = BOOT_GT_FADE0;
        return 1;

    case BOOT_GT_FADE0:
        if (palette_fadeActive()) return 1;
        bootGtGroup = 0u;
        boot_phase = BOOT_GT_FRAME;
        return 1;

    case BOOT_GT_FRAME: {
        /* b293-b2c2: CX = count+1 iterations over the idx array (the array
         * holds count+1 words — the object tail is consumed exactly); the
         * key test lives in BOOT_GT_FLIP (the asm's b2ba, between the flip
         * and the next frame).  LIVE-VERIFIED (2026-10-24): the machine's
         * relocated count word reads 32 (→ CX = 33) — the loop plays all
         * count+1 frames including the last (obj 29), whose expansion
         * wipes the assembled logo down to a small entry-8 blob; that
         * vanish-and-blob is authentic machine behavior. */
        unsigned count = rd_le_u16(bootGtList);
        if (bootGtGroup > count) {
            bootHold = 30u;                /* bf19: the 30-tick pre-bank hold */
            boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        {
            unsigned idx = rd_le_u16(bootGtList + 0x302u + bootGtGroup * 2u);
            if (!boot_gt_show(idx)) {
                /* the game's b446->b2c4: a failed decode (the checksum!)
                 * unwinds play_scene_sequence — the remaining groups are
                 * skipped, straight to the bank cycles */
                bootHold = 30u;            /* bf19: the 30-tick pre-bank hold */
                boot_phase = BOOT_GT_BANK_HOLD;
                return 1;
            }
        }
        bootGtGroup++;
        bootHold = 1u;                     /* b2d3: the boot branch ([0x627C]
                                            * set, b59f): [0x5AC8] >= 1 */
        boot_phase = BOOT_GT_FLIP;
        return 1;
    }

    case BOOT_GT_FLIP:
        /* the cadence's remaining tick: wait_flag_timeout (b2d3)'s boot
         * branch waits for the first ISR tick ([0x5AC8] >= 1), then 87c7's
         * flip spins until the ISR applies it at the next boundary — 2 ticks
         * per group with BOOT_GT_FRAME's.  (The 6-tick wait is the
         * [0x627C]-clear branch — the level scene players, not the boot.) */
        if (--bootHold != 0u) return 1;
        /* b2ba: the key test sits between the flip and the next frame.
         * b2c4 is play_scene_sequence's OWN epilogue — the RET lands back
         * in init_level_scene (bf0f) and the bank cycles still run.  A
         * TAPPED key skips only the remaining frames; a HELD key aborts
         * the whole boot in the bank hold's first c0c6 poll. */
        if (boot_keyState()) {
            bootHold = 30u;                /* bf19: the 30-tick pre-bank hold */
            boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        boot_phase = BOOT_GT_FRAME;
        return 1;

    case BOOT_GT_BANK_HOLD:
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (bf1c) */
        if (--bootHold != 0u) return 1;
        /* bf21: this bank's 4 entries fade into gtpal+0x48 (entry 24),
         * BP=3 — the banks start at DAC entry 4, +4 per cycle */
        palette_buildFade(4u + bootGtBank * 4u, 4u, 3u,
                          bootGtList + 2u + 0x48u);
        boot_phase = BOOT_GT_BANK_A;
        return 1;

    case BOOT_GT_BANK_A:
        if (boot_abort_poll()) return 1;   /* c0af (bf2e) */
        if (palette_fadeActive()) return 1;
        /* bf32: the same bank into gtpal+0x3C (entry 20), BP=0x1e */
        palette_buildFade(4u + bootGtBank * 4u, 4u, 0x1Eu,
                          bootGtList + 2u + 0x3Cu);
        boot_phase = BOOT_GT_BANK_B;
        return 1;

    case BOOT_GT_BANK_B:
        if (boot_abort_poll()) return 1;   /* c0af (bf3e) */
        if (palette_fadeActive()) return 1;
        if (++bootGtBank < 4u) {
            bootHold = 30u;
            boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        /* bf48: entries 0x20-0x2F into gtpal+0x60 (entry 32), BP=0x32 */
        palette_buildFade(0x20u, 0x10u, 0x32u, bootGtList + 2u + 0x60u);
        boot_phase = BOOT_GT_LAST_F;
        return 1;

    case BOOT_GT_LAST_F:
        if (boot_abort_poll()) return 1;   /* c0af (bf57) */
        if (palette_fadeActive()) return 1;
        bootHold = 0x1A4u;                 /* bf5a: the 420-tick hold */
        boot_phase = BOOT_GT_HOLD;
        return 1;

    case BOOT_GT_HOLD:
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (bf5d) */
        if (--bootHold != 0u) return 1;
        /* bf60: fade everything to 0x4BBC — the GT stage never calls
         * palette_load_6bit, so that buffer is still zeroed here: to
         * black.  (palette_working() is the port's 0x4BBC and is zeroed
         * for the same reason.) */
        palette_buildFade(0u, 256u, 0x32u, palette_working());
        boot_phase = BOOT_GT_OUT;
        return 1;

    case BOOT_GT_OUT:
        if (boot_abort_poll()) return 1;   /* c0af (bf6f) */
        if (palette_fadeActive()) return 1;
        free(bootGt); bootGt = NULL; bootGtSize = 0;
        free(bootGtEntries); bootGtEntries = NULL; bootGtEntryCount = 0;
        free(bootGtList); bootGtList = NULL;
        if (!boot_card_show("INTRO\\IDI.BIT")) {
            boot_unload();
            boot_phase = BOOT_DONE;
            return 0;
        }
        /* bd87: fade everything to idi's own palette, BP=0x32 */
        palette_buildFade(0u, 256u, 0x32u, palette_working());
        boot_phase = BOOT_IDI_FADE;
        return 1;

    case BOOT_IDI_FADE:
        if (boot_abort_poll()) return 1;   /* c0af (bd96) */
        if (palette_fadeActive()) return 1;
        bootHold = 0x1A4u;                 /* bd99: the 420-tick hold */
        boot_phase = BOOT_IDI_HOLD;
        return 1;

    case BOOT_IDI_HOLD:
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (bd9c) */
        if (--bootHold != 0u) return 1;
        /* bd9f: fade everything to the zeroed ramp, BP=0x32 */
        palette_buildFade(0u, 256u, 0x32u, boot_ramp);
        boot_phase = BOOT_IDI_OUT;
        return 1;

    case BOOT_IDI_OUT:
        if (boot_abort_poll()) return 1;   /* c0af (bdae) */
        if (palette_fadeActive()) return 1;
        if (!boot_card_show("INTRO\\FIRST.BIT")) {
            boot_unload();
            boot_phase = BOOT_DONE;
            return 0;
        }
        /* bd27-bd3c: the staggered fade-in, bank 0: DAC entries 16-31 into
         * the working palette's own bank, BP=0x23 (7 banks, +16 per bank) */
        bootGtBank = 0u;
        palette_buildFade(0x10u, 0x10u, 0x23u, palette_working() + 0x30u);
        boot_phase = BOOT_FIRST_BANK;
        return 1;

    case BOOT_FIRST_BANK:
        if (boot_abort_poll()) return 1;   /* c0af (bd3c) */
        if (palette_fadeActive()) return 1;
        bootHold = 0x118u;                 /* bd3f: the 280-tick hold per bank */
        boot_phase = BOOT_FIRST_HOLD;
        return 1;

    case BOOT_FIRST_HOLD:
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (bd42) */
        if (--bootHold != 0u) return 1;
        if (++bootGtBank < 7u) {
            /* bd48: the next bank — target = working+0x30+bank*0x30 */
            palette_buildFade(0x10u + bootGtBank * 0x10u, 0x10u, 0x23u,
                              palette_working() + 0x30u + bootGtBank * 0x30u);
            boot_phase = BOOT_FIRST_BANK;
            return 1;
        }
        bootHold = 0x118u;                 /* bd50: one more 0x118-tick hold */
        boot_phase = BOOT_FIRST_LASTHOLD;
        return 1;

    case BOOT_FIRST_LASTHOLD:
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (bd53) */
        if (--bootHold != 0u) return 1;
        /* bd56-bd62: fade everything to the zeroed ramp, BP=0x23 */
        palette_buildFade(0u, 256u, 0x23u, boot_ramp);
        boot_phase = BOOT_FIRST_OUT;
        return 1;

    case BOOT_FIRST_OUT:
        if (boot_abort_poll()) return 1;   /* c0af (bd65) */
        if (palette_fadeActive()) return 1;
        boot_phase = BOOT_ITB_SHOW;
        return 1;

    case BOOT_ITB_SHOW:
        /* the ITB title card (present_frame 1000:BDBC: the 64768-byte card
         * = a 64000-byte image + palette).  present_frame loads the card's
         * OWN palette into the working palette (bdbc, palette_load_6bit
         * 8d24, SI=0xFA00 — the RAM copy only) and the fade below drives
         * the DAC to it, so the card displays through its own colours.
         * bac's palette_load_6bit (b6bc, SI=0xE600) refills the working
         * palette before the dwell but the DAC keeps the card's colours
         * until the post-dwell fade — the port does the same. */
        memcpy(video_page(0), bootItb, 64000u);
        palette_loadWorking(bootItb + 64000u);
        free(bootItb); bootItb = NULL;
        /* b68f-b69e: fade the DAC to the working palette (= the card's own
         * palette) and wait; the card fades in.  The zeroed boot_ramp is
         * the 202F:0x48BC scratch — clear_fade_ramp (b90f) zeroes it
         * after every transition, the port boots it zeroed. */
        palette_buildFade(0u, 256u, 0x23u, palette_working());
        boot_phase = BOOT_CARD_FADE;
        return 1;

    case BOOT_CARD_FADE:
        if (boot_abort_poll()) return 1;   /* c0af (b69e) */
        if (palette_fadeActive()) return 1;
        /* b6c3: bac's palette_load_6bit — the working palette only; the
         * DAC keeps the card's colours through the dwell (bac.bit =
         * [58880-byte screen][768-byte palette at +0xE600]) */
        palette_loadWorking(bootBac + 0xE600u);
        dwellTicks = 280u;                 /* 0x118: the initial dwell */
        boot_phase = BOOT_DWELL;
        return 1;

    case BOOT_DWELL:
        if (--dwellTicks > 0) return 1;    /* no key skip in the original */
        /* b6f2-b701: fade the DAC to the zeroed 0x48BC ramp (to black) and
         * wait — the card fades out */
        palette_buildFade(0u, 256u, 0x23u, boot_ramp);
        boot_phase = BOOT_FADEOUT;
        return 1;

    case BOOT_FADEOUT:
        if (boot_abort_poll()) return 1;   /* c0af (b7b8) */
        if (palette_fadeActive()) return 1;
        dwellTicks = 0x8Cu;                /* b704/c0b8: the 140-tick hold */
        boot_phase = BOOT_BLACK;
        return 1;

    case BOOT_BLACK:
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (b707) */
        if (--dwellTicks > 0) return 1;
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
            /* b791-b7b8: the scene's tail — the final frame stays up for
             * 0x8C ticks, then everything fades to the zeroed ramp (to
             * black) at BP=0x1e.  (The asm's own re-render + re-present of
             * the last frame and its b611 boot-tail fade — from black to
             * the same zeroed ramp, BP=0x23 — are visual no-ops here.) */
            bootHold = 0x8Cu;
            boot_phase = BOOT_SCENE_HOLD;
            return 1;
        }
        if (script_seekPending(&boot_script)) {
            bootListPos = boot_seek(bootListBuf, bootListSize, bootListStart,
                                    script_seekFrame(&boot_script));
            bootFrame = script_seekFrame(&boot_script);
            script_seekDone(&boot_script);
        }
        /* the transition cue (cmd 1): run_screen_transition (1000:B82E) —
         * the white flash (the 0x48BC ramp at BP=2), then the words redraw
         * and the fade back.  It blocks the frame loop in the original. */
        {
            unsigned ti = script_transitionIndex(&boot_script);
            if ((int)ti != boot_bar_arg) {
                boot_bar_arg = (int)ti;
                /* b840-b852: the white ramp — 0x3F everywhere but entry 0
                 * (bytes 0x48BC-0x48BE are never filled) and the 0xE0-0xEF
                 * bank (zeroed: the captions stay black through the flash) */
                memset(boot_ramp, 0x3Fu, sizeof(boot_ramp));
                boot_ramp[0] = boot_ramp[1] = boot_ramp[2] = 0u;
                memset(boot_ramp + 0x2A0u, 0, 0x30u);
                palette_buildFade(0u, 256u, 2u, boot_ramp);
                boot_phase = BOOT_TRANS;
                return 1;
            }
        }
        /* fade_countdown (b7c0), each frame: 0x28 ticks after the
         * transition, fade the 0xE0-0xEF bank toward 202F:0x4DCC — nothing
         * in the EXE writes that region, so the target is whatever
         * palette_load_6bit left at working+0x211 */
        if (bootBankCountdown != 0u && --bootBankCountdown == 0u)
            palette_buildFade(0xE0u, 0x10u, 0x28u, palette_working() + 0x211u);
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
            uint32_t next = boot_blit_group(bootListBuf, bootListSize,
                                            bootRsc, bootRscSize, bootEntries,
                                            bootEntryCount, bootObj, bootListPos,
                                            page);
            if (--bootDwellCount == 0) {
                bootDwellCount = bootGroupDwell;
                bootListPos = next;
                bootFrame++;
            }
        }
        /* the present: the pool's rows 4-183 into the visible rows 0-179
         * (deplanarise_bitmap), the bar (rows 182-197) and the black
         * margins composed in — the original's bar lives in the visible
         * page below every present, untouched until the next transition */
        boot_compose_present(video_page(0));
        scene_osys->videoPresent(0);
        bootWaitTicks = 2u;                /* the 2-tick frame wait */
        boot_phase = BOOT_WAIT;
        return 1;
    }

    case BOOT_WAIT:
        if (--bootWaitTicks == 0) boot_phase = BOOT_FRAME;
        return 1;

    case BOOT_SCENE_HOLD:
        /* b7a3-b7b8: the scene's final hold, then the BP=0x1e fade to the
         * zeroed ramp; BOOT_OUTRO waits for it and unloads */
        if (boot_abort_poll()) return 1;   /* c0b8's c0c6 poll (b7a6) */
        if (--bootHold != 0u) return 1;
        palette_buildFade(0u, 256u, 0x1Eu, boot_ramp);
        boot_phase = BOOT_OUTRO;
        return 1;

    case BOOT_TRANS:
        if (palette_fadeActive()) return 1;
        /* b866: the words' block expands into BOTH visible pages — the
         * port folds it into its one page via the compose */
        boot_bar_draw(boot_bar_arg);
        boot_compose_present(video_page(0));
        bootTransTicks = 0xAu;             /* b869: the 10-tick hold */
        boot_phase = BOOT_TRANS_HOLD;
        return 1;

    case BOOT_TRANS_HOLD:
        if (--bootTransTicks != 0u) return 1;
        /* b873-b896: fade back to the working palette, clear the ramp,
         * start the caption bank fade countdown */
        palette_buildFade(0u, 256u, 0xAu, palette_working());
        memset(boot_ramp, 0, sizeof(boot_ramp));   /* clear_fade_ramp b90f */
        bootBankCountdown = 0x28u;                 /* [0x644E]             */
        boot_phase = BOOT_FRAME;
        return 1;

    case BOOT_OUTRO:
        if (boot_abort_poll()) return 1;   /* c0af (b7b8) */
        if (palette_fadeActive()) return 1;
        boot_unload();
        boot_phase = BOOT_DONE;
        return 0;

    case BOOT_ABORT:
        /* the c0c6 -> b60a unwind: clear_fade_ramp (b90f) is a no-op here
         * (boot_ramp is never dirtied), then the final fade to the zeroed
         * ramp, div 0x23 (b611-b61d), waited with the NON-aborting 8b8b —
         * the remaining boot stages (the cards, the caveman scene) are
         * skipped and the boot ends black */
        palette_buildFade(0u, 256u, 0x23u, boot_ramp);
        boot_phase = BOOT_ABORT_FADE;
        return 1;

    case BOOT_ABORT_FADE:
        if (palette_fadeActive()) return 1;
        boot_unload();
        boot_phase = BOOT_DONE;
        return 0;

    case BOOT_DONE:
        return 0;
    }
    return 0;
}

/* probe hook: the boot state machine's current phase (read-only, for the
 * tests and the diagnostics) */
unsigned scene_boot_phaseForProbe(void) { return (unsigned)boot_phase; }

/* probe hook: the AL expander, for the offline byte-comparisons against the
 * validated python decoder (tools/ani_lzw.py) */
void scene_al_expand_for_probe(const uint8_t *src, uint32_t src_size,
                               uint8_t *dst, uint32_t dst_size)
{
    scene_al_expand(src, src_size, dst, dst_size);
}
