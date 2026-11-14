/* The level cutscene player: cd_file_op's ANI sequence, the panel and the
 * scene chain (1000:AFF8/B07B/B157/B263).
 */

#include "scene/scene.h"
#include "scene/scene_internal.h"

#include "audio.h"
#include "font.h"
#include "frontend.h"
#include "game_strings.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "game_resources.h"
#include "textpage.h"
#include "video.h"
#include "byteorder.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    SCENE_IDLE, // not started
    SCENE_MUSIC, // 1000:B07B's timing_loop: the first frame waits
    SCENE_PANEL_FADE, // the panel's first 0x20 entries fade in
    SCENE_DWELL, // the 210-tick pre-scene dwell
    SCENE_PANEL_OUT, // the 0x20-entry fade to black before frames
    SCENE_PALFADE, // the fade to the script object's palette
    SCENE_QUIT_FADE, // key-skip: the 0x100-entry fade to black
    SCENE_PLAY, // decode + draw the next object (instant)
    SCENE_WAIT, // per-object 6-tick wait
    SCENE_CHAIN, // decide the next scene (instant)
    SCENE_CHAIN_WAIT, // wait up to 210 ticks for a key (replay)
    SCENE_RESTART, // chain advance: reload ANI (instant)
    SCENE_DONE
} ScenePhase;

/* --- scene playback ----------------------------------------------------- */

/* The scene chain table, ported from 202F:0x6293 (exe bytes): chain[scene]
 * = the next scene played after it; self = replay until a key; 4 = the
 * tail loop (scenes 4 and 8 ping-pong until a key). */
static const uint8_t _scene_chain_table[10] = { 0, 1, 2, 3, 8, 5, 6, 7, 4, 9 };

/* Background-object table, ported from 202F:0x629e: bgTable[movie] = the
 * 0xE00-byte apl\scoops.apl block drawn as the cutscene background. */
static const uint8_t _scene_bg_table[10] = { 1, 2, 3, 4, 5, 6, 8, 0, 5, 7 };

/* 202F:0x6304 - the panel's "scoop" headlines: string N+1 of the block at
 * 202F:0x6305 for scene index N (1000:B0C1 scans N+1 NULs forward).  String 9
 * (202F:0x6416) is the empty one, so scene 8 draws nothing and scene 9 prints
 * "HUMANS RECOVER THE STOLEN IDOL!".  Drawn centred at 0xA0, row 0x41,
 * colour 0x10.  The strings themselves are game_strings' SCENE_HEADLINE block. */

static ScenePhase _scene_phase = SCENE_IDLE;
static uint16_t _scene_level = 0xFFFFu; // the level the intro is for

static Ani _ani;
static uint8_t *_script_buf; // decoded script object
static uint16_t _plays;
static const uint16_t *_indices; // into _script_buf + 2 + 768
static uint8_t *_obj_buf; // 64K segment model
static uint8_t *_scoops_buf;
static uint32_t _scoops_size;
static uint8_t *_text_spr; // apl\text.spr - the panel's font

static uint8_t _cur_scene;
static uint8_t _chain_replay; // 210-tick wait ends in a self-replay
uint16_t _dwell_ticks;
static uint8_t _panel_pal[768]; // daily.bit's palette (masked 6-bit)
static uint8_t _panel_pal_valid;
/* c537/c563's target: [0x48BC] is the 256x3 scratch ramp, and every writer
 * restores it to zero - clear_fade_ramp (1000:B90F) at boot, the level's
 * discovery op (1000:3A6A) on every exit (1000:3CF9/3D01), run_screen_
 * transition (1000:B82E), and menu_loop's 0xC0 only (1000:051C) - so the
 * panel's pre-scene fade and the key exit are fades to black. */
static const uint8_t _scene_black_pal[768];
/* [0x627C]: boot_intro_sequence's fast mode - its only writer (set at
 * 1000:B59F, cleared at 1000:B623, entry's caller only).  play_scene_sequence
 * reads it twice: the pre-frame palette fade is 4 entries div 2 instead of
 * 0x100 div 0x19 (1000:B263), and wait_flag_timeout waits 1 tick instead of
 * 6 per object (1000:B2D3).  The level's .ANI always runs with it clear. */
uint8_t _scene_fast;
static uint16_t _obj_wait;
static uint16_t _chain_wait;
static uint16_t _obj_index;

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void scene_set_fast_mode(uint8_t on) { _scene_fast = on ? 1u : 0u; }
#endif /* HUMANSRE_TEST_PROBES */

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
void _scene_palette_fade(const uint8_t *target)
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
    audio_fade_out(); // 1000:B166: present_frames_75 -> 1000:B169 stop
    _scene_phase = SCENE_QUIT_FADE;
}

int32_t scene_init(void) { return 0; }

/** @brief Any key or fire from the live poll: the original's [0x48A3] / +0x5ACE
 * bit-7 checks.  The boot's waits use the cached _boot_key_state instead.
 *
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _scene_key_held(void)
{
    InputState in;
    g_osys->eventsPoll(&in);
    return in.keyHeld != 0u || in.lastKey != 0u || (in.joyBits & 0x80u) != 0;
}

/**
 * @brief Release the current intro scene's assets.
 */
static void _scene_unload(void)
{
    g_osys->memFree(_ani.image);
    _ani.image = NULL;
    _ani.image_size = 0;
    g_osys->memFree(_ani.entries);
    _ani.entries = NULL;
    _ani.n_entries = 0;
    g_osys->memFree(_script_buf);
    _script_buf = NULL;
    _indices = NULL;
    _plays = 0;
    g_osys->memFree(_obj_buf);
    _obj_buf = NULL;
    g_osys->memFree(_scoops_buf);
    _scoops_buf = NULL;
    _scoops_size = 0;
    if (_text_spr) font_publish(NULL); // the glyph table dies with it
    g_osys->memFree(_text_spr);
    _text_spr = NULL;
    memset(&_ani, 0, sizeof _ani);
}

/** @brief Load ANI<index>.ANI + the background assets, draw the background and
 * the palette onto page 0.
 *
 * @param index the intro scene index
 * @param initial non-zero on the chain's first scene (shows the panel)
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

    _script_buf = g_osys->memAlloc(OBJ_SEG_SIZE);
    _obj_buf = g_osys->memAlloc(OBJ_SEG_SIZE);
    if (!_script_buf || !_obj_buf) {
        _scene_unload();
        return -1;
    }

    /* lbm\daily.bit -> the page: [64000-byte screen][768-byte palette] */
    {
        uint32_t dailySize = 0;
        uint8_t *daily = res_load_chunk(game_resources[GAME_RES_LBM_DAILY_BIT],
                                        &dailySize, NULL);
        uint8_t *page = video_page(0);
        if (!daily) {
            _scene_unload();
            return -1;
        }
        memcpy(page, daily, dailySize < PAGE_SIZE ? dailySize : PAGE_SIZE);
        /* 1000:B0AF (palette_load_6bit, 1000:8D24): daily.bit's 768 bytes
         * are masked to 6 bits and staged at 0x4BBC, NOT installed - the
         * panel's first 0x20 entries FADE in (1000:B0DA) */
        _panel_pal_valid = dailySize >= PAGE_SIZE + 768u;
        if (_panel_pal_valid)
            for (uint16_t i = 0; i < 768u; i++) // 1000:8D24's AND 0x3F
                _panel_pal[i] = daily[PAGE_SIZE + i] & 0x3Fu;
        g_osys->memFree(daily);
    }

    /* apl\text.spr (the original's second load, 1000:C490): the panel's
     * headline is drawn with these glyphs (blit_text_tile), so the scene
     * publishes them for its own frame */
    g_osys->memFree(_text_spr);
    _text_spr =
        res_load_chunk(game_resources[GAME_RES_APL_TEXT_SPR], NULL, NULL);
    font_publish(_text_spr);

    /* apl\scoops.apl -> the background block, overlaid onto the page */
    _scoops_buf = res_load_chunk(game_resources[GAME_RES_APL_SCOOPS_APL],
                                 &_scoops_size, NULL);
    if (_scoops_buf) {
        uint8_t *page = video_page(0);
        uint32_t srcOff = 0x400u + (uint32_t)_scene_bg_table[index] * 0xE00u;
        if (srcOff + 0xE00u <= _scoops_size) {
            /* 1000:B1CF: 56 rows of 64 bytes at 0x8270 (= row 104, x 112 of
             * the 320-wide panel).  The original's row step is the REP
             * MOVSB advance (0x40) PLUS its `ADD DI,0x100`, i.e. 0x140 - the
             * panel's 320-byte row; taking only the 0x100 shears the
             * thumbnail 64 px left per row. */
            const uint8_t *src = _scoops_buf + srcOff;
            for (uint16_t row = 0; row < 0x38u; row++) {
                memcpy(page + 0x8270u + row * VIDEO_PAGE_W, src, 0x40u);
                src += 0x40u;
            }
        }
        g_osys->memFree(_scoops_buf);
        _scoops_buf = NULL;
    }

    /* the panel's headline (1000:B0D4: blit_text_tile at centre 0xA0, row
     * 0x41, mask 0x10 - OR'd into the panel art, so it takes the panel's own
     * vertical ramp) - the index-th string of 202F:0x6304 */
    {
        const char *headline =
            game_strings[g_game_language][GAME_STRING_SCENE_HEADLINE_0 + index];

        if (headline[0])
            textpage_draw_line_mask(video_page(0), headline, 0x41u, 0x10u);
    }

    g_osys->videoPresent(0);

    /* 1000:B0DA: after the panel is drawn and flipped, its first 0x20 DAC
     * entries fade from the previous screen's palette to daily.bit's
     * (div 0x19); the 210-tick dwell starts only after that fade.  A chain
     * advance (C57A) skips the panel and its fade. */
    if (initial && _panel_pal_valid)
        palette_build_fade(0u, 0x20u, 0x19u, _panel_pal);
    return 0;
}

uint8_t scene_play_level_intro(uint16_t introIndex)
{
    /* cd_file_op's head (1000:AFFD): the ANIMATIONS board (cell 5,
     * [0x5E79]) non-zero RETs before any file op - cutscenes are off. */
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u) return 0;
    if (introIndex == 0xFFu) return 0; // no cutscene for this level

    if (introIndex != _scene_level) { // (re)start on level change
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
        _plays = (uint16_t)(rd_le_u16(_script_buf) + 1u);
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

    while (1) { // instant phases resolve within one tick
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
            if (keyHeld) _dwell_ticks = 0; // any key skips the remainder
            if (_dwell_ticks > 0) {
                --_dwell_ticks;
                return 1;
            }
            if (keyHeld) {
                _scene_quit_fade();
                return 1;
            }
            /* 1000:B13D: the panel's first 0x20 entries fade back to
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
                g_osys->videoPresent(0);
            }
            /* 1000:B263: now the DAC fades to the script object's palette at
             * 0x6FEA (= _script_buf+2): the whole 256 entries div 0x19 (or 4
             * entries div 2 when [0x627C] is set - the instant path), then
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
            break; // play the first object now

        case SCENE_PLAY:
            if (_obj_index >= _plays) {
                _scene_phase = SCENE_CHAIN;
                break;
            }
            {
                uint16_t chk = 0;
                int32_t sz = _ani_decode_object(&_ani, _indices[_obj_index],
                                                _obj_buf, OBJ_SEG_SIZE, &chk);
                if (sz < 0 || chk != 0) { // place_scene_obj: JZ / abort
                    _scene_phase = SCENE_CHAIN;
                    break;
                }
                _scene_al_expand(_obj_buf, (uint32_t)sz, video_page(0),
                                 PAGE_SIZE);
                g_osys->videoPresent(0);
                _obj_wait =
                    _scene_fast ? 1u : 6u; // wait_flag_timeout 1000:B2D3
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
            _cur_scene = nxt; // chain advance
            if (nxt == 4u) { // 1000:B194: wait first
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
                    _obj_index = 0; // replay this scene
                    _scene_phase = SCENE_PLAY;
                } else {
                    _scene_phase = SCENE_RESTART; // play the chained scene
                }
            }
            return 1;

        case SCENE_RESTART: // chain advance: no dwell
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
 * the RSC's u32 table (the same machine as res_decode_lzw_mem - the b444
 * checksum gate) and the object's RLE stream expands over the whole page
 * (blit_background_data 1000:B2EB - _scene_al_expand is that machine,
 * validated through the level-intro scenes).
 *
 * @param idx the group index
 * @return 1 while the group shows, 0 when it finishes
 */