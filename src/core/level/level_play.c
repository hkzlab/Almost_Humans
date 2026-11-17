#include "entity.h"
#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "osystem.h"
#include "palette.h"
#include "statusbar.h"
#include "video.h"

#include <stdint.h>
#include <string.h>

static uint8_t _start_phase;
static uint8_t _warmup_left;
/* the fade-to-black target: 0x48BC, the ramp the boot leaves zeroed */
static const uint8_t _black_pal[768];
static const uint8_t _zero_pal[768]; // the zeroed 0x14D3 (1000:3170)
/* [0xC]: update_world's (1000:80AD) gate.  level_fade_in sets it at 1000:336C,
 * before its eight frames; the claim clears it (1000:3B19) and restores it
 * (1000:3CAB), so the _world is frozen while the claim's blocking sequence
 * runs.  Zero until the level's own chain reaches that store. */
static uint8_t _world_updates;
/* [0x5ACE]: this tick's input word, the level's per-tick render being
 * draw_map_digit (1000:4F22) + draw_terrain (1000:4FEF) over the 512x320
 * screen buffer: tile ids >= 0xA9 are empty, and a zero tile byte is not
 * stored, so the BG shows through the transparent pixels. */
static uint8_t _level_joy;

void level_set_updates(uint8_t on) { _world_updates = on ? 1 : 0; }

uint8_t level_updates(void) { return _world_updates; }

void level_begin(void)
{
    if (!_level_is_loaded() || _start_phase != LVSTART_IDLE) return;
    /* level_video_init's two component fades, in its own order:
     *   32EE/32F1  two frames of wait (CALL 8B56 twice)
     *   32F4       all 0x100 entries to 0x14D3, div 2, then 8B8B + one frame
     *   3309       entries 0xC0..0xFF to 0x1713, div 2, then 8B8B + one frame
     *   then level_fade_in (0x3334): entries 0..0xBF to 0x48BC, div 0x19.
     * Each wait is one display period; the phase switch below walks them. */
    /* The pre-level screen fades itself to black (the party's 1000:2711,
     * 0x100 entries div 0x23).  In the original level_video_init's fades start
     * from that black; entering the level state before it has finished must
     * not override it - palette_build_fade replaces the ramp, so overriding
     * recoloured the still-lit party art in the level's palette.  Wait here
     * (the caller retries next tick) and let it finish. */
    if (palette_fade_active()) return;
    /* 1000:32D6 (level_video_init -> reset_display_mode -> clear_vram
     * 1000:833A): the whole A000 window is zeroed BEFORE the 32F4/3309
     * fades, so the pre-level page - the front-end room on a save load,
     * the party screen on a fresh start - never shows through them.
     * Both port pages model the one cleared VRAM window. */
    {
        uint8_t *p0 = g_osys->videoPage(0);
        uint8_t *p1 = g_osys->videoPage(1);

        if (p0) memset(p0, 0, VIDEO_PAGE_BYTES);
        if (p1) memset(p1, 0, VIDEO_PAGE_BYTES);
    }
    /* [0x5B12] is level_fade_in's gate and is 0 until the chain completes, so
     * the bar's countdown cannot run during the fades (1000:95B5 reseeds the
     * clock but leaves the gate clear). */
    statusbar_set_active(0);
    /* 1000:32F4: all 0x100 entries to 0x14D3, which 1000:3170 (level_setup)
     * has just zeroed - a fade to BLACK over 2 steps.  level_run (1000:3054)
     * puts it BEFORE the loading screen (level_video_init 1000:3083,
     * load_sprite_pal 1000:3086). */
    palette_build_fade(0u, 0x100u, 2u, _zero_pal); // 1000:32F4
    /* the chain's fades draw the bar before the loading screen sets the
     * mode, so the field is set here too (level_loading_begin repeats it
     * after its mode set) */
    video_set_field(level_field_rows());
    _start_phase = LVSTART_VIDEO_BASE;
}

uint8_t level_starting(void)
{ return _level_is_loaded() && _start_phase != LVSTART_PLAYING; }

void level_note_input(const InputState *in)
{ _level_joy = in ? in->joyBits : 0u; }

/** @brief The A000 window -> the page: one field-width pixel row per plane
 * row.  The flat model makes a row's four pixel bytes contiguous, so each row
 * is a straight copy. */
void _level_page_present(void)
{
    uint8_t *page = video_page(0);
    uint32_t w = video_page_w();
    uint32_t pitch = level_plane_pitch();

    if (!page) return;
    for (uint32_t y = 0; y < video_page_h(); y++)
        memcpy(page + y * w, video_vram_group((uint16_t)(y * pitch)), w);
}

/* Bake the visible page back into the planar work buffer. */
void _level_page_bake(void)
{
    const uint8_t *page = video_page(0);
    uint32_t w = video_page_w();
    uint32_t pitch = level_plane_pitch();

    if (!page) return;
    for (uint32_t y = 0; y < video_page_h(); y++)
        memcpy(video_vram_group((uint16_t)(y * pitch)), page + y * w, w);
}

/** @brief The per-frame world pass: present_and_blit (1000:A0E1).
 *
 * Copies the prebuilt `_world` window into the page with blit_bitmap_planes
 * (1000:4C4E), then runs render_objects (1000:5D56) and the bar.  The level
 * bitmap is drawn once, at load (`_level_world_build`).  The original's
 * blit_bitmap_call (1000:4C28) draws it into BOTH pages (DI = 0x1200 then
 * 0x8100) before the eight update_world frames; the flat port has one page,
 * so one pass is that pre-draw and the same function serves the frames after.
 */
static void _render_world(void)
{
    /* 1000:3D5E's four-plane presented frame, as a flat compose: the visible
     * window is the _world at the display window ([0x0], [0x2] - see
     * `level_follow_view`), written through the chained page and read back.
     * ([0x1AEA]/[0x1AEC] are 1000:3DC0's *strip save* coordinate, not the
     * window: that routine copies a 64x18-byte page strip to A000:0xF000 and
     * never scrolls.) */
    uint16_t vx = level_view_x(), vy = level_view_y();
    const uint8_t *world = _level_world();
    uint32_t w = video_page_w();
    uint32_t pitch = level_plane_pitch();
    uint32_t margin = level_view_margin();
    uint32_t cols = w - 2u * margin;
    int32_t scrollX = level_view_scroll_x();

    for (uint32_t y = 0; y < video_page_h(); y++) {
        uint8_t *dst = video_vram_group((uint16_t)(y * pitch));
        uint32_t wy = (uint32_t)vy + y;

        memset(dst, 0, w);
        if (wy < WORLD_H && vx < WORLD_W) {
            uint32_t n = WORLD_W - vx;

            if (n > cols) n = cols;
            memcpy(dst + margin, world + wy * WORLD_W + vx, n);
        }
    }
    _level_spark_frame();
    uint8_t *page = video_page(0);
    _level_page_present();

    /* the object pass (render_objects 1000:5D56): the placed records' base
     * sprites over the terrain, before the bar */
    object_render(page, scrollX, (int32_t)vy);

    /* the entity pass (render_objects 1000:5D56: 0x6421 then 0x5823) */
    entity_render(page, scrollX, (int32_t)vy);

    /* update_object_list's tail: the two TER.DAT slots (1000:5C2F), off on
     * shipped data because 1000:4CAB disables them */
    object_render_ter(page, scrollX, (int32_t)vy);

    /* render_objects' tail (1000:5F54): the 0xA9 background sprites */
    level_draw_bg_sprites(page, scrollX, (int32_t)vy);

    /* the bar over the _world rows (the original's band lives below the
     * 192-row visible window via the line compare; the flat page redraws
     * band + marks per tick instead) */
    statusbar_set_surface(NULL, 0u, 0u, 0u, level_bar_row());
    statusbar_tick();
}

void level_redraw_page(void)
{
    if (!_level_is_loaded()) return;
    _render_world();
}

void level_center_page(void)
{
    uint8_t *page = video_page(0);
    uint32_t w = video_page_w();
    uint32_t h = video_page_h();
    uint32_t pad = h > WORLD_H ? (h - WORLD_H) / 2u : 0u;

    if (!page || !pad) return;
    memmove(page + pad * w, page, WORLD_H * w);
    memset(page, 0, pad * w);
    memset(page + (pad + WORLD_H) * w, 0, (h - pad - WORLD_H) * w);
}

void level_end_redraw_page(void)
{
    if (!_level_is_loaded()) return;
    if (level_view_y() > 0x78u)
        level_set_view(level_view_x(),
                       level_view_max_y() < 0x78u ? level_view_max_y() : 0x78u);
    _render_world();
}

uint8_t _level_phase(void) { return _start_phase; }

void _level_phase_reset(void) { _start_phase = LVSTART_IDLE; }

uint8_t level_tick(void)
{
    if (!_level_is_loaded()) return 0;

    /* 1000:34E6: the exit hold - the _world is frozen and the last page is
     * presented until the 15 waits expire. */
    if (_level_exit_hold_step()) return 1;

    if (!_level_world()) return 1;

    /* level_fade_in (1000:3334) - the chain: the fade-out waits for its
     * steps, then the eight update_world (1000:80AD) frames, then the final
     * 256-entry fade to 0x14D3 (div 0x32), then the hook is installed (the
     * bar's ticking gate [0x5B12] := 0xFF).  The _world is kept drawn behind
     * each phase. */
    /* The play loop leaves at 1000:34E6 and never calls update_world
     * (1000:80AD) again; the end caption (draw_level_text 0x33A3) only
     * renders the frozen _world.  `_level_end_requested` is that exit. */
    uint8_t doUpdate = (_start_phase == LVSTART_PLAYING &&
                        (!_level_end_requested() || level_goal_pan_running()))
                           ? 1
                           : 0;

    switch (_start_phase) {
    case LVSTART_IDLE: level_begin(); break;
    case LVSTART_VIDEO_BASE:
        if (!palette_fade_active()) {
            /* 1000:3309: the `0xC0..0xFF` block to 0x1713, filled at 1000:32CA
             * with panel4's palette (1000:4BBC) - the bar's colours. */
            palette_build_fade(0xC0u, 0x40u, 2u, statusbar_palette());
            _start_phase = LVSTART_VIDEO_BAR;
        }
        break;
    case LVSTART_VIDEO_BAR:
        if (!palette_fade_active())
            _start_phase = LVSTART_LOADING; // 1000:3086 runs the loading
        break;
    case LVSTART_LOADING:
        if (level_loading_screen()) break; // 1000:1DC0's screen is up
        /* 1000:3334: the loading art fades to black (0..0xBF -> 0x48BC, div 0x19) */
        palette_build_fade(0u, 0xC0u, 0x19u, _black_pal);
        _start_phase = LVSTART_FADE_OUT;
        break;
    case LVSTART_FADE_OUT:
        if (!palette_fade_active()) {
            /* 1000:3334's order after the fade-out: the scroll seeds (set in
             * the LOADING case above), slot 8's init, then 1000:4C28
             * blit_bitmap_call - the _world drawn into BOTH pages (DI = 0x1200
             * then 0x8100) before the eight update_world frames.  The port has
             * one page, so its first WARMUP tick is that pre-draw. */
            /* 1000:3346/334C/334F: the display window's seeds - the eight
             * update_world frames run with the camera at (0x60,0x50), and
             * 1000:5CB6 walks it onto the acting record at <=4 px a frame.
             * The seed is clamped to the pan range: at scale 2 the range is
             * empty, so the view starts centred on the whole map and never
             * moves (D-274). */
            {
                uint16_t sx = 0x60u, sy = 0x50u;

                if (sx > level_view_max_x()) sx = level_view_max_x();
                if (sy > level_view_max_y()) sy = level_view_max_y();
                level_set_view(sx, sy);
            }
            object_sequence_level_start_record(); // 1000:3355
            _world_updates = 1; // 1000:336C: [0xC] = 0xFF
            _render_world(); // 1000:4C28's pre-draw (one page)
            _start_phase = LVSTART_WARMUP;
            _warmup_left = 8u; // level_fade_in's LOOP CX=8
        }
        break;
    case LVSTART_WARMUP:
        doUpdate = 1; // 1000:3374's eight frames
        if (--_warmup_left == 0u) {
            palette_build_fade(0, 256, 0x32, level_palette());
            _start_phase = LVSTART_FADE_IN;
        }
        break;
    case LVSTART_FADE_IN:
        if (!palette_fade_active()) {
            _start_phase = LVSTART_PLAYING;
            statusbar_set_active(1); // level_fade_in's [0x5B12]
        }
        break;
    default: break;
    }

    _level_palette_rotate();

    /* the object machine's frame (the claim's sequence and its fades); the
     * original runs it from the level's per-frame body, so not during the
     * start chain. */
    /* 1000:80AD update_world runs for level_fade_in's eight frames and then
     * from the play loop; it does NOT run during the palette fade, where
     * level_fade_in spins in `wait_palette_fade_done` (1000:8B8B).  The
     * marker's lifetime shows it: the live record 8 reads type 0x18 /
     * +0xA 0x1001 when play starts - exactly eight updates after the
     * 0x1006 init (the fade-out tick that seeds the view does not count). */
    if (doUpdate) {
        /* get_object_ptr (1000:79F4) then the input command (1000:E3F6) for
         * the acting record, before the object tick.  `joyBits` is the port's
         * [0x5ACE] (backend -> 1000:8526's shape).  The whole chain is behind
         * `[0xC]` (1000:80AD's first test), which level_fade_in sets before
         * its eight frames, and the claim holds it at 0. */
        uint8_t *actor = object_core_slot(object_sequence_action_entity());

        if (_world_updates && !object_sequence_claim_active()) {
            _level_goal_pan_tick(); // 1000:3540, before update_world
            level_follow_view(); // 1000:5CB6, first in update_world
            if (actor) (void)object_command_run(actor, _level_joy);
            object_walk_update_list(); // 1000:C295, 1000:80BD
            object_render_camera_slots_tick(); // 1000:C7A0, 1000:80C0
            entity_tick(); // 1000:C93C, inside the [0xC] gate
            object_core_goal_scan(); // 1000:C2CE, the goal count
            object_sequence_marker_tick(); // 1000:D64F, after the dispatch
            object_core_world_tick(); // 1000:80CC/0x80CF, update_world's tails
            object_command_row_pass(); // 1000:D860, the static records. rows
            _level_goal_pan_after(); // the frame the pan just ran
        }
        /* The claim's own frames only: the original sequence never calls
         * update_world, even after 1000:3CAB sets [0xC] = 0xFF. */
        object_core_claim_frame(level_palette());
    }

    /* The _world is not drawn during level_video_init's two component fades
     * (1000:32F4 / 1000:3309): in the original the screen still holds the
     * pre-level art there, so those fades recolour IT while the DAC walks to
     * 0x14D3 - the _world's first frames are the eight update_world passes of
     * level_fade_in, drawn while the DAC is at black (the fade-out of 0..0xBF
     * runs first, 1000:3334). */
    /* 0x3FB3 is the play frame's flush, inside 0x3F4A (see `_render_world`);
     * the chain's fades and the loading come before the first play frame. */
    if (_start_phase != LVSTART_WARMUP && _start_phase != LVSTART_FADE_IN &&
        _start_phase != LVSTART_PLAYING) {
        /* the bar is up from the mode reset onward (1000:952C/957B), so it is
         * redrawn here while the _world waits for its black frames */
        statusbar_set_surface(NULL, 0u, 0u, 0u, level_bar_row());
        statusbar_tick();
        return 1;
    }

    /* 1000:3D04/3D5E/3CB5: the original draws the world once, freezes it at
     * the pan's recolour and only re-blits it at 0x3CB5.  The claim's other
     * frames present the frozen page (1000:40CA / 1000:3F4A).
     * The three phases before the pan (1000:3B34..3B8E) only fade the DAC:
     * the last drawn page - sprites included - must be left alone. */
    if (!object_sequence_claim_active() || object_sequence_claim_pan_active() ||
        object_sequence_claim_flip_active()) {
        _render_world();
        if (object_sequence_claim_pan_active()) { // 1000:3D5E's recolour on both pan frames
            uint8_t *page = g_osys->videoPage(0);

            level_claim_recolour();
            /* 1000:3D30: 40CA redraws the two records AFTER the recolour, so
             * the witchdoctor and the victim (frame 0x103, the surprised face)
             * sit on the black page and persist through the fades/blinks. */
            if (page) {
                level_claim_draw_records(page);
                _level_page_bake();
            }
        }
    } else if (object_sequence_claim_page_frozen() &&
               !object_sequence_claim_settle_active()) {
        level_claim_frame();
    }
    /* CLAIM_SETTLE: the page is the final 1000:3CB5 frame (world + objects);
     * the original only fades the DAC there (1000:3CDE), so leave it. */

    /* The [0x1979] dispatch is input_dispatch_loop's 1000:34C3..0x34E4, i.e.
     * game_main's play frame after scan_id_list and pause_toggle; level_tick
     * only runs update_world.  Kept out of here so the key list is gated on a
     * pending code (1000:386E) and a code armed by pause_toggle dispatches on
     * the same frame, as in the original. */

    /* input_dispatch_loop 1000:34B8's object_core_test_dispatch call, after the
     * _world tick: a negative return ends the play loop.  menu_flag_tick's pan
     * blocks the outer loop, so the test cannot run while it is up. */
    if (_start_phase == LVSTART_PLAYING && !level_goal_pan_running()) {
        (void)level_end_test();
        if (_level_end_sound_pending()) { // 1000:34E6
            object_core_driver_call(0x0Cu); // 1000:83B7 with AX = 0xC
            _level_end_hold_start();
        }
    }

    /* the colours are the working palette 202F:0x14D3, built in level_load
     * and faded in by the first tick above. */
    return 1;
}
