/* scene.c — the .ANI/.RSC interpreter (game_structure.md §1 subsystem 11,
 * re_formats.md §8.5-8.7a): the playlist script, the display list, the
 * frame-keyed event cues.
 *
 * scene_play_level_intro(introIndex) ports cd_file_op (1000:AFF8) as a
 * per-tick state machine: the first call for an introIndex loads
 * ANM\ANI<idx>.ANI from the table-selected disc, draws the scoops.apl
 * background over the daily.bit screen, sets the palette, and dwells 210
 * ticks; each further call _plays the next scene object (6 ticks each) and
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
#include "scene.h"
#include "audio.h"
#include "font.h"
#include "frontend.h"
#include "game_strs.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "script.h"
#include "textpage.h"
#include "video.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANI_NAME  "ANM\\ANI%02u.ANI"
#define PAGE_SIZE (320u * 200u)
#define OBJ_SEG_SIZE                                                           \
    65536u   /* the original decodes objects into a 64K segment */

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
    uint8_t *image;
    uint32_t image_size;
    uint32_t table_size;
    uint32_t n_entries;
    uint32_t *entries; /* copy of the u32 offset table           */
} Ani;

typedef enum {
    SCENE_IDLE, /* not started                                */
    SCENE_MUSIC, /* 1000:B07B's timing_loop: the first frame waits */
    SCENE_PANEL_FADE, /* the panel's first 0x20 entries fade in     */
    SCENE_DWELL, /* the 210-tick pre-scene dwell               */
    SCENE_PANEL_OUT, /* the 0x20-entry fade to black before frames */
    SCENE_PALFADE, /* the fade to the script object's palette    */
    SCENE_QUIT_FADE, /* key-skip: the 0x100-entry fade to black    */
    SCENE_PLAY, /* decode + draw the next object (instant)    */
    SCENE_WAIT, /* per-object 6-tick wait                     */
    SCENE_CHAIN, /* decide the next scene (instant)            */
    SCENE_CHAIN_WAIT, /* wait up to 210 ticks for a key (replay)    */
    SCENE_RESTART, /* chain advance: reload ANI (instant)        */
    SCENE_DONE
} ScenePhase;

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
 * Stage 4 _plays INTRO\INTRO.RSC's display list over a horizontally
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
 *   5. present, then the frame wait (2 ticks at 1000:b780; 4 in the port —
 *      port_deviations.md D-248)
 * The quit cue (frame 385) ends it; the frame counter ran 0..386 live.
 * A held key/fire also aborts the boot: c0c6 is polled after every frame's
 * wait (1000:b787) and in the transition's fade wait (1000:c0af).
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
    BOOT_GT_PRE, /* the boot fade-to-black wait; the list decode  */
    BOOT_GT_FADE0, /* group 0 shown; the 4-entry fast fade + wait   */
    BOOT_GT_FRAME, /* one group shown (b29b's LOOP)                 */
    BOOT_GT_FLIP, /* the flip-apply tick (87c7's ISR sync)         */
    BOOT_GT_BANK_HOLD, /* the 30-tick hold before each bank cycle       */
    BOOT_GT_BANK_A, /* bank fade -> gtpal+0x48 (BP=3) + wait         */
    BOOT_GT_BANK_B, /* bank fade -> gtpal+0x3C (BP=0x1e) + wait      */
    BOOT_GT_LAST_F, /* entries 0x20-0x2F -> gtpal+0x60 (BP=0x32)     */
    BOOT_GT_HOLD, /* the 0x1a4-tick hold                           */
    BOOT_GT_OUT, /* fade all -> 0x4BBC (zeroed: nothing loaded yet
                         * — to black), BP=0x32 (bf60)                   */
    BOOT_ABORT, /* c0c6 -> JMP b60a: a held key/fire unwinds the
                         * whole boot; the final fade (div 0x23) starts   */
    BOOT_ABORT_FADE, /* its wait (8b8b — the non-aborting spin)       */
    BOOT_IDI_FADE, /* idi.bit shown; fade to its own palette        */
    BOOT_IDI_HOLD, /* the 0x1a4-tick hold                           */
    BOOT_IDI_OUT, /* fade to the zeroed ramp + wait                */
    BOOT_FIRST_BANK, /* bank fade (16 entries at BP=0x23) + wait      */
    BOOT_FIRST_HOLD, /* the 0x118-tick hold per bank                  */
    BOOT_FIRST_LASTHOLD, /* bd50: the extra 0x118-tick hold after bank 7  */
    BOOT_FIRST_OUT, /* fade to the zeroed ramp (BP=0x23) + wait      */
    BOOT_ITB_SHOW, /* the ITB card present + its fade-in start      */
    BOOT_CARD_FADE, /* its wait                                      */
    BOOT_DWELL, /* the 280-tick initial dwell                    */
    BOOT_FADEOUT, /* the post-dwell fade to the zeroed ramp (b6f2) */
    BOOT_BLACK, /* the 0x8C-tick black hold (c0b8)               */
    BOOT_FRAME, /* cues + scroll + sprites + display-list blit   */
    BOOT_WAIT, /* the frame wait (4 ticks in the port; D-248)  */
    BOOT_SCENE_HOLD, /* the scene's final 0x8C-tick hold (b7a3)       */
    BOOT_TRANS, /* the transition's white-flash fade (b854)      */
    BOOT_TRANS_HOLD, /* the words redraw + 0xa-tick hold (b866-b871)  */
    BOOT_OUTRO, /* the scene-tail fade to black (b7a9, BP=0x1e)  */
    BOOT_DONE
} BootPhase;

/* --- scene playback ----------------------------------------------------- */

/* The scene chain table, ported from 202F:0x6293 (exe bytes): chain[scene]
 * = the next scene played after it; self = replay until a key; 4 = the
 * tail loop (scenes 4 and 8 ping-pong until a key). */
static const uint8_t _scene_chain_table[10] = { 0, 1, 2, 3, 8, 5, 6, 7, 4, 9 };

/* Background-object table, ported from 202F:0x629e: bgTable[movie] = the
 * 0xE00-byte apl\scoops.apl block drawn as the cutscene background. */
static const uint8_t _scene_bg_table[10] = { 1, 2, 3, 4, 5, 6, 8, 0, 5, 7 };

/* 202F:0x6304 — the panel's "scoop" headlines: string N+1 of the block at
 * 202F:0x6305 for scene index N (1000:B0C1 scans N+1 NULs forward).  String 9
 * (202F:0x6416) is the empty one, so scene 8 draws nothing and scene 9 prints
 * "HUMANS RECOVER THE STOLEN IDOL!".  Drawn centred at 0xA0, row 0x41,
 * colour 0x10.  The strings themselves are game_strs' SCENE_HEADLINE block. */

static const OSystem *_scene_osys;
static ScenePhase _scene_phase = SCENE_IDLE;
static uint32_t _scene_level = 0xFFFFu; /* the level the intro is for */

static uint8_t *_ani_data;
static Ani _ani;
static uint8_t *_script_buf; /* decoded script object                */
static uint32_t _plays;
static const uint16_t *_indices; /* into _script_buf + 2 + 768             */
static uint8_t *_obj_buf; /* 64K segment model                    */
static uint8_t *_scoops_buf;
static uint32_t _scoops_size;
static uint8_t *_text_spr; /* apl\text.spr — the panel's font */

static uint8_t _cur_scene;
static int32_t _chain_replay; /* 210-tick wait ends in a self-replay */
static uint16_t _dwell_ticks;
static uint8_t _panel_pal[768]; /* daily.bit's palette (masked 6-bit) */
static int32_t _panel_pal_valid;
/* c537/c563's target: [0x48BC] is the 256x3 scratch ramp, and every writer
 * restores it to zero — clear_fade_ramp (1000:B90F) at boot, the level's
 * discovery op (1000:3A6A) on every exit (1000:3CF9/3D01), run_screen_
 * transition (1000:B82E), and menu_loop's 0xC0 only (1000:051C) — so the
 * panel's pre-scene fade and the key exit are fades to black. */
static const uint8_t _scene_black_pal[768];
/* [0x627C]: boot_intro_sequence's fast mode — its only writer (set at
 * 1000:B59F, cleared at 1000:B623, entry's caller only).  play_scene_sequence
 * reads it twice: the pre-frame palette fade is 4 entries div 2 instead of
 * 0x100 div 0x19 (1000:B263), and wait_flag_timeout waits 1 tick instead of
 * 6 per object (1000:B2D3).  The level's .ANI always runs with it clear. */
static int32_t _scene_fast;
static uint16_t _obj_wait;
static uint16_t _chain_wait;
static uint32_t _obj_index;

/* The boot's flag-style key tests (b2ba and c0c6) read the original's
 * level-triggered ISR flags ([0x48A3] / +0x5ACE bit-7) — not an event
 * queue.  They read the main loop's per-tick poll (scene_note_input):
 * extra eventsPoll calls here would consume input events the foreground
 * owns (the null backend's script is call-indexed). */
static InputState _scene_last_input;

static BootPhase _boot_phase = BOOT_START;
static CueScript _boot_script;
static uint8_t *_boot_bac; /* intro\bac.bit — scroll source + palette */
static uint8_t *_boot_treetop; /* intro\treetop.bit 140x113               */
static uint8_t *_boot_treebot; /* intro\treebot.bit 21x59                 */
static uint8_t *_boot_apple; /* intro\apple.spr 16x16                   */
static uint8_t *_boot_words; /* intro\words.bit — the bar's 1-bit text  */
static uint8_t _boot_bar[16u * 320u]; /* the visible bar, rows 182-197     */
static int32_t _boot_bar_arg = -1; /* the transition the bar holds      */
static uint8_t *_boot_rsc; /* intro\intro.rsc — the display list      */
static uint32_t _boot_rsc_size;
static uint32_t *_boot_entries;
static uint32_t _boot_entry_count;
static uint8_t *_boot_list_buf; /* the decoded display-list object         */
static uint32_t _boot_list_size; /* its decoded size                        */
static uint8_t *_boot_obj; /* the records' decoded overlay objects    */
static uint32_t _boot_list_start; /* 0x6FEA: the records' start              */
static uint32_t _boot_list_pos; /* [0x6468]: the current group             */
static uint32_t _boot_frame; /* [0x6466]: the display-list group index  */
static uint32_t _boot_group_dwell; /* [0x6460]: frames per group              */
static uint32_t _boot_dwell_count; /* [0x6462]: the group dwell countdown     */
static uint32_t _boot_scroll_pos; /* [0x6473]: the horizontal scroll         */
static uint16_t _boot_wait_ticks;
static uint8_t _boot_ramp[768]; /* the fade-target scratch (202F:0x48BC)   */
static uint32_t _boot_trans_ticks; /* the 0xa-tick words hold (b869)          */
static uint32_t _boot_bank_countdown; /* [0x644E]: the b7c0 bank-fade timer   */
static uint8_t *_boot_gt; /* intro\gametek.rsc — the logo display list */
static uint32_t _boot_gt_size;
static uint32_t *_boot_gt_entries; /* its u32 object table                    */
static uint32_t _boot_gt_entry_count;
static uint8_t *_boot_gt_list; /* decoded object 0: [u16 count][768-byte  *
                                   * palette][u16 object index x count]      */
static uint32_t _boot_gt_group; /* the group loop counter (b29b's CX)      */
static uint32_t _boot_gt_bank; /* the 4-bank palette-cycle counter        */
static uint8_t *_boot_itb; /* intro\itb.bit held until its stage      */
static uint32_t _boot_hold; /* the stages' hold counters (c0b8's CX)   */

/** @brief --- AL expander: blit_background_data (1000:B2EB) --------------------
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
 *
 * @param src the source buffer
 * @param src_size the source size
 * @param dst the destination buffer
 * @param dst_size the destination size
 */
static void _scene_al_expand(const uint8_t *src, uint32_t src_size,
                             uint8_t *dst, uint32_t dst_size)
{
    uint32_t si = 0, di = 0;

    while (si < src_size) {
        uint8_t cl = src[si++];

        if (cl == 0) { /* [count][byte] run */
            if (si + 1 >= src_size) break;
            uint8_t count = src[si++];
            uint8_t fill = src[si++];
            for (uint32_t i = 0; i < count && di < dst_size; i++)
                dst[di++] = fill;
            continue;
        }

        if (cl < 0x80) { /* literal copy */
            for (uint32_t i = 0; i < cl && si < src_size && di < dst_size; i++)
                dst[di++] = src[si++];
            continue;
        }

        if (cl == 0x80) { /* 16-bit control word */
            if (si + 1 >= src_size) break;
            uint16_t cw = rd_le_u16(src + si);
            si += 2;
            if (cw == 0) break; /* END */
            if (!(cw & 0x8000u)) { /* skip CW bytes */
                di += cw;
                if (di > dst_size) di = dst_size;
                continue;
            }
            /* CW < 0: bit 15 set */
            uint16_t cx = (uint16_t)(cw & 0x7fff);
            uint8_t ch =
                (uint8_t)((cw >> 8) & 0x7f); /* high byte, bit 15 cleared */
            if (ch < 0x40) { /* copy CX bytes from stream */
                for (uint32_t i = 0; i < cx && si < src_size && di < dst_size;
                     i++)
                    dst[di++] = src[si++];
            } else { /* fill (CX - 0x4000) bytes */
                if (si >= src_size) break;
                uint8_t fill = src[si++];
                uint32_t run = (uint32_t)cx -
                               0x4000u; /* the listing's SUB CH,0x40 (b365) */
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

/**
 * @brief Load one .ANI newspaper into the scene decoder.
 *
 * @param a the ANI object
 * @param ani_idx the animation index
 * @return 0 on success, -1 when the file is missing
 */
static int32_t _ani_load(Ani *a, uint32_t ani_idx)
{
    char name[32];
    snprintf(name, sizeof name, ANI_NAME, ani_idx);

    uint16_t chk = 0;
    (void)chk;
    a->image = res_load_chunk(name, &a->image_size, &chk);
    if (!a->image) return -1;

    a->table_size = rd_le_u32(a->image);
    a->n_entries = a->table_size / 4;
    a->entries = malloc(a->n_entries * sizeof(uint32_t));
    if (!a->entries) {
        free(a->image);
        a->image = NULL;
        return -1;
    }
    for (uint32_t i = 0; i < a->n_entries; i++)
        a->entries[i] = rd_le_u32(a->image + i * 4u);
    return 0;
}

/** @brief Decode the LZW object at table entry `idx` into `out` (capacity
 * `out_size`).  Returns the decompressed size, or -1 on failure.
 * *chkOut receives the object's checksum accumulator (0 = the game's
 * scene-object test passes, 1000:B444).
 *
 * @param a the ANI object
 * @param idx the table entry index
 * @param out the output buffer
 * @param out_size the output size
 * @param chkOut the output checksum
 * @return 0 on success, -1 on a bad table entry
 */
static int32_t _ani_decode_object(const Ani *a, uint32_t idx, uint8_t *out,
                                  uint32_t out_size, uint16_t *chkOut)
{
    if (idx >= a->n_entries) return -1;
    return res_decode_lzw_mem(a->image, a->image_size, a->entries[idx], out,
                              out_size, chkOut);
}

void scene_set_fast_mode(int32_t on) { _scene_fast = on ? 1u : 0u; }

void scene_reset_intro(void)
{
    _scene_level = 0xFFFFu;
    _scene_phase = SCENE_IDLE;
}

/** @brief play_scene_sequence's pre-frame palette fade (1000:B263), shared by the
 * level's .ANI and the boot's logo: fast mode = 4 entries div 2, otherwise
 * all 256 entries div 0x19.
 *
 * @param target the target value
 */
static void _scene_palette_fade(const uint8_t *target)
{
    if (_scene_fast) palette_build_fade(0u, 4u, 2u, target);
    else palette_build_fade(0u, 0x100u, 0x19u, target);
}

/** @brief cd_file_op's key exit (1000:B157): a 0x100-entry fade to [0x48BC]
 * (black) before the function returns.
 */
static void _scene_quit_fade(void)
{
    palette_build_fade(0u, 0x100u, 0x19u, _scene_black_pal);
    audio_fade_out(); /* 1000:B166: present_frames_75 -> 1000:B169 stop */
    _scene_phase = SCENE_QUIT_FADE;
}

int32_t scene_init(const OSystem *osys)
{
    _scene_osys = osys;
    return 0;
}

/**
 * @brief The boot intro's key gate: any key or fire.
 *
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _boot_key_state(void)
{
    return _scene_last_input.keyHeld != 0u || _scene_last_input.lastKey != 0u ||
           (_scene_last_input.joyBits & 0x80u) != 0u;
}

void scene_note_input(const InputState *in) { _scene_last_input = *in; }

/** @brief Any key or fire: the original's [0x48A3] / +0x5ACE bit-7 checks.  (The
 * level intro's skip polls — its contract predates the boot's flag-cache
 * model; the boot's waits use _boot_key_state instead.)
 *
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _scene_key_held(void)
{
    InputState in;
    _scene_osys->eventsPoll(&in);
    return in.keyHeld != 0u || in.lastKey != 0u || (in.joyBits & 0x80u) != 0;
}

/**
 * @brief Release the current intro scene's assets.
 */
static void _scene_unload(void)
{
    free(_ani_data);
    _ani_data = NULL;
    free(_script_buf);
    _script_buf = NULL;
    _indices = NULL;
    _plays = 0;
    free(_obj_buf);
    _obj_buf = NULL;
    free(_scoops_buf);
    _scoops_buf = NULL;
    _scoops_size = 0;
    if (_text_spr) font_publish(NULL); /* the glyph table dies with it */
    free(_text_spr);
    _text_spr = NULL;
    memset(&_ani, 0, sizeof _ani);
}

/** @brief Load ANI<index>.ANI + the background assets, draw the background and
 * the palette onto page 0.  Returns 0 on success.
 *
 * @param index the index
 * @param initial the initial value
 * @return 0 on success, -1 when the scene assets are missing
 */
static int32_t _scene_start(uint32_t index, int32_t initial)
{
    _scene_unload();
    _cur_scene = (uint8_t)index;
    res_set_disc(res_movie_disc(index));

    char name[32];
    snprintf(name, sizeof name, ANI_NAME, index);
    if (_ani_load(&_ani, index)) return -1;

    _script_buf = malloc(OBJ_SEG_SIZE);
    _obj_buf = malloc(OBJ_SEG_SIZE);
    if (!_script_buf || !_obj_buf) {
        _scene_unload();
        return -1;
    }

    /* lbm\daily.bit -> the page: [64000-byte screen][768-byte palette] */
    {
        uint32_t dailySize = 0;
        uint8_t *daily = res_load_chunk("LBM\\DAILY.BIT", &dailySize, NULL);
        uint8_t *page = video_page(0);
        if (!daily) {
            _scene_unload();
            return -1;
        }
        memcpy(page, daily, dailySize < PAGE_SIZE ? dailySize : PAGE_SIZE);
        /* 1000:B0AF (palette_load_6bit, 1000:8D24): daily.bit's 768 bytes
         * are masked to 6 bits and staged at 0x4BBC, NOT installed — the
         * panel's first 0x20 entries FADE in (1000:B0DA) */
        _panel_pal_valid = dailySize >= PAGE_SIZE + 768u;
        if (_panel_pal_valid)
            for (uint32_t i = 0; i < 768u; i++) /* 1000:8D24's AND 0x3F */
                _panel_pal[i] = daily[PAGE_SIZE + i] & 0x3Fu;
        free(daily);
    }

    /* apl\text.spr (the original's second load, 1000:C491): the panel's
     * headline is drawn with these glyphs (blit_text_tile), so the scene
     * publishes them for its own frame */
    free(_text_spr);
    _text_spr = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(_text_spr);

    /* apl\scoops.apl -> the background block, overlaid onto the page */
    _scoops_buf = res_load_chunk("APL\\SCOOPS.APL", &_scoops_size, NULL);
    if (_scoops_buf) {
        uint8_t *page = video_page(0);
        uint32_t srcOff = 0x400u + (uint32_t)_scene_bg_table[index] * 0xE00u;
        if (srcOff + 0xE00u <= _scoops_size) {
            /* 1000:B1CF: 56 rows of 64 bytes at 0x8270 (= row 104, x 112 of
             * the 320-wide panel).  The original's row step is the REP
             * MOVSB advance (0x40) PLUS its `ADD DI,0x100`, i.e. 0x140 — the
             * panel's 320-byte row; taking only the 0x100 shears the
             * thumbnail 64 px left per row. */
            const uint8_t *src = _scoops_buf + srcOff;
            for (uint32_t row = 0; row < 0x38u; row++) {
                memcpy(page + 0x8270u + row * 0x140u, src, 0x40u);
                src += 0x40u;
            }
        }
        free(_scoops_buf);
        _scoops_buf = NULL;
    }

    /* the panel's headline (1000:B0D4: blit_text_tile at centre 0xA0, row
     * 0x41, mask 0x10 — OR'd into the panel art, so it takes the panel's own
     * vertical ramp) — the index-th string of 202F:0x6304 */
    {
        const char *headline =
            game_strs[g_game_lang][GAME_STR_SCENE_HEADLINE_0 + index];

        if (headline[0])
            textpage_draw_line_mask(video_page(0), headline, 0x41u, 0x10u);
    }

    _scene_osys->videoPresent(0);

    /* 1000:B0DA: after the panel is drawn and flipped, its first 0x20 DAC
     * entries fade from the previous screen's palette to daily.bit's
     * (div 0x19); the 210-tick dwell starts only after that fade.  A chain
     * advance (C57A) skips the panel and its fade. */
    if (initial && _panel_pal_valid)
        palette_build_fade(0u, 0x20u, 0x19u, _panel_pal);
    return 0;
}

int32_t scene_play_level_intro(uint32_t introIndex)
{
    /* cd_file_op's head (1000:AFFD): the ANIMATIONS board (cell 5,
     * [0x5E79]) non-zero RETs before any file op — cutscenes are off. */
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u) return 0;
    if (introIndex == 0xFFu) return 0; /* no cutscene for this level */

    if (introIndex != _scene_level) { /* (re)start on level change   */
        _scene_level = introIndex;
        if (_scene_start(introIndex, 1)) {
            _scene_phase = SCENE_DONE;
            return 0;
        }
        /* 1000:B061-B07B: play_tune(levA or title, by [0xF7E]) /
         * music_resume / timing_loop.  _scene_start set the movie's disc,
         * which is the [0xF7E] the choice reads. */
        audio_play_tune(res_movie_disc(introIndex) != 0u ? audio_tune_lev
                                                         : audio_tune_title);
        audio_music_resume();
        audio_fade_in();

        /* the script object (entry 0): [u16 _plays-1][768 palette][_indices] */
        uint16_t chk = 0;
        int32_t script_size =
            _ani_decode_object(&_ani, 0, _script_buf, OBJ_SEG_SIZE, &chk);
        if (script_size < 0 || chk != 0) {
            _scene_unload();
            _scene_phase = SCENE_DONE;
            return 0;
        }
        _plays = (uint32_t)rd_le_u16(_script_buf) + 1u;
        if (2u + 768u + _plays * 2u > (uint32_t)script_size) {
            _scene_unload();
            _scene_phase = SCENE_DONE;
            return 0;
        }
        _indices = (const uint16_t *)(_script_buf + 2 + 768);
        /* the script object (entry 0) carries the cutscene's 768-byte
         * 6-bit palette at +2, but it belongs to the FRAMES: the panel is
         * shown in daily.bit's palette for the whole dwell and only then
         * does the DAC fade to the script palette (1000:B263, below). */
        _obj_index = 0;
        /* 1000:B07B: the fade-in blocks before the first frame. */
        _scene_phase = SCENE_MUSIC;
    }

    if (_scene_phase == SCENE_DONE) return 0;

    int32_t keyHeld = _scene_key_held();

    while (1) { /* instant phases resolve within one tick */
        switch (_scene_phase) {
        case SCENE_MUSIC:
            if (audio_fade_busy()) return 1;
            if (_panel_pal_valid) {
                _scene_phase = SCENE_PANEL_FADE;
            } else {
                _scene_phase = SCENE_DWELL;
                _dwell_ticks = 210u;
            }
            break;

        case SCENE_PANEL_FADE:
            if (palette_fade_active()) return 1;
            /* 1000:B0EC: the 210-tick count starts at 0 AFTER the fade
             * returns, so this tick is not one of the 210 */
            _dwell_ticks = 210u;
            _scene_phase = SCENE_DWELL;
            return 1;

        case SCENE_DWELL:
            if (keyHeld) _dwell_ticks = 0; /* any key skips the remainder */
            if (_dwell_ticks > 0) {
                --_dwell_ticks;
                return 1;
            }
            if (keyHeld) {
                _scene_quit_fade();
                return 1;
            }
            /* 1000:B13F: the panel's first 0x20 entries fade back to
             * [0x48BC] (black) over div 0x19; play_scene_sequence's own
             * head (B248) waits for that fade before the prologue frame. */
            palette_build_fade(0u, 0x20u, 0x19u, _scene_black_pal);
            _scene_phase = SCENE_PANEL_OUT;
            return 1;

        case SCENE_QUIT_FADE:
            /* 1000:B15A-B16C: the palette fade, then present_frames_75 (the
             * deferred music_stop) before the function returns. */
            if (palette_fade_active()) return 1;
            if (audio_fade_busy()) return 1;
            _scene_phase = SCENE_DONE;
            return 0;

        case SCENE_PANEL_OUT:
            if (palette_fade_active()) return 1;
            /* 1000:B24E..B260: the FIRST frame is decoded, drawn and flipped
             * BEFORE the palette fade, so the fade happens over the frame and
             * not over the panel (the loop then replays index 0).  A decode
             * failure unwinds the whole player (B22C: JMP B2C4). */
            {
                uint16_t chk = 0;
                int32_t sz = _ani_decode_object(&_ani, _indices[0], _obj_buf,
                                                OBJ_SEG_SIZE, &chk);
                if (sz < 0 || chk != 0) {
                    _scene_phase = SCENE_DONE;
                    return 0;
                }
                _scene_al_expand(_obj_buf, (uint32_t)sz, video_page(0),
                                 PAGE_SIZE);
                _scene_osys->videoPresent(0);
            }
            /* 1000:B263: now the DAC fades to the script object's palette at
             * 0x6FEA (= _script_buf+2): the whole 256 entries div 0x19 (or 4
             * entries div 2 when [0x627C] is set — the instant path), then
             * wait_palette_fade_done. */
            _scene_palette_fade(_script_buf + 2);
            _scene_phase = SCENE_PALFADE;
            return 1;

        case SCENE_PALFADE:
            if (keyHeld) {
                _scene_quit_fade();
                return 1;
            }
            if (palette_fade_active()) return 1;
            _scene_phase = SCENE_PLAY;
            _obj_index = 0;
            break; /* play the first object now */

        case SCENE_PLAY:
            if (_obj_index >= _plays) {
                _scene_phase = SCENE_CHAIN;
                break;
            }
            {
                uint16_t chk = 0;
                int32_t sz = _ani_decode_object(&_ani, _indices[_obj_index],
                                                _obj_buf, OBJ_SEG_SIZE, &chk);
                if (sz < 0 || chk != 0) { /* place_scene_obj: JZ / abort */
                    _scene_phase = SCENE_CHAIN;
                    break;
                }
                _scene_al_expand(_obj_buf, (uint32_t)sz, video_page(0),
                                 PAGE_SIZE);
                _scene_osys->videoPresent(0);
                _obj_wait =
                    _scene_fast ? 1u : 6u; /* wait_flag_timeout 1000:B2D3 */
                _obj_index++;
                _scene_phase = SCENE_WAIT;
                return 1;
            }

        case SCENE_WAIT:
            if (keyHeld) {
                _scene_quit_fade();
                return 1;
            }
            if (--_obj_wait == 0) _scene_phase = SCENE_PLAY;
            return 1;

        case SCENE_CHAIN: {
            uint8_t nxt = _scene_chain_table[_cur_scene];
            if (keyHeld) {
                _scene_quit_fade();
                return 1;
            }
            /* 1000:B17E: `CMP BL,[BX+0x6293]` - a scene whose chain entry is
             * itself waits 210 ticks (1000:B1A6: also the key/fire poll) and
             * then replays from its first object (1000:B189 -> 0xB14D ->
             * 0xB211).  That is the ending's "loop until ESC" and the intros'
             * "runs until skipped".  A chain entry of 4 is the 8->4 tail
             * loop, which waits and then _plays scene 4. */
            _chain_replay = (nxt == _cur_scene) ? 1 : 0;
            if (_chain_replay) {
                _scene_phase = SCENE_CHAIN_WAIT;
                _chain_wait = 210u;
                return 1;
            }
            _cur_scene = nxt; /* chain advance */
            if (nxt == 4u) { /* 1000:B194: wait first */
                _scene_phase = SCENE_CHAIN_WAIT;
                _chain_wait = 210u;
                return 1;
            }
            _scene_phase = SCENE_RESTART;
            break;
        }

        case SCENE_CHAIN_WAIT:
            if (keyHeld) {
                _scene_quit_fade();
                return 1;
            }
            if (--_chain_wait == 0) {
                if (_chain_replay) {
                    _obj_index = 0; /* replay this scene */
                    _scene_phase = SCENE_PLAY;
                } else {
                    _scene_phase = SCENE_RESTART; /* play the chained scene */
                }
            }
            return 1;

        case SCENE_RESTART: /* chain advance: no dwell */
            if (_scene_start(_cur_scene, 0)) {
                _scene_phase = SCENE_DONE;
                return 0;
            }
            _scene_phase = SCENE_PLAY;
            _obj_index = 0;
            break;

        case SCENE_DONE:
        case SCENE_IDLE: return 0;
        }
    }
}

/** @brief One gametek group: place_scene_obj (1000:B3F9) decodes the object out of
 * the RSC's u32 table (the same machine as res_decode_lzw_mem — the b444
 * checksum gate) and the object's RLE stream expands over the whole page
 * (blit_background_data 1000:B2EB — _scene_al_expand is that machine,
 * validated through the level-intro scenes).
 *
 * @param idx the index
 * @return 1 while the group shows, 0 when it finishes
 */
static int32_t _boot_gt_show(uint32_t idx)
{
    if (idx >= _boot_gt_entry_count) return 0;
    uint16_t chk = 0;
    int32_t sz =
        res_decode_lzw_mem(_boot_gt, _boot_gt_size, _boot_gt_entries[idx],
                           _boot_obj, OBJ_SEG_SIZE, &chk);
    if (sz < 0 || chk != 0) return 0; /* 1000:B444: skip */
    _scene_al_expand(_boot_obj, (uint32_t)sz, video_page(0), 64000u);
    return 1;
}

/** @brief present_frame (1000:BDBC) for the idi/first cards: load, the file's own
 * palette into the working palette (8d24, RAM only), the image on the page.
 * The DAC moves when the caller's fade targets the working palette.
 *
 * @param name the name
 * @return 1 while the card shows, 0 when it finishes
 */
static int32_t _boot_card_show(const char *name)
{
    uint32_t size = 0;
    uint8_t *img = res_load_chunk(name, &size, NULL);
    if (!img || size < 64000u) {
        free(img);
        return 0;
    }
    memcpy(video_page(0), img, 64000u);
    palette_load_working(img + 64000u);
    free(img);
    return 1;
}

/** @brief c0c6 (input_abort_check): every c0af/c0b8 wait polls the key each tick;
 * a held key or fire bit jumps b60a — the whole remaining boot (the cards,
 * the caveman scene) is skipped, the final fade runs, done.
 *
 * @return 1 when a key or fire aborts the wait, 0 otherwise
 */
static int32_t _boot_abort_poll(void)
{
    if (!_boot_key_state()) return 0;
    _boot_phase = BOOT_ABORT;
    return 1;
}

/**
 * @brief Release the boot intro's loaded assets.
 */
static void _boot_unload(void)
{
    free(_boot_bac);
    _boot_bac = NULL;
    free(_boot_treetop);
    _boot_treetop = NULL;
    free(_boot_treebot);
    _boot_treebot = NULL;
    free(_boot_apple);
    _boot_apple = NULL;
    free(_boot_words);
    _boot_words = NULL;
    free(_boot_rsc);
    _boot_rsc = NULL;
    _boot_rsc_size = 0;
    free(_boot_entries);
    _boot_entries = NULL;
    _boot_entry_count = 0;
    free(_boot_list_buf);
    _boot_list_buf = NULL;
    free(_boot_obj);
    _boot_obj = NULL;
    free(_boot_gt);
    _boot_gt = NULL;
    _boot_gt_size = 0;
    free(_boot_gt_entries);
    _boot_gt_entries = NULL;
    _boot_gt_entry_count = 0;
    free(_boot_gt_list);
    _boot_gt_list = NULL;
    free(_boot_itb);
    _boot_itb = NULL;
}

/** @brief blit_sprite_transparent (1000:BAA5): a width x height sprite at page
 * (worldX - scroll, pageYOff); 0 bytes are transparent; the sprite clips
 * at the page's right edge and is off-screen once X wraps.
 *
 * @param spr the sprite index
 * @param width the width
 * @param height the height
 * @param worldX the world x
 * @param pageYOff the page y offset
 * @param page the page buffer
 */
static void _boot_blit_sprite(const uint8_t *spr, uint32_t width,
                              uint32_t height, uint32_t worldX,
                              uint32_t pageYOff, uint8_t *page)
{
    uint32_t x = ((uint32_t)worldX - _boot_scroll_pos) & 0xFFFFu;
    if (x >= 320u) return; /* CMP AX,0x140 / JC */
    uint32_t visible = 320u - x;
    if (visible > width) visible = width;
    uint8_t *dst = page + pageYOff + x;
    const uint8_t *src = spr;
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t i = 0; i < visible; i++) {
            uint8_t b = src[i];
            if (b) dst[i] = b; /* 0 = transparent */
        }
        dst += 0x140u; /* the 320-byte row stride */
        src += width; /* the clipped part skipped */
        if (dst + visible > page + PAGE_SIZE) return;
    }
}

/** @brief copy_rows_0x140 (1000:B9F8): the scroll render — the bac screen wrapped
 * horizontally by `scroll`: each of the 184 rows shows
 * bac[row][scroll..320) then bac[row][0..scroll), in 92 two-row pairs
 * (each pair: two DX-byte copies + two BP advances; DX + BP = 320).
 *
 * @param page the page buffer
 * @param bac the BAC image
 * @param bacSize the BAC image size
 * @param scroll the scroll amount
 * @param destOff the destination offset
 */
static void _boot_scroll_render(uint8_t *page, const uint8_t *bac,
                                uint32_t bacSize, uint32_t scroll,
                                uint32_t destOff)
{
    uint32_t ax = scroll % 0x140u;
    uint32_t dx, bp, si, di = destOff;
    if (di != 0) {
        dx = 0x140u - di;
        bp = di;
        si = 0;
    } else {
        dx = 0x140u - ax;
        bp = ax;
        si = ax;
    }
    for (uint32_t i = 0; i < 0x5cu; i++) { /* 92 two-row pairs */
        for (uint32_t j = 0; j < dx; j++) {
            if (si >= bacSize || di >= PAGE_SIZE) return;
            page[di++] = bac[si++];
        }
        di += bp;
        si += bp;
        for (uint32_t j = 0; j < dx; j++) {
            if (si >= bacSize || di >= PAGE_SIZE) return;
            page[di++] = bac[si++];
        }
        di += bp;
        si += bp;
    }
}

/** @brief One display-list record: the stored index INCs (16-bit — a 0xFFFF index
 * wraps to object 0, the display-list object itself), the object LZW-
 * decodes to [u16 width][u16 height][bytes] and transparent-blits at
 * `dest` with the 320-byte row stride (1000:B955's inner blit).
 *
 * @param rsc the resource
 * @param rscSize the resource size
 * @param entries the entry count
 * @param entryCount the entry count
 * @param _obj_buf the object buffer
 * @param storedIdx the stored index
 * @param dest the destination
 * @param listObj the list object
 * @param listObjSize the list object size
 * @param page the page buffer
 */
static void _boot_blit_record(const uint8_t *rsc, uint32_t rscSize,
                              const uint32_t *entries, uint32_t entryCount,
                              uint8_t *_obj_buf, uint32_t storedIdx,
                              uint16_t dest, const uint8_t *listObj,
                              uint32_t listObjSize, uint8_t *page)
{
    uint32_t idx = (storedIdx + 1u) & 0xFFFFu; /* INC AX: 16-bit wrap */
    const uint8_t *src;
    uint32_t srcSize;
    uint16_t chk = 0;
    int32_t sz;
    if (idx == 0) { /* object 0: the list itself */
        src = listObj;
        srcSize = listObjSize;
    } else {
        if (idx >= entryCount) return;
        sz = res_decode_lzw_mem(rsc, rscSize, entries[idx], _obj_buf,
                                OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) return; /* 1000:B444: skip */
        src = _obj_buf;
        srcSize = (uint32_t)sz;
    }
    uint32_t width = rd_le_u16(src);
    uint32_t height = rd_le_u16(src + 2);
    const uint8_t *body = src + 4;
    uint32_t bodySize = srcSize - 4u;
    uint32_t di = dest;
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t i = 0; i < width && di + i < PAGE_SIZE; i++) {
            uint32_t so = (uint32_t)r * width + i;
            uint8_t b = so < bodySize ? body[so] : 0u; /* past the object: 0 */
            if (b) page[di + i] = b; /* 0 = transparent */
        }
        di += 0x140u;
        if (di >= PAGE_SIZE) return;
    }
}

/** @brief blit_display_list (1000:B955): blit the current group's records; returns
 * the position after the group's 0xFFFF terminator.  The records are the
 * decoded object 0's OWN TAIL — §8.7a: nobody builds the list at runtime,
 * `MOV SI,[0x6468]` ([0x6468] = obj0 base+2) walks the scratch segment, so
 * the records read from the DECODED buffer and bounds against its size.
 * The objects themselves still LZW-decode out of the raw RSC's table.
 *
 * @param listBuf the list buffer
 * @param listSize the list size
 * @param rsc the resource
 * @param rscSize the resource size
 * @param entries the entry count
 * @param entryCount the entry count
 * @param _obj_buf the object buffer
 * @param listPos the list position
 * @param page the page buffer
 * @return the source cursor after the group
 */
static uint32_t _boot_blit_group(const uint8_t *listBuf, uint32_t listSize,
                                 const uint8_t *rsc, uint32_t rscSize,
                                 const uint32_t *entries, uint32_t entryCount,
                                 uint8_t *_obj_buf, uint32_t listPos,
                                 uint8_t *page)
{
    uint32_t si = listPos;
    while (1) { /* the group's records */
        if (si + 4 > listSize) break;
        uint16_t dest = rd_le_u16(listBuf + si);
        uint16_t idx = rd_le_u16(listBuf + si + 2);
        si += 4;
        _boot_blit_record(rsc, rscSize, entries, entryCount, _obj_buf, idx,
                          dest, _boot_list_buf, _boot_list_size, page);
        if (si + 2 > listSize) break;
        if (rd_le_u16(listBuf + si) == 0xFFFFu) {
            si += 2;
            break;
        } /* the group end */
    }
    return si;
}

/** @brief The seek (cmd 4): walk `frame` 0xFFFF group terminators from the list
 * start — the group for `frame`.  The terminators live in the decoded
 * object 0's tail (§8.7a: the list is consumed straight out of the decoded
 * object, [0x6468] runs in the scratch segment), so the walk bounds
 * against the DECODED size, not the raw RSC file.  Returns the group's
 * offset in the decoded buffer.
 *
 * @param listBuf the list buffer
 * @param listSize the list size
 * @param listStart the list start
 * @param frame the frame index
 * @return the list position of the requested frame
 */
static uint32_t _boot_seek(const uint8_t *listBuf, uint32_t listSize,
                           uint32_t listStart, uint32_t frame)
{
    uint32_t si = listStart;
    for (uint32_t g = 0; g < frame && si + 2 <= listSize;) {
        if (rd_le_u16(listBuf + si) == 0xFFFFu) {
            si += 2;
            g++;
        } else si += 4; /* a [dest][idx] record */
    }
    return si;
}

/** @brief the bar text (run_screen_transition's builder, 1000:B89D -> B8A8): the
 * words' 1-bit block at [0x8689 + arg*0x258] — 16 rows × 40 bytes, each
 * bit = one pixel, MSB first, colour 0xE0+row (bac's grey gradient bank)
 * or black — written into the visible bar, rows 182-197, once per
 * transition; the per-frame present never touches those rows so the bar
 * persists until the next transition.
 *
 * @param arg the argument
 */
static void _boot_bar_draw(uint32_t arg)
{
    for (uint32_t r = 0; r < 16u; r++) {
        uint8_t *dst = _boot_bar + r * 320u;
        const uint8_t *src = _boot_words + arg * 600u + r * 40u;
        for (uint32_t x = 0; x < 320u; x++) {
            uint8_t b = src[x / 8u];
            dst[x] = ((b >> (7u - (x % 8u))) & 1u) ? (uint8_t)(0xE0u + r) : 0u;
        }
    }
}

/** @brief deplanarise_bitmap (1000:BB05) — the present.  The visible page (chain-4
 * mode 13h, pitch 80 bytes/row/plane) is filled from the pool's rows 4-183
 * (SI+0x500, 180 rows): visible row v = pool row v+4.  The visible rows
 * 180-199 are outside every present — the bar (run_screen_transition's
 * direct A000 writes, rows 182-197, colours 0xE0-0xEF = bac's grey
 * gradient) persists there until the next transition redraws it; rows
 * 180-181 and 198-199 stay black.  The port keeps one flat page, so the
 * compose folds both stages into it.
 *
 * @param page the page buffer
 */
static void _boot_compose_present(uint8_t *page)
{
    memmove(page, page + 0x500u, 180u * 320u); /* pool rows 4-183 */
    memset(page + 180u * 320u, 0, 2u * 320u); /* rows 180-181    */
    memcpy(page + 182u * 320u, _boot_bar, 16u * 320u);
    memset(page + 198u * 320u, 0, 2u * 320u); /* rows 198-199    */
}

/** @brief update_scene (1000:B9B5): the scrolled bac over the pool rows 0-183
 * plus the tree/apple sprites.  The caller advances the scroll when the cue
 * script's scroll gate is on.
 */
static void _boot_scene_render(void)
{
    uint8_t *page = video_page(0);
    _boot_scroll_render(page, _boot_bac, 0xE600u, _boot_scroll_pos, 0u);
    _boot_scroll_render(page, _boot_bac, 0xE600u, _boot_scroll_pos,
                        0x140u - (_boot_scroll_pos % 0x140u));
    _boot_blit_sprite(_boot_treetop, 140u, 113u, 0x5A2u, 0x140u, page);
    _boot_blit_sprite(_boot_treebot, 21u, 59u, 0x608u, 0x8E80u, page);
    if (_boot_frame < 331u) /* the apple leaves at frame 331 */
        _boot_blit_sprite(_boot_apple, 16u, 16u, 0x5B5u, 0x3700u, page);
}

/** @brief BOOT_START: load the boot's assets, build the two u32 object tables,
 * reset the fade scratch and start the title music (1000:B59F-b5FB).
 *
 * @return 1 while the boot intro shows, 0 when the assets cannot be loaded
 */
static int32_t _boot_start(void)
{
    _scene_fast = 1; /* 1000:B59F */
    _boot_unload();
    res_set_disc(0u); /* the boot assets ship on disc 0 */
    _boot_gt = res_load_chunk("INTRO\\GAMETEK.RSC", &_boot_gt_size, NULL);
    _boot_bac = res_load_chunk("INTRO\\BAC.BIT", NULL, NULL);
    _boot_treetop = res_load_chunk("INTRO\\TREETOP.BIT", NULL, NULL);
    _boot_treebot = res_load_chunk("INTRO\\TREEBOT.BIT", NULL, NULL);
    _boot_apple = res_load_chunk("INTRO\\APPLE.SPR", NULL, NULL);
    _boot_words = res_load_chunk("INTRO\\WORDS.BIT", NULL, NULL);
    _boot_rsc = res_load_chunk("INTRO\\INTRO.RSC", &_boot_rsc_size, NULL);
    _boot_itb = res_load_chunk("INTRO\\ITB.BIT", NULL, NULL);
    if (!_boot_gt || !_boot_bac || !_boot_treetop || !_boot_treebot ||
        !_boot_apple || !_boot_words || !_boot_rsc || !_boot_itb) {
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;
    }
    _boot_entry_count = _boot_rsc_size / 4u;
    _boot_entries = malloc(_boot_entry_count * sizeof(uint32_t));
    _boot_gt_entry_count = _boot_gt_size / 4u;
    _boot_gt_entries = malloc(_boot_gt_entry_count * sizeof(uint32_t));
    _boot_list_buf = malloc(OBJ_SEG_SIZE);
    _boot_obj = malloc(OBJ_SEG_SIZE);
    _boot_gt_list = malloc(OBJ_SEG_SIZE);
    if (!_boot_entries || !_boot_gt_entries || !_boot_list_buf || !_boot_obj ||
        !_boot_gt_list) {
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;
    }
    for (uint32_t i = 0; i < _boot_entry_count; i++)
        _boot_entries[i] = rd_le_u32(_boot_rsc + i * 4u);
    for (uint32_t i = 0; i < _boot_gt_entry_count; i++)
        _boot_gt_entries[i] = rd_le_u32(_boot_gt + i * 4u);

    /* the words stash (b7df: 4160 bytes copied to 202F:0x8689 — the
     * bar's 1-bit text, consumed by the transition handler) */

    /* b5d9-b5e8: the boot's fade to the zeroed ramp (to black), BP=2,
     * then b5f2-b5fb: play_tune / music_resume / timing_loop with
     * mus\title.ALB (the trio's music starts here, before the logo). */
    memset(_boot_ramp, 0, sizeof(_boot_ramp));
    _boot_bank_countdown = 0u;
    _boot_gt_group = 0u;
    _boot_gt_bank = 0u;
    palette_build_fade(0u, 256u, 2u, _boot_ramp);
    audio_play_tune(audio_tune_title); /* 1000:b5f2 */
    audio_music_resume(); /* 1000:b5f8 */
    audio_fade_in(); /* 1000:b5fb */
    _boot_phase = BOOT_GT_PRE;
    return 1;
}

/** @brief BOOT_GT_PRE: wait out the fade to black and the title music's
 * fade-in, then decode the logo display list (entry 0), show its first group
 * and fade the first DAC entries into the embedded palette (1000:B24B-B26A).
 *
 * @return 1 while the boot intro shows
 */
static int32_t _boot_gt_pre(void)
{
    if (palette_fade_active()) return 1;
    /* b5f2-b5fb: the original blocks inside timing_loop while the title
     * music fades in, so the logo starts after the fade (~150 ticks).
     * The port's fade is non-blocking; wait for it here, in the flow. */
    if (audio_fade_busy()) return 1;
    /* b24b: decode the display-list object (entry 0) and relocate it:
     * [u16 group count][768-byte 6-bit palette][u16 object index per
     * group] — the original's fade targets (0x6FEA/0x7026/0x7032/
     * 0x704A) and idx array (0x72EA) are all offsets into this buffer */
    {
        uint16_t chk = 0;
        uint32_t off0 = rd_le_u32(_boot_gt);
        int32_t sz = res_decode_lzw_mem(_boot_gt, _boot_gt_size, off0,
                                        _boot_gt_list, OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) {
            /* b3cc's b446->b2c4: the failed decode unwinds
             * play_scene_sequence — init_level_scene still runs the
             * bank cycles (toward unrelocated-garbage palette bytes,
             * exactly as the asm reads 202F:0x6FE8's garbage) */
            _boot_hold = 30u;
            _boot_phase = BOOT_GT_BANK_HOLD;
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
        uint32_t idx = rd_le_u16(_boot_gt_list + 0x302u);
        _boot_gt_show(idx);
    }
    /* b26a: the boot's first fade — the fast branch ([0x627C] is set for
     * the whole boot), i.e. entries 0-3 at BP=2; the b27b branch (all
     * 256 at BP=0x19) would show the 4-19 dark reds/teals */
    _scene_palette_fade(_boot_gt_list + 2u);
    _boot_phase = BOOT_GT_FADE0;
    return 1;
}

/** @brief BOOT_GT_FRAME: show the next gametek group (1000:B293-B2C2) and
 * arm the flip wait; a failed decode skips to the bank cycles.
 *
 * @return 1 while the boot intro shows
 */
static int32_t _boot_gt_frame(void)
{
    /* b293-b2c2: CX = count+1 iterations over the idx array (the array
     * holds count+1 words — the object tail is consumed exactly); the
     * key test lives in BOOT_GT_FLIP (the asm's b2ba, between the flip
     * and the next frame).  LIVE-VERIFIED (2026-10-24): the machine's
     * relocated count word reads 32 (→ CX = 33) — the loop _plays all
     * count+1 frames including the last (obj 29), whose expansion
     * wipes the assembled logo down to a small entry-8 blob; that
     * vanish-and-blob is authentic machine behavior. */
    uint32_t count = rd_le_u16(_boot_gt_list);
    if (_boot_gt_group > count) {
        _boot_hold = 30u; /* bf19: the 30-tick pre-bank hold */
        _boot_phase = BOOT_GT_BANK_HOLD;
        return 1;
    }
    {
        uint32_t idx = rd_le_u16(_boot_gt_list + 0x302u + _boot_gt_group * 2u);
        if (!_boot_gt_show(idx)) {
            /* the game's b446->b2c4: a failed decode (the checksum!)
             * unwinds play_scene_sequence — the remaining groups are
             * skipped, straight to the bank cycles */
            _boot_hold = 30u; /* bf19: the 30-tick pre-bank hold */
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
    }
    _boot_gt_group++;
    _boot_hold = 1u; /* b2d3: the boot branch ([0x627C]
                                        * set, b59f): [0x5AC8] >= 1 */
    _boot_phase = BOOT_GT_FLIP;
    return 1;
}

/** @brief BOOT_BLACK: after the 0x8C-tick black hold, decode the scene's
 * display list (entry 0), reset the cue/frame state and present frame 0
 * (1000:B707-B748).
 *
 * @return 1 while the boot intro shows, 0 when the display list is corrupt
 */
static int32_t _boot_black(void)
{
    /* decode the display-list object (entry 0): [u16 ?][records...] —
     * the records start at object+2 ([0x6468] init 0x6FEA = 0x6FE8+2) */
    {
        uint16_t chk = 0;
        int32_t sz =
            res_decode_lzw_mem(_boot_rsc, _boot_rsc_size, _boot_entries[0],
                               _boot_list_buf, OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) {
            _boot_unload();
            _boot_phase = BOOT_DONE;
            return 0;
        }
        _boot_list_size = (uint32_t)sz;
        _boot_list_start = 2u;
    }
    _boot_list_pos = _boot_list_start;
    _boot_frame = 0u;
    _boot_group_dwell = 1u;
    _boot_dwell_count = 1u;
    _boot_scroll_pos = 0u;
    /* 1000:b71e-b739: render and present frame 0 before the cue loop,
     * so the frame-0 transition flash runs over the scene, as the
     * original does. */
    _boot_scene_render();
    (void)_boot_blit_group(_boot_list_buf, _boot_list_size, _boot_rsc,
                           _boot_rsc_size, _boot_entries, _boot_entry_count,
                           _boot_obj, _boot_list_pos, video_page(0));
    _boot_compose_present(video_page(0));
    _scene_osys->videoPresent(0);
    _boot_list_pos = _boot_list_start; /* b748 */
    script_intro_start(&_boot_script);
    _boot_phase = BOOT_FRAME;
    return 1;
}

/** @brief BOOT_FRAME: one cue-driven scene frame — the cues, the seek, the
 * transition trigger, the caption bank countdown, the scroll render, the
 * group blit and the present (1000:B750-B780).
 *
 * @return 1 while the boot intro shows
 */
static int32_t _boot_frame_step(void)
{
    script_fire(&_boot_script, _boot_frame);
    if (script_quit(&_boot_script) || script_done(&_boot_script)) {
        /* b791-b7b8: the scene's tail — the final frame stays up for
         * 0x8C ticks, then everything fades to the zeroed ramp (to
         * black) at BP=0x1e.  (The asm's own re-render + re-present of
         * the last frame and its b611 boot-tail fade — from black to
         * the same zeroed ramp, BP=0x23 — are visual no-ops here.) */
        _boot_hold = 0x8Cu;
        _boot_phase = BOOT_SCENE_HOLD;
        return 1;
    }
    if (script_seek_pending(&_boot_script)) {
        _boot_list_pos =
            _boot_seek(_boot_list_buf, _boot_list_size, _boot_list_start,
                       script_seek_frame(&_boot_script));
        _boot_frame = script_seek_frame(&_boot_script);
        script_seek_done(&_boot_script);
    }
    /* the transition cue (cmd 1): run_screen_transition (1000:B82E) —
     * the white flash (the 0x48BC ramp at BP=2), then the words redraw
     * and the fade back.  It blocks the frame loop in the original. */
    {
        uint32_t ti = script_transition_index(&_boot_script);
        if ((int32_t)ti != _boot_bar_arg) {
            _boot_bar_arg = (int32_t)ti;
            /* b840-b852: the white ramp — 0x3F everywhere but entry 0
             * (bytes 0x48BC-0x48BE are never filled) and the 0xE0-0xEF
             * bank (zeroed: the captions stay black through the flash) */
            memset(_boot_ramp, 0x3Fu, sizeof(_boot_ramp));
            _boot_ramp[0] = _boot_ramp[1] = _boot_ramp[2] = 0u;
            memset(_boot_ramp + 0x2A0u, 0, 0x30u);
            palette_build_fade(0u, 256u, 2u, _boot_ramp);
            _boot_phase = BOOT_TRANS;
            return 1;
        }
    }
    /* fade_countdown (b7c0), each frame: 0x28 ticks after the
     * transition, fade the 0xE0-0xEF bank toward 202F:0x4DCC — nothing
     * in the EXE writes that region, so the target is whatever
     * palette_load_6bit left at working+0x211 */
    if (_boot_bank_countdown != 0u && --_boot_bank_countdown == 0u)
        palette_build_fade(0xE0u, 0x10u, 0x28u, palette_working() + 0x211u);
    /* update_scene (1000:B9B5): the scroll render + the sprites; the
     * scroll position advances after the render (when scroll is on) */
    _boot_scene_render();
    if (script_scroll_on(&_boot_script))
        _boot_scroll_pos += script_scroll_delta(&_boot_script);
    /* blit_display_list: the group's overlays; the dwell counts down
     * per frame — when it expires, the next group + frame */
    {
        uint8_t *page = video_page(0);
        uint32_t next = _boot_blit_group(
            _boot_list_buf, _boot_list_size, _boot_rsc, _boot_rsc_size,
            _boot_entries, _boot_entry_count, _boot_obj, _boot_list_pos, page);
        if (--_boot_dwell_count == 0) {
            _boot_dwell_count = _boot_group_dwell;
            _boot_list_pos = next;
            _boot_frame++;
        }
    }
    /* the present: the pool's rows 4-183 into the visible rows 0-179
     * (deplanarise_bitmap), the bar (rows 182-197) and the black
     * margins composed in — the original's bar lives in the visible
     * page below every present, untouched until the next transition */
    _boot_compose_present(video_page(0));
    _scene_osys->videoPresent(0);
    /* 1000:b780 waits `[0x5AC8] >= 2`; doubled only for this scene —
     * port_deviations.md D-248 (requested slowdown) */
    _boot_wait_ticks = 4u;
    _boot_phase = BOOT_WAIT;
    return 1;
}

int32_t scene_play_boot_intro(void)
{
    if (_boot_phase == BOOT_DONE) {
        _scene_fast = 0;
        return 0;
    } /* 1000:B623 */

    switch (_boot_phase) {
    case BOOT_START: return _boot_start();

    case BOOT_GT_PRE: return _boot_gt_pre();

    case BOOT_GT_FADE0:
        if (palette_fade_active()) return 1;
        _boot_gt_group = 0u;
        _boot_phase = BOOT_GT_FRAME;
        return 1;

    case BOOT_GT_FRAME: return _boot_gt_frame();

    case BOOT_GT_FLIP:
        /* the cadence's remaining tick: wait_flag_timeout (b2d3)'s boot
         * branch waits for the first ISR tick ([0x5AC8] >= 1), then 87c7's
         * flip spins until the ISR applies it at the next boundary — 2 ticks
         * per group with BOOT_GT_FRAME's.  (The 6-tick wait is the
         * [0x627C]-clear branch — the level scene players, not the boot.) */
        if (--_boot_hold != 0u) return 1;
        /* b2ba: the key test sits between the flip and the next frame.
         * b2c4 is play_scene_sequence's OWN epilogue — the RET lands back
         * in init_level_scene (bf0f) and the bank cycles still run.  A
         * TAPPED key skips only the remaining frames; a HELD key aborts
         * the whole boot in the bank hold's first c0c6 poll. */
        if (_boot_key_state()) {
            _boot_hold = 30u; /* bf19: the 30-tick pre-bank hold */
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        _boot_phase = BOOT_GT_FRAME;
        return 1;

    case BOOT_GT_BANK_HOLD:
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (bf1c) */
        if (--_boot_hold != 0u) return 1;
        /* bf21: this bank's 4 entries fade into gtpal+0x48 (entry 24),
         * BP=3 — the banks start at DAC entry 4, +4 per cycle */
        palette_build_fade(4u + _boot_gt_bank * 4u, 4u, 3u,
                           _boot_gt_list + 2u + 0x48u);
        _boot_phase = BOOT_GT_BANK_A;
        return 1;

    case BOOT_GT_BANK_A:
        if (_boot_abort_poll()) return 1; /* c0af (bf2e) */
        if (palette_fade_active()) return 1;
        /* bf32: the same bank into gtpal+0x3C (entry 20), BP=0x1e */
        palette_build_fade(4u + _boot_gt_bank * 4u, 4u, 0x1Eu,
                           _boot_gt_list + 2u + 0x3Cu);
        _boot_phase = BOOT_GT_BANK_B;
        return 1;

    case BOOT_GT_BANK_B:
        if (_boot_abort_poll()) return 1; /* c0af (bf3e) */
        if (palette_fade_active()) return 1;
        if (++_boot_gt_bank < 4u) {
            _boot_hold = 30u;
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        /* bf48: entries 0x20-0x2F into gtpal+0x60 (entry 32), BP=0x32 */
        palette_build_fade(0x20u, 0x10u, 0x32u, _boot_gt_list + 2u + 0x60u);
        _boot_phase = BOOT_GT_LAST_F;
        return 1;

    case BOOT_GT_LAST_F:
        if (_boot_abort_poll()) return 1; /* c0af (bf57) */
        if (palette_fade_active()) return 1;
        _boot_hold = 0x1A4u; /* bf5a: the 420-tick hold */
        _boot_phase = BOOT_GT_HOLD;
        return 1;

    case BOOT_GT_HOLD:
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (bf5d) */
        if (--_boot_hold != 0u) return 1;
        /* bf60: fade everything to 0x4BBC — the GT stage never calls
         * palette_load_6bit, so that buffer is still zeroed here: to
         * black.  (palette_working() is the port's 0x4BBC and is zeroed
         * for the same reason.) */
        palette_build_fade(0u, 256u, 0x32u, palette_working());
        _boot_phase = BOOT_GT_OUT;
        return 1;

    case BOOT_GT_OUT:
        if (_boot_abort_poll()) return 1; /* c0af (bf6f) */
        if (palette_fade_active()) return 1;
        free(_boot_gt);
        _boot_gt = NULL;
        _boot_gt_size = 0;
        free(_boot_gt_entries);
        _boot_gt_entries = NULL;
        _boot_gt_entry_count = 0;
        free(_boot_gt_list);
        _boot_gt_list = NULL;
        if (!_boot_card_show("INTRO\\IDI.BIT")) {
            _boot_unload();
            _boot_phase = BOOT_DONE;
            return 0;
        }
        /* bd87: fade everything to idi's own palette, BP=0x32 */
        palette_build_fade(0u, 256u, 0x32u, palette_working());
        _boot_phase = BOOT_IDI_FADE;
        return 1;

    case BOOT_IDI_FADE:
        if (_boot_abort_poll()) return 1; /* c0af (bd96) */
        if (palette_fade_active()) return 1;
        _boot_hold = 0x1A4u; /* bd99: the 420-tick hold */
        _boot_phase = BOOT_IDI_HOLD;
        return 1;

    case BOOT_IDI_HOLD:
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (bd9c) */
        if (--_boot_hold != 0u) return 1;
        /* bd9f: fade everything to the zeroed ramp, BP=0x32 */
        palette_build_fade(0u, 256u, 0x32u, _boot_ramp);
        _boot_phase = BOOT_IDI_OUT;
        return 1;

    case BOOT_IDI_OUT:
        if (_boot_abort_poll()) return 1; /* c0af (bdae) */
        if (palette_fade_active()) return 1;
        if (!_boot_card_show("INTRO\\FIRST.BIT")) {
            _boot_unload();
            _boot_phase = BOOT_DONE;
            return 0;
        }
        /* bd27-bd3c: the staggered fade-in, bank 0: DAC entries 16-31 into
         * the working palette's own bank, BP=0x23 (7 banks, +16 per bank) */
        _boot_gt_bank = 0u;
        palette_build_fade(0x10u, 0x10u, 0x23u, palette_working() + 0x30u);
        _boot_phase = BOOT_FIRST_BANK;
        return 1;

    case BOOT_FIRST_BANK:
        if (_boot_abort_poll()) return 1; /* c0af (bd3c) */
        if (palette_fade_active()) return 1;
        _boot_hold = 0x118u; /* bd3f: the 280-tick hold per bank */
        _boot_phase = BOOT_FIRST_HOLD;
        return 1;

    case BOOT_FIRST_HOLD:
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (bd42) */
        if (--_boot_hold != 0u) return 1;
        if (++_boot_gt_bank < 7u) {
            /* bd48: the next bank — target = working+0x30+bank*0x30 */
            palette_build_fade(0x10u + _boot_gt_bank * 0x10u, 0x10u, 0x23u,
                               palette_working() + 0x30u +
                                   _boot_gt_bank * 0x30u);
            _boot_phase = BOOT_FIRST_BANK;
            return 1;
        }
        _boot_hold = 0x118u; /* bd50: one more 0x118-tick hold */
        _boot_phase = BOOT_FIRST_LASTHOLD;
        return 1;

    case BOOT_FIRST_LASTHOLD:
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (bd53) */
        if (--_boot_hold != 0u) return 1;
        /* bd56-bd62: fade everything to the zeroed ramp, BP=0x23 */
        palette_build_fade(0u, 256u, 0x23u, _boot_ramp);
        _boot_phase = BOOT_FIRST_OUT;
        return 1;

    case BOOT_FIRST_OUT:
        if (_boot_abort_poll()) return 1; /* c0af (bd65) */
        if (palette_fade_active()) return 1;
        _boot_phase = BOOT_ITB_SHOW;
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
        memcpy(video_page(0), _boot_itb, 64000u);
        palette_load_working(_boot_itb + 64000u);
        free(_boot_itb);
        _boot_itb = NULL;
        /* b68f-b69e: fade the DAC to the working palette (= the card's own
         * palette) and wait; the card fades in.  The zeroed _boot_ramp is
         * the 202F:0x48BC scratch — clear_fade_ramp (b90f) zeroes it
         * after every transition, the port boots it zeroed. */
        palette_build_fade(0u, 256u, 0x23u, palette_working());
        _boot_phase = BOOT_CARD_FADE;
        return 1;

    case BOOT_CARD_FADE:
        if (_boot_abort_poll()) return 1; /* c0af (b69e) */
        if (palette_fade_active()) return 1;
        /* b6c3: bac's palette_load_6bit — the working palette only; the
         * DAC keeps the card's colours through the dwell (bac.bit =
         * [58880-byte screen][768-byte palette at +0xE600]) */
        palette_load_working(_boot_bac + 0xE600u);
        _dwell_ticks = 280u; /* 0x118: the initial dwell */
        _boot_phase = BOOT_DWELL;
        return 1;

    case BOOT_DWELL:
        if (--_dwell_ticks > 0) return 1; /* no key skip in the original */
        /* b6f2-b701: fade the DAC to the zeroed 0x48BC ramp (to black) and
         * wait — the card fades out */
        palette_build_fade(0u, 256u, 0x23u, _boot_ramp);
        _boot_phase = BOOT_FADEOUT;
        return 1;

    case BOOT_FADEOUT:
        if (_boot_abort_poll()) return 1; /* c0af (b7b8) */
        if (palette_fade_active()) return 1;
        _dwell_ticks = 0x8Cu; /* b704/c0b8: the 140-tick hold */
        _boot_phase = BOOT_BLACK;
        return 1;

    case BOOT_BLACK:
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (b707) */
        if (--_dwell_ticks > 0) return 1;
        return _boot_black();

    case BOOT_FRAME: return _boot_frame_step();

    case BOOT_WAIT:
        if (--_boot_wait_ticks == 0) {
            if (_boot_abort_poll()) return 1; /* b787: c0c6            */
            _boot_phase = BOOT_FRAME;
        }
        return 1;

    case BOOT_SCENE_HOLD:
        /* b7a3-b7b8: the scene's final hold, then the BP=0x1e fade to the
         * zeroed ramp; BOOT_OUTRO waits for it and unloads */
        if (_boot_abort_poll()) return 1; /* c0b8's c0c6 poll (b7a6) */
        if (--_boot_hold != 0u) return 1;
        palette_build_fade(0u, 256u, 0x1Eu, _boot_ramp);
        _boot_phase = BOOT_OUTRO;
        return 1;

    case BOOT_TRANS:
        if (_boot_abort_poll()) return 1; /* c0af (b863) */
        if (palette_fade_active()) return 1;
        /* b866: the words' block expands into BOTH visible pages — the
         * port folds it into its one page via the compose */
        _boot_bar_draw(_boot_bar_arg);
        _boot_compose_present(video_page(0));
        _boot_trans_ticks = 0xAu; /* b869: the 10-tick hold */
        _boot_phase = BOOT_TRANS_HOLD;
        return 1;

    case BOOT_TRANS_HOLD:
        if (--_boot_trans_ticks != 0u) return 1;
        /* b873-b896: fade back to the working palette, clear the ramp,
         * start the caption bank fade countdown */
        palette_build_fade(0u, 256u, 0xAu, palette_working());
        memset(_boot_ramp, 0, sizeof(_boot_ramp)); /* clear_fade_ramp b90f */
        _boot_bank_countdown = 0x28u; /* [0x644E]             */
        _boot_phase = BOOT_FRAME;
        return 1;

    case BOOT_OUTRO:
        if (_boot_abort_poll()) return 1; /* c0af (b7b8) */
        if (palette_fade_active()) return 1;
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;

    case BOOT_ABORT:
        /* the c0c6 -> b60a unwind: clear_fade_ramp (b90f) zeroes 0x48BC —
         * the transition may have left its white ramp there — then the
         * final fade to the zeroed ramp, div 0x23 (b611-b61d), waited with
         * the NON-aborting 8b8b.  The remaining boot stages (the cards, the
         * caveman scene) are skipped and the boot ends black. */
        memset(_boot_ramp, 0, sizeof(_boot_ramp));
        palette_build_fade(0u, 256u, 0x23u, _boot_ramp);
        _boot_phase = BOOT_ABORT_FADE;
        return 1;

    case BOOT_ABORT_FADE:
        if (palette_fade_active()) return 1;
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;

    case BOOT_DONE: return 0;
    }
    return 0;
}

uint32_t scene_boot_phase_for_probe(void) { return (uint32_t)_boot_phase; }

/** @brief probe hook: the AL expander, for the offline byte-comparisons against the
 * validated python decoder (tools/ani_lzw.py)
 *
 * @param src the source buffer
 * @param src_size the source size
 * @param dst the destination buffer
 * @param dst_size the destination size
 */
void scene_al_expand_for_probe(const uint8_t *src, uint32_t src_size,
                               uint8_t *dst, uint32_t dst_size)
{ _scene_al_expand(src, src_size, dst, dst_size); }
