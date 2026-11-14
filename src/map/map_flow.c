/* The MAP flow: menu_loop's phase machine, the fades and the key handling
 * (1000:012A..0x0650).
 */

#include "map/map.h"
#include "map/map_internal.h"

#include "audio.h"
#include "font.h"
#include "hiscore.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "game_resources.h"
#include "score.h"
#include "video.h"

#include <stdint.h>
#include <string.h>

#define ALPHA_WINDOW 0x2000u // 1000:0705's fixed 4096-word source window

enum {
    MAP_CONGRATS,
    MAP_NAME_FADE,
    MAP_NAME,
    MAP_ENTER, // 1000:049A..0x0516: the spin + white fade after Enter
    MAP_LIST_HEADER,
    MAP_LIST,
    MAP_EXIT, // 1000:062B: fade all 256 entries to black, hold, save
    MAP_EXIT_FADE
};
struct MapFade {
    uint16_t first, entries, div, target;
};

static uint8_t *_map_high;
static uint32_t _map_high_size;
static uint8_t *_map_font;
static uint8_t _map_up;
static uint8_t _map_armed;  /* the first tick ignores the key that skipped the
                           * ending cutscene */
static uint8_t _map_phase;
static uint8_t _map_name_len;
static uint8_t _map_col;
/* 1000:03FB/0x0405: the fire append waits for the ISR tick counter [0x5AC8]
 * to reach 0x23 and resets it; the name screen arms it at entry (0x030A). */
static uint16_t _map_fire_wait;
/* 1000:05F4/0613: a panel step is 0x14 frames, one [0x8AE] unit per frame. */
static uint8_t _map_step; // frames left in the current step
static int8_t _map_step_dir; // +1 = up (0x060C), -1 = down (0x05ED)
/* 1000:0251's MOV CX,2 loop: the congratulation page is drawn twice, with
 * flip_page_swap (1000:87C7) between.  The port presents once per tick, so
 * the main loop's present after each map_tick is the flip equivalent. */
#if defined(HUMANSRE_TEST_PROBES)
static uint8_t _map_congrats_draws; // read by the test seam only
#endif
/* 1000:028C..030A: the name-entry transition's fade/rectangle steps. */
static uint8_t _map_trans; // 0..6; 6 = the ring is up
#define MAP_FADE_LOG 16u
static uint8_t _map_fade_starts; // the palette_build_fade calls made
static struct MapFade _map_fade_log[MAP_FADE_LOG];
/* the 0x48BC ramp: zeroed = black, the opening's 0x0239 target. */
static const uint8_t _map_black[768];

/* 1000:021E..0x0285: the opening's fade/animate steps. */
#define MAP_INTRO_WAIT 4u
static uint8_t _map_intro;
/* 1000:0233's 0x8C palette_animate frames, one per tick (each presents). */
static uint8_t _map_intro_frames;
/* the top-ten list's 0x0518 header frames (the 15 outlines) before 0x0577. */
static uint8_t _map_list_step;
/* 1000:049A..0x0516: the Enter spin, the white ramp and the two fades. */
static uint8_t _map_enter_phase; // 0 = to white, 1 = back to working
static uint16_t _map_enter_bx; // the 0x04B9/0x0515 counter
static uint8_t _map_white[0xC0]; // the 0x48BC ramp during the spin
/* 1000:0577/0x0595: the offscreen list page (cleared, then the rows).  It is
 * the pool's head, [0x8A4] (menu_loop 1000:013B), a heap segment - not a VGA
 * page - so it is allocated with the screen and released by _map_close. */
uint8_t *_map_off;
/* 1000:063A/0x063D: the 0x4B-frame hold after the exit fade (present_frames_75). */
static uint8_t _map_exit_hold;
static uint8_t _map_music_fade;

/* 1000:048/0x04A: the ring's per-draw step. */
static uint8_t _map_angle_step;
static uint8_t _map_spin; // frames left in the entry spin (20 + 3)
/* 1000:03B5..0x03F8: a left/right press is four 3-degree draws plus the three
 * 0x0330 holds, not one 12-degree jump. */
static uint8_t _map_move_steps;
static int8_t _map_move_dir; // -1 = left (0x03DA), +1 = right (0x03B5)
static uint8_t _map_move_hold;
/* 1000:0368 clears [0x5ACE]: after a typed key the action bit is swallowed
 * until the key is released (Z is the shipped action key). */
static uint8_t _map_fire_lock;
/* [0x48A3]: the ISR's last-key latch; the menu takes it once, so a key that
 * lands during the spin/holds is not lost and a held key does not repeat. */
static uint8_t _map_pending_key;

/* [0x8A2]: 1..6, 0 while the entry animation runs; [0x8AE]: the frame band's
 * source row (1000:0518's 0x130 down to 2, then 0x14 steps per panel). */
static uint16_t _map_panel;
static uint16_t _map_box_bottom;

/** @brief 1000:040B: the ring cell at an angle, `(0x10E-angle)/0x0C`.
 *
 * @return the cell index (0..29)
 */
static uint8_t _map_col_from_angle(void);

/**
 * @brief Accessor: the running score as one 32-bit value.
 *
 * @return the score
 */
static uint32_t _map_score(void)
{ return ((uint32_t)score_total_hi() << 16) | score_total_lo(); }
#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint8_t map_transition_step(void) { return _map_trans; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint8_t map_fade_starts(void) { return _map_fade_starts; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
int32_t map_fade_at(uint32_t i, uint32_t *first, uint32_t *entries,
                    uint32_t *div, uint32_t *target)
{
    if (i >= _map_fade_starts || i >= MAP_FADE_LOG) return 0;
    if (first) *first = _map_fade_log[i].first;
    if (entries) *entries = _map_fade_log[i].entries;
    if (div) *div = _map_fade_log[i].div;
    if (target) *target = _map_fade_log[i].target;
    return 1;
}
#endif /* HUMANSRE_TEST_PROBES */

/** @brief Build a fade to an arbitrary target and log it.
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param target the target palette
 * @param targetId the logged target id
 */
static void _map_build_fade_to(uint16_t first, uint16_t entries, uint16_t div,
                               const uint8_t *target, uint16_t targetId)
{
    palette_build_fade(first, entries, div, target);
    if (_map_fade_starts < MAP_FADE_LOG) {
        _map_fade_log[_map_fade_starts].first = first;
        _map_fade_log[_map_fade_starts].entries = entries;
        _map_fade_log[_map_fade_starts].div = div;
        _map_fade_log[_map_fade_starts].target = targetId;
    }
    _map_fade_starts++;
}

/** @brief 1000:0273/028C/029B/02AD/02BF/02F5/0307: palette_build_fade with the
 * working palette's offset as the target (0x4BBC + N).
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param targetOff the target offset
 */
static void _map_build_fade(uint16_t first, uint16_t entries, uint16_t div,
                            uint16_t targetOff)
{
    _map_build_fade_to(first, entries, div, palette_working() + targetOff,
                       targetOff);
}

static void _map_page_copy(uint8_t *page)
{
    if (!page) return;
    memset(page, 0, MAP_PAGE);
    if (_map_high)
        memcpy(page, _map_high,
               _map_high_size < MAP_PAGE ? _map_high_size : MAP_PAGE);
}

/** @brief 1000:02C5: HIGH.BIT plus the band strips' palette reset.
 *
 * @param page the page buffer
 */
static void _map_page_reset(uint8_t *page)
{
    _map_page_copy(page);
    /* 1000:0221/0x02E6: the band strips sit under the art that follows. */
    if (_map_stage_ready) map_palette_reset();
}

/** @brief the whole screen: high.bit's page, then the state's own art and the
 * box frame (1000:07E6).
 */
static void _map_redraw(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    if (_map_phase == MAP_NAME || _map_phase == MAP_ENTER) map_draw_name(page);
    else if (_map_phase == MAP_LIST) {
        /* 1000:05A5/05F8/0617: only the frame bands; the rows stay in the
         * offscreen page and the display keeps high.bit + the header. */
        map_draw_box_edges(page, _map_box_bottom);
    }
}

/**
 * @brief One left/right animation tick: four 3-degree draws, then the three
 * 0x0330 holds (1000:03B5..0x03F8).
 *
 * @return 1 when a tick was consumed, 0 when the animation is idle
 */
static int32_t _map_move_tick(void)
{
    if (_map_move_steps > 0) {
        int32_t a = (int32_t)_map_angle + _map_move_dir * 3 + 0x168;

        _map_angle = (uint16_t)(a % 0x168u);
        _map_col = _map_col_from_angle();
        _map_redraw();
        _map_move_steps = (uint8_t)(_map_move_steps - 1u);
        if (_map_move_steps == 0u) _map_move_hold = 3;
        return 1;
    }
    if (_map_move_hold > 0) {
        _map_redraw(); // 1000:0330
        _map_move_hold--;
        return 1;
    }
    return 0;
}

uint8_t map_begin(void)
{
    if (_map_up) return 1;

    res_set_disc(0u); // entry 0x151A [0xF7E]=0
    _map_high = res_load_chunk(game_resource_name(GAME_RES_LBM_HIGH_BIT),
                               &_map_high_size, NULL);
    uint8_t *page = video_page(0);
    if (_map_high && _map_high_size >= MAP_PAGE + 768u)
        palette_load_working(_map_high + MAP_PAGE); // 1000:01C3/8D24

    _map_font =
        res_load_chunk(game_resource_name(GAME_RES_APL_TEXT_SPR), NULL, NULL);
    font_publish(_map_font);

    /* 1000:01A2..0x01AB: apl\alpha.spr expanded 4bpp -> four planes */
    g_osys->memFree(_alpha_packed);
    _alpha_packed = NULL;
    g_osys->memFree(_alpha_sheet);
    _alpha_sheet = NULL;
    {
        uint32_t asz = 0;

        _alpha_packed = res_load_chunk(
            game_resource_name(GAME_RES_APL_ALPHA_SPR), &asz, NULL);
        /* 1000:0705 transposes a fixed 4096-word (8192-B) window.  The
         * shipped file is 8128 B, so the window's 64-byte tail is zeroed
         * rather than read past the chunk (the original's tail is whatever
         * follows ALPHA in its pool; the cells that would use it are never
         * drawn). */
        if (_alpha_packed && asz < ALPHA_WINDOW) {
            uint8_t *win = g_osys->memAlloc(ALPHA_WINDOW);

            if (!win) {
                g_osys->memFree(_alpha_packed);
                _alpha_packed = NULL;
            } else {
                memset(win, 0, ALPHA_WINDOW);
                memcpy(win, _alpha_packed, asz);
                g_osys->memFree(_alpha_packed);
                _alpha_packed = win;
            }
        }
    }
    if (_alpha_packed) {
        _alpha_sheet = g_osys->memAlloc(65536u);
        if (_alpha_sheet) alpha_expand(_alpha_packed, _alpha_sheet);
    }
    /* 1000:01B7: apl\band.spr, the 304 x 80 sheet */
    g_osys->memFree(_band_sheet);
    _band_sheet = NULL;
    _band_sheet =
        res_load_chunk(game_resource_name(GAME_RES_APL_BAND_SPR), NULL, NULL);
    _map_stage_ready = 0;
    _map_stage_fill(); // 1000:12D3

    /* 1000:01CD: the running total is offered to the table.  A qualifying one
     * opens the name entry first, then the list. */
    _map_name[0] = 0;
    _map_name_len = 0u;
    _map_col = 0u;
    _map_angle = 0x10Eu;
    _map_angle_step = 0u;
    _map_spin = 0;
    _map_move_steps = 0;
    _map_move_dir = 0;
    _map_move_hold = 0;
    _map_fire_lock = 0;
    _map_pending_key = 0;
    memset(_map_glyphs, 0, sizeof _map_glyphs);
    _map_dirty_reset();
    _map_fade_starts = 0u;
    _map_intro = 0u;
    _map_intro_frames = 0u;
    _map_list_step = 0u;
    _map_enter_phase = 0u;
    _map_enter_bx = 0u;
    _map_exit_hold = 0u;
    g_osys->memFree(_map_off);
    _map_off = g_osys->memAlloc(MAP_PAGE);
    if (!_map_off) return 0;
    memset(_map_off, 0, MAP_PAGE);
    _map_box_bottom = 0x130u; // 1000:0518's [0x8AE]
    _map_panel = 0u; // 0 = the entry animation
    _map_step = 0;
    _map_step_dir = 0;
    _map_phase = hiscore_qualifies(_map_score()) ? MAP_CONGRATS : MAP_EXIT;
#if defined(HUMANSRE_TEST_PROBES)
    _map_congrats_draws = 0u;
#endif
    if (_map_phase == MAP_CONGRATS) {
        /* 1000:018D-0196: play_tune("mus\hiscore") / music_resume /
         * timing_loop for the congrats + name entry. */
        audio_play_tune(audio_tune_name(AUDIO_TUNE_HISCORE));
        audio_music_resume();
        audio_fade_in();
        _map_music_fade = 1; // 1000:018D-0196's timing_loop
        /* 1000:021E..0x0233: high.bit, the strips, then the band's 0x10..0x1F
         * colours fade in (the rest of the DAC is still the previous page's). */
        _map_page_copy(page);
        map_palette_reset(); // 0x0221
        _map_build_fade(0x10u, 0x10u, 0x1Eu, 0x30u); // 0x0224
    }

    _map_up = 1;
    _map_armed = 0;
    return 1;
}

static uint8_t _map_col_from_angle(void)
{ return (((0x10Eu + 0x168u - _map_angle) % 0x168u) / 0x0Cu); }

/** @brief 1000:0518's header: high.bit, the 15 outlines and the title.  The
 * page is cleared at 0x0577 on the next tick before the rows are drawn.
 */
static void _map_list_begin(void)
{
    uint8_t *page = video_page(0);

    _map_phase = MAP_LIST_HEADER;
    _map_list_step = 0u;
    _map_enter_phase = 2u; // the test seam: the Enter spin is over
    if (!page) return;
    _map_page_copy(page); // 0x0526
    map_draw_list_header(page); // 0x052F..0x054E
    if (_map_stage_ready) map_palette_reset(); // 0x053B
    /* 0x055E: the working palette is reloaded from high.bit, then 0x0568
     * fades 0x20..0x5F to it before the rows' box unroll starts. */
    if (_map_high && _map_high_size >= MAP_PAGE + 768u)
        palette_load_working(_map_high + MAP_PAGE);
    _map_build_fade(0x20u, 0x40u, 0x1Eu, 0x60u); // 0x0568
}

/** @brief 1000:049A..0x0516: the Enter spin, white ramp and two fades.
 *
 * `0x049A` fills the 0x48BC ramp with 0x3F (white) and `0x04B4` fades palette
 * 0x20..0x5F to it; `0x04BC` spins the wheel until that fade ends.  `0x04EA`
 * then copies working[0x60..0x7F] into the ramp twice and `0x050E` fades
 * 0x20..0x5F back to the working palette; the spin continues until it ends,
 * then `0x0518` draws the header.
 */
static void _map_enter_begin(void)
{
    memset(_map_white, 0x3Fu, sizeof _map_white); // 0x049A
    _map_build_fade_to(0x20u, 0x40u, 0x64u, _map_white, 0xFFFFu); // 0x04B4
    _map_enter_phase = 0u;
    _map_enter_bx = 1u;
    _map_phase = MAP_ENTER;
}

/** @brief 1000:062B: fade all 256 DAC entries to the zeroed 0x48BC ramp.
 *
 * `0x062B` targets `0x48BC` with `BP=0x23` and `CX=0x100`; `0x063A` waits for it, `0x063D`
 * presents 0x4B frames (`present_frames_75`), then 0x8389 stops the music and `0x06CC` saves.
 */
static void _map_exit_begin(void)
{
    _map_build_fade_to(0u, 0x100u, 0x23u, _map_black, 0xFFFFu); // 0x062B
    _map_exit_hold = 0x4Bu; // 0x063D
    audio_fade_out(); // 0x063D: present_frames_75
    _map_phase = MAP_EXIT_FADE;
}

/** @brief 1000:063D..0x0650: save the table and release the map assets. */
static void _map_close(void)
{
    _map_up = 0;
    audio_music_stop(); // 0x0640
    hiscore_write(); // 0x06CC
    g_osys->memFree(_map_high);
    _map_high = NULL;
    g_osys->memFree(_map_font);
    _map_font = NULL;
    g_osys->memFree(_alpha_packed);
    _alpha_packed = NULL;
    g_osys->memFree(_alpha_sheet);
    _alpha_sheet = NULL;
    g_osys->memFree(_band_sheet);
    _band_sheet = NULL;
    g_osys->memFree(_map_off); // the [0x8A4] pool head from map_begin
    _map_off = NULL;
    _map_stage_ready = 0;
    video_arena_release(); // the list/header text arena
    font_publish(NULL);
}

/**
 * @brief 1000:040B: one grid column applied to the 13-character buffer.
 *
 * @param column the column
 * @return 1 when Enter committed and the list header is up
 */
static int32_t _map_apply(uint8_t column)
{
    uint8_t n0 = _map_name_len;

    if (hiscore_name_apply(_map_name, column, &_map_name_len)) {
        /* 0x1D: commit the 14-byte name and insert, then 0x049A's spin. */
        hiscore_insert(_map_score(), _map_name);
        _map_enter_begin();
        return 1;
    }
    if (column != 0x1Cu) {
        /* 1000:0452..0x0459: the new glyph starts at (0x95,0x16). */
        uint8_t i = n0 < NAME_MAX ? n0 : NAME_MAX - 1u;

        _map_glyphs[i].cx = 0x95;
        _map_glyphs[i].cy = 0x16;
    }
    return 0;
}

uint8_t map_tick(const InputState *in)
{
    if (!_map_up) return 0;
    if (!_map_armed) {
        _map_armed = 1;
        return 1;
    }
    if (_map_music_fade) { // the congrats trio's fade-in blocks
        if (audio_fade_busy()) return 1;
        _map_music_fade = 0;
    }
    _map_palette_animate(); // 1000:09DA, every menu-loop iteration

    if (_map_phase == MAP_EXIT) {
        _map_exit_begin(); // 1000:062B: fade/save, no list
        return 1;
    }
    if (_map_phase == MAP_EXIT_FADE) {
        if (palette_fade_active()) return 1; // 0x062B..0x063A
        if (_map_exit_hold > 0u) { // 0x063D present_frames_75
            _map_exit_hold--;
            return 1;
        }
        if (audio_fade_busy()) return 1; // 0x063D's blocking fade
        _map_close(); // 0x06CC
        return 0;
    }
    if (_map_phase == MAP_CONGRATS) {
        /* 1000:021E..0x0285: high.bit + palette_reset + the band's 0x10..0x1F
         * fade (built in map_begin), 0x8C animates, the 0x20..0x3F fade to
         * black, high.bit again, the text twice, then the 0..0x3F fade in. */
        switch (_map_intro) {
        case 0:
            /* 1000:0233: 0x8C palette_animate frames, one present per tick.
             * The 0x0224 fade (0x10..0x1F) steps inside them, so the band
             * strips fade in while the stone table is still black. */
            _map_intro_frames++;
            if (_map_intro_frames < 0x8Cu) return 1;
            _map_build_fade_to(0x20u, 0x20u, 2u, _map_black,
                               0xFFFFu); // 0x0239
            _map_intro = 1u;
            return 1;
        case 1: {
            uint8_t *page = video_page(0);

            if (palette_fade_active()) return 1;
            _map_page_copy(page); // 0x024B
            _map_draw_strips(page); // the display strips survive the copy
            map_draw_congrats(page); // 0x024E, first pass
#if defined(HUMANSRE_TEST_PROBES)
            _map_congrats_draws = 1u;
#endif
            _map_intro = 2u;
            return 1;
        }
        case 2: {
            uint8_t *page = video_page(0);

            map_draw_congrats(page); // 0x0251's second pass after the flip
#if defined(HUMANSRE_TEST_PROBES)
            _map_congrats_draws = 2u;
#endif
            _map_build_fade(0u, 0x40u, 0x28u, 0u); // 0x0273
            _map_intro = 3u;
            return 1;
        }
        case 3:
            if (palette_fade_active()) return 1;
            _map_intro = MAP_INTRO_WAIT;
            return 1;
        default: break;
        }
        if (in->lastKey || (in->joyBits & JOYBITS_FIRE)) {
            _map_phase = MAP_NAME_FADE; // 1000:0285: the key wait
            _map_trans = 0;
        }
        return 1;
    }
    if (_map_phase == MAP_NAME_FADE) {
        /* The order is 1000:028C..030A: wait the congrats fade, then three
         * fades, the HIGH.BIT re-copy + two rectangles, then two more fades. */
        switch (_map_trans) {
        case 0:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x80u, 0x80u, 2u, 0x180u); // 1000:028F
            _map_trans = 1;
            return 1;
        case 1:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x20u, 0x20u, 0x1Eu, 0x120u); // 1000:02A1
            _map_trans = 2;
            return 1;
        case 2:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x40u, 0x20u, 2u, 0x120u); // 1000:02B3
            _map_trans = 3;
            return 1;
        case 3: {
            uint8_t *page = video_page(0);

            if (palette_fade_active()) return 1;
            if (page) {
                _map_page_reset(page); // 1000:02C5
                map_draw_rect(page, 0x95u, 9u, 0xFu, 0xFu); // 1000:02C8
                map_draw_rect(page, 0x60u, 0x45u, 0x77u, 0xFu); // 1000:02D7
            }
            _map_build_fade(0x40u, 0x20u, 0x1Eu, 0xC0u); // 1000:02E9
            _map_trans = 4;
            return 1;
        }
        case 4:
            if (palette_fade_active()) return 1;
            /* 1000:02FB/030A: the last fade starts and the ring goes up
             * without waiting for it ([0x5AC8] = 0x23 and the entry spin). */
            _map_build_fade(0x20u, 0x20u, 0x46u, 0x60u);
            _map_trans = 6;
            _map_phase = MAP_NAME;
            _map_fire_wait = 0x23u; // 1000:030A: [0x5AC8] := 0x23
            _map_angle = 0x10Eu; // 1000:0310
            _map_angle_step = 0x12u; // 1000:0316
            _map_spin = 23; // 20 stepped draws + the three 0x0330 draws
            _map_col = 0u;
            _map_move_steps = 0;
            _map_move_hold = 0;
            _map_fire_lock = 0;
            _map_pending_key = 0;
            _map_dirty_reset();
            return 1;
        default:
            _map_phase = MAP_NAME;
            _map_fire_wait = 0x23u;
            _map_angle = 0x10Eu;
            _map_angle_step = 0x12u;
            _map_spin = 23;
            _map_move_steps = 0;
            _map_move_hold = 0;
            _map_fire_lock = 0;
            _map_pending_key = 0;
            return 1;
        }
    }

    if (_map_phase == MAP_NAME) {
        if (_map_fire_wait < 0xFFFFu) _map_fire_wait++; // the [0x5AC8] tick
        if (in->lastKey) _map_pending_key = in->lastKey; // [0x48A3] latch

        /* 1000:031C..0x0342: the ring spins one full revolution (20 x 0x12)
         * and then holds for the three 0x0330 draws. */
        if (_map_spin > 0) {
            _map_redraw();
            if (_map_spin > 3) {
                _map_angle = (_map_angle + _map_angle_step) % 0x168u; // 0x0DF3
            } else {
                _map_angle_step = 0u; // 1000:032A
            }
            _map_spin--;
            _map_col = _map_col_from_angle();
            return 1;
        }
        if (_map_move_tick()) return 1; // four 3-degree draws + three holds

        if (!(in->joyBits & JOYBITS_FIRE)) _map_fire_lock = 0;

        uint8_t committed = 0;

        if (in->joyBits & JOYBITS_LEFT) { // left: 1000:03DA, [0x48] -= 3 x4
            _map_move_dir = -1;
            _map_move_steps = 4;
            return (uint8_t)(_map_move_tick());
        }
        if (in->joyBits & JOYBITS_RIGHT) { // right: 1000:03B5, [0x48] += 3 x4
            _map_move_dir = 1;
            _map_move_steps = 4;
            return (uint8_t)(_map_move_tick());
        }
        if (_map_pending_key) {
            uint8_t c = _map_pending_key;
            uint8_t cell = 0xFFu;

            _map_pending_key = 0u; // 1000:0363 [0x48A3] = 0
            if (c >= 'a' && c <= 'z') c -= 32u;
            if (c >= 'A' && c <= 'Z') cell = c - 'A';
            else if (c == ' ') cell = 0x1Au;
            else if (c == '.') cell = 0x1Bu;
            else if (c == 8u || c == 0x7Fu) cell = 0x1Cu; // delete
            else if (c == 13u || c == 10u) cell = 0x1Du; // enter
            _map_fire_lock = 1; // 1000:0368 [0x5ACE] = 0
            if (cell != 0xFFu) {
                /* 1000:039C..0x03B3: the key turns the ring to the cell and
                 * then falls into the shared append at 0x040B (the typed
                 * path bypasses the fire debounce). */
                _map_col = cell;
                _map_angle = (0x10Eu + 0x168u - cell * 0x0Cu) % 0x168u;
                committed = (uint8_t)(_map_apply(cell));
            }
        } else if ((in->joyBits & JOYBITS_FIRE) && !_map_fire_lock &&
                   _map_fire_wait >= 0x23u) {
            /* 1000:03FB CMP [0x5AC8],0x23 / 0x0405 reset */
            _map_fire_wait = 0u;
            committed = (uint8_t)(_map_apply(_map_col));
        }
        if (committed) return 1;
        _map_redraw(); // 1000:033C
        return 1;
    }

    if (_map_phase == MAP_ENTER) {
        /* 1000:04BC: step = min(BX/8, 0x10), the ring turns left and the
         * 0x0224-style present happens every frame. */
        uint8_t step = (uint8_t)(_map_enter_bx / 8u);

        if (step > 0x10u) step = 0x10u;
        _map_angle = (_map_angle + 0x168u - step) % 0x168u;
        _map_col = _map_col_from_angle();
        _map_redraw(); // 0x0CBE
        if (palette_fade_active()) {
            _map_enter_bx++;
            return 1;
        }
        if (_map_enter_phase == 0u) {
            /* 0x04EA: the 0x60-byte working[0x60..0x7F] block, twice */
            memcpy(_map_white, palette_working() + 0x120u, 0x60u);
            memcpy(_map_white + 0x60u, palette_working() + 0x120u, 0x60u);
            _map_build_fade_to(0x20u, 0x40u, 0x3Cu, _map_white,
                               0xFFFFu); // 0x050E
            _map_enter_phase = 1u;
            _map_enter_bx++;
            return 1;
        }
        _map_list_begin(); // 0x0518
        return 1;
    }

    if (_map_phase == MAP_LIST_HEADER) {
        /* 0x053E/0x0558: the title's two draws; 0x0577/0x0595: the rows into
         * the offscreen page; 0x0598: wait for the 0x0568 fade; 0x059B: the
         * 0x12E-frame box unroll. */
        _map_list_step++;
        if (_map_list_step <= 2u) return 1;
        if (_map_list_step == 3u) {
            map_draw_list(_map_off); // 0x0577 + 0x0595
            return 1;
        }
        if (palette_fade_active()) return 1; // 0x0598
        _map_box_bottom = 0x130u; // 0x059B
        _map_panel = 0u;
        _map_step = 0;
        _map_step_dir = 0;
        _map_phase = MAP_LIST;
        return 1;
    }

    /* 1000:0518's entry animation: [0x8AE] runs from 0x130 down to 2, one step
     * per frame, so the five frame rows unroll; [0x8A2] becomes 1 when it ends
     * and the panel loop (0x05B9) takes over. */
    if (_map_panel == 0u) {
        _map_redraw(); // 1000:05A5: draw, wait, then SUB
        if (_map_box_bottom > 2u) _map_box_bottom--;
        else _map_panel = 1u;
        return 1;
    }
    /* 1000:05F4/0613: a panel step is 0x14 frames long, not instant.  Each
     * frame draws the frame and moves [0x8AE] by one; [0x8A2] changes only
     * after the 0x14th.  Up (bit 0x01) adds 1, down (bit 0x02) subtracts 1. */
    if (_map_step > 0) {
        _map_redraw(); // 1000:05F8: draw, wait, then ADD/SUB
        _map_box_bottom = (uint16_t)((int32_t)_map_box_bottom + _map_step_dir);
        _map_step = (uint8_t)(_map_step - 1u);
        if (_map_step == 0u)
            _map_panel = (uint16_t)((int32_t)_map_panel + _map_step_dir);
        return 1;
    }
    if (in->joyBits & JOYBITS_UP) {
        if (_map_panel < 6u) {
            _map_step = 0x14;
            _map_step_dir = 1;
        }
        _map_redraw();
        return 1;
    }
    if (in->joyBits & JOYBITS_DOWN) {
        if (_map_panel > 1u) {
            _map_step = 0x14;
            _map_step_dir = -1;
        }
        _map_redraw();
        return 1;
    }

    if (in->lastKey || (in->joyBits & JOYBITS_FIRE)) {
        _map_exit_begin(); // 1000:062B: fade all 256 entries, then save
        return 1;
    }
    return 1;
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
uint8_t map_congrats_draws(void) { return _map_congrats_draws; }

uint16_t map_panel_for_probe(void) { return _map_panel; }

uint16_t map_box_bottom_for_probe(void) { return _map_box_bottom; }

uint8_t map_name_col_for_probe(void) { return _map_col; }

uint16_t map_name_fire_wait_for_probe(void) { return _map_fire_wait; }

uint8_t map_intro_step_for_probe(void) { return _map_intro; }

uint8_t map_enter_step_for_probe(void) { return _map_enter_phase; }

uint16_t map_enter_bx_for_probe(void) { return _map_enter_bx; }

uint8_t *map_off_page_for_probe(void) { return _map_off; }
#endif /* HUMANSRE_TEST_PROBES */
