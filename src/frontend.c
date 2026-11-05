/* frontend.c — the front-end room: the walkable level-select / game-start
 * menu (subsystem 8; frontend_room_setup 1000:A48E + the _frame web +
 * level_select_event_loop 1000:AA2C), ported from re_map.md -> "The
 * front-end room (the level-select / game-start menu) — the core decoded
 * (2026-10-26)" and its "Closed since" list.
 *
 * THE ROOM.  A 640-px-wide walkable side view on the original's unchained
 * mode-X space: two pages at byte offsets 0 / 0x7D00, 160 bytes per row
 * (byte mode: 1 address byte = 4 pixels, one per plane; 160*4 = 640), the
 * _scroll word [0x0] (set_scroll_x 1000:AA05) picking the 320-px window the
 * CRTC scans out.  The name/code screen is another window at page offset
 * 0x2800 (name_screen_show 1000:AF92).  Every present composes the window
 * into the flat page 0 — game_main's videoPresent shows it (the attract's
 * contract).
 *
 * THE SELECT.  The typed name is matched against levels\_codes.bin (160
 * NUL-terminated strings, string i = level i, #1 "DARWIN" — menu_code_match
 * 1000:AB39); the matched index [0x5F09] becomes the level: entry's PUSH AX
 * at 004B / POP AX at 00BA feed [0x22].  The exits: walk off the right edge
 * (x=0x280, via _cell 6's R rail) — the match runs, the result is NOT tested,
 * the fade happens either way; or Enter on the name screen, which exits
 * only on a match (a miss resets the name to DARWIN and stays).  ESC clears
 * the name and never exits (0x5F11's act0); Backspace erases the last
 * char; A-Z 0-9 space type.  There is no key path back to the attract.
 *
 * THE WALK.  [0x5ACE] bits -> dir (bit2 left=0 > bit3 right=1 > bit0 up=2 >
 * bit1 down=3, update_object_fields 1000:A75B).  At rest a dir key tests
 * the rail table at 202F:0x5ECD (7 cells x 4 words (L,R,U,D) =
 * (target_cell << 12) | target_coord); a nonzero rail arms the walk
 * (state := dir, _frame := the cycle low, the origin published to
 * [0x5F05]/[0x5F07]).  Mid-walk, only the opposite dir re-aims (the +-1
 * reverse gate); otherwise the steppers (walk_step_horizontal 1000:A8D3 /
 * _vertical 1000:A963 via the presets at 0x5EBD) advance 1 px per tick
 * toward the rail's target, arriving on coord equality (_cell := the high
 * nibble, state := 0xFFFF = at rest).  The floor table at 202F:0x5E7E pins
 * y for x < 0x138.  The tail dispatches on x: 0x280 -> the right-edge exit,
 * 0x108 -> the name screen's pan-out.
 *
 * THE OPTIONS.  The ACTION key at rest cycles the _cell's option counter
 * ([0x5E46] = (struct-idx, count); the structs' +7 byte at 0x5E5B/0x5E65/
 * 0x5E6F/0x5E79 IS the badge's _frame, so the setting and the picture are
 * one byte).  One cycle per press (the original spins on the held key).
 * The counters PERSIST for the run: frontend_room_setup (1000:A48E) never
 * writes 0x5E5B..0x5E79 and the image ships them zero, so re-entering the
 * room must not reset them.  The four boards (the signs are painted in
 * _code2.raw's backdrop; the sprites are the badges):
 *   _cell 2 = LEVEL, EASY/NORMAL/HARD -> [0x5E65]: entry (1000:009F) maps
 *            0/1/2 to the 12/10/8 tribe and render_level_frame (1000:958C)
 *            picks the time limit A/B/C;
 *   _cell 3 = MUSIC, ON/OFF -> [0x5E5B]: the _frame web (1000:A5E8) stops
 *            the music when non-zero and resumes it when zero;
 *   _cell 5 = ANIMATIONS, ON/OFF -> [0x5E79]: cd_file_op (1000:AFFD) RETs
 *            at once when non-zero — the .ANI cutscenes are off;
 *   _cell 6 = RESTORE GAME, OFF/1..20 -> [0x5E6F]: the exit hands it to
 *            entry 1000:004C in BL, which appends 'A'+n-1 to
 *            "\humans\cdrom\save" and loads that save _slot.
 * frontend_option() is the read side's one accessor.
 *
 * DEVIATIONS (all port-only, each waiting on its named step; the
 * authoritative list is port_deviations.md — D-01/D-19/D-20 here):
 *  - the INT 60h calls (music_resume/stop 83F7/8389, the cmd-0xA pump) are
 *    not made: the audio backend is an M5 stub.  audio_music_gate ports the
 *    gate and the [0x40BB] latch; only the tune handle is missing (M6).
 *  - the CRTC split ([0x5B0A]) is modelled in the _compose: rows >= the
 *    split display the FIXED band from absolute _vram 0, unpanned (the
 *    level's status bar and the name screen's human stage both live
 *    there); the original latches the same at vsync.
 *  - the name line is the AB8D render proper (2026-10-27): the typed
 *    name goes to the _arena via draw_text and the WM2 bit-7 copy loop
 *    carries it to the page — DARWIN shows in the entry box; the prior
 *    "never displayed" reading is archived in re_history.md..
 *  - the backdrop copy is the original's 200-row one (`room_backdrop_build`
 *    `1000:AF07`, CX = 0xC8): rows 192..199 are the bytes past the 640x192
 *    image — its palette tail, then the heap pool.  The room's field is
 *    `reset_display_mode`'s 320x192 (`1000:82C6`, called at `1000:A544`),
 *    so those 8 rows are written but never presented, exactly as in the
 *    original.  Past the loaded chunk the port writes zeros where the
 *    original reads an allocator-dependent pool tail; the difference has no
 *    observable output (the field ends on row 191).
 *  - with no data (the headless tests) a failed load skips the palette and
 *    the fade phases, and the code match answers index 1 (DARWIN) so the
 *    flow completes — the attract's asset-failure pattern.
 */
#include "frontend.h"
#include "audio.h"
#include "font.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "video.h"

#include <stdlib.h>
#include <string.h>

/* --- the module state (the DS analogs in comments) -------------------- */
typedef enum FePhase {
    FE_IDLE = 0,
    FE_LOAD,
    FE_FADEIN,
    FE_ROOM,
    FE_PAN,
    FE_NAME,
    FE_EXITFADE
} FePhase;

/* --- the planar space (A000, unchained, byte mode) -------------------- */
static uint8_t _vram[65536][4];   /* [address][plane]: 1 byte = 4 pixels   */
static uint32_t _page_draw, _page_disp; /* [0x5AC4]/[0x5AC6] — page bases   */
static uint32_t _scroll; /* [0x0]: the front-end _scroll      */

/* --- the shipped tables, verbatim -------------------------------------- */

/* 202F:0x5ECD — 7 cells x 4 words (L,R,U,D):
 * (target_cell << 12) | target_coord (x for L/R, y for U/D). */
static const uint16_t _rails[7][4] = {
    { 0x0108, 0x6208, 0x1037, 0x0000 }, { 0x2190, 0x0000, 0x4020, 0x00A0 },
    { 0x316A, 0x11D2, 0x0000, 0x0000 }, { 0x0000, 0x2190, 0x0000, 0x0000 },
    { 0x0000, 0x521B, 0x0000, 0x1037 }, { 0x41D2, 0x0000, 0x0000, 0x0000 },
    { 0x01D2, 0x0280, 0x0000, 0x0000 },
};

/* 202F:0x5E46 — per _cell (struct-idx, count); count 0 = no option.
 * _cell 2 = LEVEL (3 states), _cell 3 = MUSIC (2), _cell 5 = ANIMATIONS (2),
 * _cell 6 = RESTORE GAME (21 = 0x15). */
static const uint8_t _cell_opts[7][2] = {
    { 0, 0 }, { 0, 0 }, { 1, 3 }, { 0, 2 }, { 0, 0 }, { 3, 2 }, { 2, 0x15 },
};

/* 202F:0x5EC5 — the walk cycle's _frame low per dir; the presets' cycle
 * high (the CX at 0x5EBD's entries) alongside. */
static const uint16_t _dir_frame_low[4] = { 0x0B, 0x03, 0x14, 0x16 };
static const uint16_t _dir_cycle_high[4] = { 0x13, 0x0B, 0x17, 0x13 };

/* 202F:0x5E7E — the floor table, indexed (x - 0xFD); y = byte - 0x18. */
static const uint8_t _floor_tab[0x3B] = {
    0xB1, 0xB1, 0xB0, 0xB0, 0xAF, 0xAF, 0xAF, 0xAE, 0xAE, 0xAE, 0xAE, 0xAE,
    0xAE, 0xAE, 0xAE, 0xAE, 0xAE, 0xAE, 0xAE, 0xAF, 0xAF, 0xAF, 0xB0, 0xB0,
    0xB1, 0xB1, 0xB2, 0xB2, 0xB2, 0xB3, 0xB3, 0xB3, 0xB4, 0xB4, 0xB4, 0xB4,
    0xB4, 0xB5, 0xB5, 0xB5, 0xB5, 0xB5, 0xB5, 0xB5, 0xB6, 0xB6, 0xB6, 0xB6,
    0xB6, 0xB6, 0xB6, 0xB6, 0xB6, 0xB7, 0xB7, 0xB7, 0xB7, 0xB7, 0xB7,
};

/* 202F:0x61A3 — the anim _slot table: 60 data bytes, then the 0x00 wrap
 * terminator at [60] (verbatim from the image; an earlier embed had a
 * duplicated 0x99 there, which made the +3 _slot walk run past the table
 * and never wrap — the humans' cut column and the name screen's pan both
 * went garbage-derived: the humans vanished behind the range guard and
 * the window scrolled wildly). */
static const uint8_t _slot_tab[61] = {
    0x88, 0x88, 0x88, 0x77, 0x77, 0x77, 0x66, 0x66, 0x66, 0x55, 0x55,
    0x66, 0x44, 0x55, 0x66, 0x44, 0x55, 0x66, 0x44, 0x55, 0x66, 0x55,
    0x55, 0x66, 0x66, 0x66, 0x66, 0x77, 0x77, 0x77, 0x88, 0x88, 0x88,
    0x99, 0x99, 0x99, 0xAA, 0xAA, 0xAA, 0xBB, 0xBB, 0xAA, 0xCC, 0xBB,
    0xAA, 0xCC, 0xBB, 0xAA, 0xCC, 0xBB, 0xAA, 0xBB, 0xBB, 0xAA, 0xAA,
    0xAA, 0xAA, 0x99, 0x99, 0x99, 0x00, /* [60] = the wrap terminator, verbatim from the image      */
};

static FePhase _phase = FE_IDLE;
static uint16_t _cell; /* [0x4220]: the rail node              */
static uint16_t _walk_state; /* [0x4222]: bit15 = at rest, 0..3 walk */
static uint16_t _char_x, _char_y; /* [0x4224]/[0x4226]: the world x/y     */
static uint16_t _frame, _frame_cnt; /* [0x4228]/[0x422A]                    */
static uint16_t _pub_x, _pub_y; /* [0x5F05]/[0x5F07]: the walk origin   */
static char _name_buf[15]; /* 0x61E0: 14 chars + the 0x61EE NUL   */
static uint32_t _code_index; /* [0x5F09]: the matched code's index   */
static uint8_t _opt_counter[4]; /* the structs' +7 byte (0x5E5B..0x5E79)
                                  * = FRONTEND_OPT_*; persists for the run */
static int32_t _fire_latch; /* the action key's held-spin           */
static uint16_t _slot_cnt; /* [0x619F]: the anim countdown (5)     */
static uint32_t _slot; /* [0x61A1]: the anim _slot (parked 0x12)*/
static uint8_t _name_screen; /* [0x6267]: the name-screen flag       */

static uint8_t *_code2, *_codehum, *_codes;
static uint8_t *_codeb1, *_codeb2, *_restore;
static uint32_t _code2_size, _codehum_size, _codes_size;
static uint32_t _codeb1_size, _codeb2_size, _restore_size;
static uint8_t _arena[16000]; /* the font _arena (the [0x1E20] analog) */
static const uint8_t _ramp0[768]; /* the 0x48BC ramp: zeroed = black     */
static uint32_t _split_row = 200; /** [0x5B0A]: the CRTC split (0x88 on the
                                   * name screen — the fixed band below) */

/** @brief Bind the frontend module to the backend. */
int32_t frontend_init(const OSystem *osys)
{
    (void)osys;
    return 0;
}

/** @brief Accessor: the level-code cursor. */
uint32_t frontend_selected_level(void) { return _code_index; }

/** entry's JMP LAB_002B: the room's setup runs again from the top (the
 * counters persist — frontend_room_setup seeds none of them).  The next
 * frontend_tick re-loads the assets (FE_LOAD). */
void frontend_restart(void)
{
    _phase = FE_LOAD;
    _fire_latch = 0;
}

/** The four option counters — the read side's one accessor.  Consumers:
 * level.c (LEVEL: the time-limit pick + the 12/10/8 tribe), audio.c (MUSIC:
 * the _frame web's gate), scene.c (ANIMATIONS: cd_file_op's early RET).  The
 * RESTORE GAME _slot is carried out here and consumed by entry 1000:004C's
 * save load, which the port's save.c does not have yet (M6). */
uint32_t frontend_option(uint32_t idx)
{ return idx < 4u ? _opt_counter[idx] : 0u; }

/** [0x4220] — the rail node, exposed for the option read-side test's walk
 * legs (the same role as attract_phase_for_probe). */
uint32_t frontend_cell_for_probe(void) { return _cell; }

/** the name screen's _phase and typed buffer, for the input-law tests */
int32_t frontend_name_screen_for_probe(void) { return _phase == FE_NAME; }
/** @brief Accessor: the typed high-score name. */
const char *frontend_name_for_probe(void) { return _name_buf; }

/** --- the VRAM primitives ---------------------------------------------- */
static void _put_pixel(uint32_t rowBase, uint32_t px, uint8_t v)
{ _vram[(rowBase + (px >> 2)) & 0xFFFFu][px & 3u] = v; }

/** blit_planar_strip (1000:AEAD): planarise chunky 640-px rows into the
 * page at `base` — dest[base + r*160 + c] plane p = src[r*640 + c*4 + p].
 * `size` is the whole loaded chunk, so the 200-row backdrop copy reads the 8
 * rows past the 192-row image out of its palette tail, as `1000:AEDB`'s row
 * loop does; only the bytes past the chunk (the original's uninitialised
 * pool) are written as zeros.  The room's 320x192 field never shows them. */
static void _planarise_rows(const uint8_t *src, uint32_t size, uint32_t base,
                            uint32_t rows)
{
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < 160u; c++) {
            uint32_t a = (base + r * 160u + c) & 0xFFFFu;
            uint32_t off = (uint32_t)r * 640u + (uint32_t)c * 4u;
            if (off + 3u < size) {
                _vram[a][0] = src[off];
                _vram[a][1] = src[off + 1u];
                _vram[a][2] = src[off + 2u];
                _vram[a][3] = src[off + 3u];
            } else {
                _vram[a][0] = _vram[a][1] = 0u;
                _vram[a][2] = _vram[a][3] = 0u;
            }
        }
}

/** The present: the 200-row window, flat — pixel x of the window = the
 * world pixel (_scroll + x), i.e. set_display_ptr's byte start (882D:
 * [0x5AC4] + [0x2]*88 + [0x0]/4) plus the pel-pan _phase ([0x0] & 3 —
 * 8848/884a).  Rows >= _split_row ([0x5B0A]) are the CRTC split's FIXED
 * band: they display from ABSOLUTE _vram 0 (the level's status bar and the
 * name screen's human stage both live there), unpanned.  The room is
 * double-buffered (present_page 1000:8809 swaps after showing); the name
 * screen's cursor+present double-swap cancels, so it passes 0. */
static void _compose(int32_t swapPages)
{
    uint8_t *page = video_page(0);
    if (page) {
        for (uint32_t y = 0; y < 200u; y++) {
            uint8_t *d = page + y * 320u;
            if (y < _split_row) {
                uint32_t rowBase = _page_draw + y * 160u;
                for (uint32_t x = 0; x < 320u; x++) {
                    uint32_t w = _scroll + x; /* the world pixel */
                    d[x] = _vram[(rowBase + (w >> 2)) & 0xFFFFu][w & 3u];
                }
            } else { /* the fixed band: from _vram 0 */
                uint32_t rowBase = (y - _split_row) * 160u;
                for (uint32_t x = 0; x < 320u; x++)
                    d[x] = _vram[(rowBase + (x >> 2)) & 0xFFFFu][x & 3u];
            }
        }
    }
    if (swapPages) {
        uint32_t t = _page_draw;
        _page_draw = _page_disp;
        _page_disp = t;
    }
}

/** @brief Copy a rectangular, zero-transparent cell block to the page. */
static void _page_cells(uint32_t dst, const uint8_t *src, uint32_t srcStride,
                        uint32_t w, uint32_t h, uint32_t dx)
{
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++) {
            uint8_t v = src[r * srcStride + c];

            if (v) _put_pixel(dst + r * 160u, dx + c, v);
        }
}

/** @brief Patch a zero-transparent cell block into the loaded code2 image. */
static void _patch_code2(uint32_t pOff, const uint8_t *src, uint32_t w,
                         uint32_t h)
{
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++) {
            uint8_t v = src[r * w + c];

            if (v) _code2[pOff + r * 640u + c] = v;
        }
}

/** place_4_sprites_to_vga (1000:A684) — the option-value badges at the
 * MUSIC / LEVEL / RESTORE GAME / ANIMATIONS signs.  The entry table at
 * 202F:0x5E54
 * (10 bytes apart, verbatim): {u16 dest, u16 srcOff, u8 w, u8 h,
 * u8 _cell, u16 &srcSeg} — and the +7 `_cell` field IS the option counter
 * (the "option structs" and this table are the same bytes: 0x5E5B /
 * 0x5E65 / 0x5E6F / 0x5E79).  The sprite = w*h chunky bytes at
 * srcOff + _cell*w*h, nonzero-transparent, at the page byte `dest`
 * (+[0x5AC4], 160-pitch).  The shipped files close the _cell counts:
 * _codeb1 6144 = 4x(48x32) (MUSIC's ON/OFF + ANIMATIONS' ON/OFF), _codeb2
 * 7680 = 3x(80x32) (LEVEL's EASY/NORMAL/HARD), _restore 32256 =
 * 21x(48x32) (RESTORE GAME's OFF/1..20); the 4th entry is _codeb1's cells
 * 2..3 at srcOff 0xC00. */
static void _draw_option_sprites(void)
{
    static const struct {
        uint16_t dest, srcOff;
        uint8_t w, h, cellIdx;
    } e[4] = {
        { 0x1D14, 0x0000, 48, 32,
          0 }, /* MUSIC        (2, _codeb1 cells 0..1) */
        { 0x1C80, 0x0000, 80, 32,
          1 }, /* LEVEL        (3, _codeb2)            */
        { 0x5BBC, 0x0000, 48, 32,
          2 }, /* RESTORE GAME (21, _restore.spr)      */
        { 0x0F88, 0x0C00, 48, 32,
          3 }, /* ANIMATIONS   (2, _codeb1 cells 2..3) */
    };
    const uint8_t *file[4] = { _codeb1, _codeb2, _restore, _codeb1 };
    const uint32_t size[4] = { _codeb1_size, _codeb2_size, _restore_size,
                               _codeb1_size };
    for (uint32_t i = 0; i < 4u; i++) {
        const uint8_t *src = file[i];
        if (!src) continue;
        uint32_t w = e[i].w, h = e[i].h;
        uint32_t _cell = _opt_counter[e[i].cellIdx];
        uint32_t off = e[i].srcOff + _cell * w * h;
        if (off + w * h > size[i]) continue;
        /* the stager's inner pass (A6CB..A6F5) FIRST patches the badge
         * into the LOADED _code2.raw image itself — ES := [0x625D] +
         * dest/4 PARAGRAPHS = the chunky byte offset dest*4, w bytes per
         * row at +640 stride, nonzero-transparent — so the character's
         * backdrop-restored box carries the badge and both the occlusion
         * and the 2-col page-alternation trail stay clean.  The cells'
         * transparent masks are uniform within each badge's _cell group,
         * so cycling overwrites cleanly. */
        if (_code2 && (uint32_t)e[i].dest * 4u < _code2_size &&
            (uint32_t)e[i].dest * 4u + (h - 1u) * 640u + w < _code2_size)
            _patch_code2((uint32_t)e[i].dest * 4u, src + off, w, h);
        uint32_t destRow = e[i].dest / 160u, destCol = e[i].dest % 160u;
        if (destCol * 4u + w > 640u) continue;
        _page_cells(_page_draw + destRow * 160u, src + off, w, w, h,
                    destCol * 4u);
    }
}

/** name_screen_anim_frame's two human draws (via name_entry_render's ADB6
 * entry): a 20x20 cut of _code2's human strip bank — rows 0..19 at column
 * (0x9E - nibble), the nibble from the NEXT TWO _slot bytes — copied to
 * the SAME x; on the name screen to the fixed band (rows 0..19/16..35),
 * on the room screen to [0x5AC4]+0x5500 + those (page rows 136..155/
 * 152..171 — the middle pair of the four-human stack holding the board).
 * The stager's OVERLAY pass (AE6E..AE9E) then composites _codehum _frame 2
 * (CS:[0xadb4]=0, 16 rows of 16 bytes, nonzero-only) at scratch rows 1..16,
 * cols 2..17 — ported here as the same overlay. */
static void _draw_humans(void)
{
    if (!_code2 || _code2_size < 0x17C00u + 20u * 640u) return;
    static const uint32_t bankBase[2] = { 0x15400u, 0x17C00u };
    static const uint32_t destRow[2] = { 0u, 16u };
    for (uint32_t h = 0; h < 2u; h++) {
        uint32_t nibble = _slot_tab[(_slot + 1u + h) % 61u] & 0x0Fu;
        uint32_t cx = 0x9Eu - nibble; /* 0x9E - nibble */
        /* ADB6's flag pick: [0x6267] set -> DI used directly (the fixed
         * band's rows 0..19/16..35); clear -> DI += 0x5500 (page row 136)
         * + [0x5AC4] — the room screen: page rows 136..155/152..171 at the
         * same world x, the art's empty gap between the painted rail-
         * gripper (rows 112..138) and the floored one (166..188).  It is
         * exactly this middle pair that turns the screen's hanging human
         * into the FOUR-human stack holding the board while the 320-px
         * window slides across the 640-px room during the pan. */
        {
            uint32_t base = _name_screen
                                ? destRow[h] * 160u
                                : _page_draw + (136u + destRow[h]) * 160u;

            _page_cells(base, _code2 + bankBase[h] + cx, 640u, 20u, 20u, cx);
            /* the overlay's scratch offset CS:[0xaeab] is 0 for the anim
             * draws (ada2's room draw leaves it 0) — the sprite lands at
             * box rows 0..15, cols 2..17 (0x5F65 = base+2) */
            if (_codehum && _codehum_size >= 16u * 16u)
                _page_cells(base, _codehum, 16u, 16u, 16u, cx + 2u);
        }
    }
}

/* --- the name ----------------------------------------------------------- */

/** @brief Write mode 2 on one chained-page byte, bits 0..3. */
static void _wm2_set(uint32_t addr, uint8_t v)
{
    for (uint32_t p = 0; p < 4u; p++)
        _vram[addr][p] =
            (uint8_t)((_vram[addr][p] & 0x7F) | ((v >> p) & 1u) << 7);
}

/** name_entry_blit (1000:AB8D) — the typed name IS displayed: the last
 * claim ("never displayed, live-verified") misidentified this routine's
 * purpose; its callers were verified across the room path (addchar,
 * backspace, ESC-clear, the match's miss path, the room backdrop build)
 * and the loop's raw bytes settle the shape: 10 rows x 40 copy-pairs —
 * the ARENA (draw_text already wrote the glyphs at rows 4..13, bytes
 * 18..57) read at 0x152+80/row, written to the page at [0x5AC4]+0x38F5
 * +160/row under vga_set_write_mode2's bit-mask 0x80 (299b: OUT DX,AX
 * with AH=0x80).  Each write sets the color-index TOP bit of the 4 dest
 * pixels from the _arena byte's bits 3..0 — the bright-variant palette
 * (0x80+n) the dark bar's pixels sit in.  Ported verbatim. */
static void _draw_name_line(void)
{
    /* AB8D's exact sequence: clear the _arena's first 1600 bytes, draw the
     * name into it (draw_text centres at x 0x98, rows 0..15 — the glyphs
     * land at _arena rows 4..13), then the WM2 copy: 10 rows x 40 bytes,
     * source _arena 0x152+80/row, dest [0x5AC4]+0x38F5+0xA0/row — AB8D
     * writes 40 bytes (DI +40) then skips 0x78 (120): 40+0x78 = 0xA0. */
    memset(_arena, 0, 1600u);
    font_draw_text(_arena, _name_buf, 0x98u, 0);
    for (uint32_t r = 0; r < 10u; r++)
        for (uint32_t c = 0; c < 40u; c++) {
            uint8_t v = _arena[0x152u + r * 80u + c];
            uint32_t addr = (_page_draw + 0x38F5u + r * 0xA0u + c) & 0xFFFFu;
            _wm2_set(addr, v);
        }
}

/** name_entry_init (1000:AAC9): the "DARWIN" seed AND four zero words at
 * 0x61E6..0x61ED - the whole 14-byte buffer, not just the terminator. */
static void _name_reset(void)
{
    memset(_name_buf, 0, sizeof _name_buf);
    memcpy(_name_buf, "DARWIN", 6u);
    _draw_name_line();
}

/** name_entry_addchar (1000:AB1E): append at the first NUL.  The BX = 13 walk
 * writes the 14th character at index 13; a full buffer overwrites index 13.
 * (The original's buffer is 0x61E0..0x61ED, terminated by the fixed zero at
 * 0x61EE; _name_buf[14] is that zero.) */
static void _name_addchar(uint8_t c)
{
    uint32_t i = 0;
    int32_t bx = 13;

    while (_name_buf[i]) {
        bx--;
        if (bx < 0) break;
        i++;
    }
    _name_buf[i] = (char)c;
    _draw_name_line();
}

/** menu_action_backspace (1000:AB00): erase the last char. */
static void _name_erase(void)
{
    int32_t i = 13;
    while (i > 0 && _name_buf[i] == 0) i--;
    _name_buf[i] = 0;
    _draw_name_line();
}

/** menu_code_match (1000:AB39): walk _codes.bin's NUL strings against the
 * name; a full match returns nonzero with _code_index = the 1-based index
 * (string i = level i); exhausted -> reset DARWIN, _code_index = 161 (the
 * original's own edge: [0x5F09] counts past every miss). */
static int32_t _code_match(void)
{
    _code_index = 1;
    if (!_codes || _codes_size < 2u) {
        return 1; /* the no-data path: DARWIN at index 1 */
    }
    const char *p = (const char *)_codes;
    const char *end = (const char *)_codes + _codes_size;
    uint32_t remaining = 160u; /* [0x6193] = 0xA0 */
    while (remaining-- && p < end) {
        const char *c = p, *n = _name_buf;
        while (*n && *c == *n) {
            c++;
            n++;
        }
        if (*c == *n) return 1;
        _code_index++;
        p += strlen(p) + 1;
    }
    _name_reset();
    return 0;
}

/* --- the character draw (room_draw_character 1000:AD40) ---------------- */

/** The floor adjust (the tail A85A..A885) + the record publish + the draw.
 * The _frame is the record's unless x < 0x10B (the idle pose), the draw x
 * clamped >= 0x108.  The box (sprite_stage_rows 1000:AE39 +
 * sprite_blit_4plane 1000:ADC8) is the BACKDROP RESTORED FROM _code2.raw
 * ITSELF — the stager copies _code2's own 20-byte strips at the character's
 * position (DS := code2seg + (y-1)*0x28 paragraphs, SI := x, +0x26C after
 * 20 bytes = one 640-byte row per strip) into the scratch, then overlays
 * only the sprite's NONZERO bytes (box rows 1..24, cols 2..17); the blit
 * writes all 27 rows x 20 cols unconditionally.  The 2-col backdrop
 * margins are what make the page alternation seam-free: the 2-px-per-
 * 2-frames trail lands on pure backdrop. */
static void _draw_character(void)
{
    uint16_t rx = _char_x, ry = _char_y;
    if (rx < 0x138u) {
        if (rx >= 0x108u) ry = (uint16_t)(_floor_tab[rx - 0xFDu] - 0x18u);
        else ry = 0x96u;
    }
    if (!_codehum || _codehum_size < 8064u) return;
    if (!_code2 || _code2_size < 0x1E000u) return;
    uint16_t dframe = (rx < 0x10Bu) ? 0x13u : _frame;
    if (dframe < 2u || dframe > 22u) return;
    uint16_t dx = (rx < 0x108u) ? 0x108u : rx;
    const uint8_t *spr = _codehum + (size_t)(dframe - 2u) * 384u;
    for (uint32_t r = 0; r < 27u; r++) {
        uint32_t srcRow = (uint32_t)(ry - 1u) + r;
        /* AD78..AD7F: on the name screen ([0x6267]) the blit dest skips the
         * [0x5AC4] page add entirely (screen show holds [0x5AC4]=0 across
         * the call) and is forced to ABSOLUTE 0x820 — page rows 13..39 of
         * the fixed band, i.e. screen rows 149..175 at the char's own x:
         * the human standing on the small hill at the bottom right. */
        uint32_t base =
            _name_screen ? 0x820u + r * 160u : _page_draw + srcRow * 160u;
        for (uint32_t c = 0; c < 20u; c++) {
            uint8_t v = (srcRow < 192u) ? _code2[srcRow * 640u + dx + c] : 0u;
            if (r >= 1u && r < 25u && c >= 2u && c < 18u) {
                uint8_t s = spr[(r - 1u) * 16u + (c - 2u)];
                if (s) v = s;
            }
            _put_pixel(base, dx + c, v);
        }
    }
}

/* --- the walk (update_object_fields 1000:A75B + the steppers) ----------- */

/** set_scroll_x (1000:AA05): y >= 0x64 -> min(x-0x92, 0x140), else 0x140. */
static void _set_scroll(void)
{
    uint32_t s = 0x140u;
    if (_char_y >= 0x64u) {
        s = (uint16_t)(_char_x - 0x92u);
        if (s >= 0x140u) s = 0x140u;
    }
    _scroll = s;
}

/** walk_step_horizontal (1000:A8D3) / _vertical (1000:A963), entered via
 * the presets at 0x5EBD: step = -1 left/up, +1 right/down; the _frame
 * cycle and the rail-arrival tests verbatim. */
static void _walk_step(uint32_t dir)
{
    int16_t step = (dir == 1u || dir == 3u) ? 1 : -1;
    uint32_t high = _dir_cycle_high[dir];
    uint16_t rail = _rails[_cell][dir];
    uint16_t target = rail & 0x0FFFu;

    if (dir < 2u) { /* A8D3 */
        if (_frame != 0x13u) _char_x = (uint16_t)(_char_x + step);
        _frame_cnt--;
        if (_frame_cnt & 0x8000u) {
            _frame_cnt = 3;
            _frame = (uint16_t)(_frame + 1u);
            if (_frame >= high) _frame = (uint16_t)(high - 8u);
        }
        if (_char_x == target) {
            _cell = rail >> 12;
            _walk_state = 0xFFFF;
            _frame = 0x13;
            _frame_cnt = 3;
        } else if (_char_x == _pub_x) {
            _walk_state = 0xFFFF;
            _frame = 0x13;
            _frame_cnt = 3;
        }
    } else { /* A963 */
        if (_frame != 0x13u) _char_y = (uint16_t)(_char_y + step);
        step = (int16_t)-step;
        _frame_cnt--;
        if (_frame_cnt & 0x8000u) {
            _frame_cnt = 3;
            int32_t flip = (_frame == 0x13u);
            if (!flip) {
                _frame = (uint16_t)(_frame + step);
                flip = (_frame == high);
            }
            if (flip) {
                step = (int16_t)-step;
                _frame = (uint16_t)(high + step);
                _frame = (uint16_t)(_frame + step);
                _frame = (uint16_t)(_frame + step);
            }
        }
        if (_char_y == target) {
            _cell = rail >> 12;
            _walk_state = 0xFFFF;
            _frame = 0x13;
            _frame_cnt = 3;
        } else if (_char_y == _pub_y) {
            _walk_state = 0xFFFF;
            _frame = 0x13;
            _frame_cnt = 3;
        }
    }
}

/** @brief The frontend room's cursor walk for one tick. */
static void _room_walk(const InputState *in)
{
    uint32_t dir = 4;
    if (in->joyBits & 0x04u) dir = 0; /* left  — the cascade's order */
    else if (in->joyBits & 0x08u) dir = 1; /* right                        */
    else if (in->joyBits & 0x01u) dir = 2; /* up                           */
    else if (in->joyBits & 0x02u) dir = 3; /* down                         */

    if (dir != 4u) {
        if (_walk_state & 0x8000u) { /* at rest: the rail */
            uint16_t rail = _rails[_cell][dir];
            if (rail) { /* A818..A854 */
                _pub_x = _char_x;
                _pub_y = _char_y;
                _walk_state = (uint16_t)dir;
                _frame = _dir_frame_low[dir];
                _frame_cnt = 3;
            }
        } else if ((uint16_t)(((_walk_state & 1u) ? -1 : 1) +
                              (int16_t)_walk_state) == dir) {
            /* A7DB..A816: the reverse gate re-aims the walk */
            _walk_state = (uint16_t)dir;
            if (dir < 2u) {
                _frame = 0x13;
                _frame_cnt = 3;
            }
        } else {
            _walk_step(_walk_state & 3u); /* A7C6 */
        }
    } else if (!(_walk_state & 0x8000u)) {
        _walk_step(_walk_state & 3u); /* the stepper runs on */
    }
    /* at rest + no key: nothing (A7C3) */
}

/* --- the options (the A5DE action block) -------------------------------- */

/** The action key at rest cycles the _cell's counter, once per press (the
 * original spins on the held key). */
static void _cycle_option(void)
{
    uint32_t idx = _cell_opts[_cell][0], cnt = _cell_opts[_cell][1];
    if (idx < 4u && cnt) {
        _opt_counter[idx] = (uint8_t)(_opt_counter[idx] + 1u);
        if (_opt_counter[idx] == cnt) _opt_counter[idx] = 0;
    }
}

/* --- the name screen ----------------------------------------------------- */

/** name_screen_anim_frame (1000:ACB9): the _slot advances +3 every 5 ticks
 * (a 0 table byte wraps to _slot 0); the pan := (byte & 0xF) - 4.  Its two
 * draws are the two humans (1000:ADB6 -> sprite_stage_rows), which the port
 * makes in _draw_humans: the 20x20 strips at _code2 + 0x15400 / + 0x17C00,
 * source column 0x9E - nibble, into the name page's 0x000 / 0xA00 windows or
 * (room) page rows 136.. through ADB6's [0x6267]-clear offset.  (An earlier
 * note here called them "decor sprites ... not ported": that was stale - the
 * routine below is their port.) */
static void _anim_frame(void)
{
    _slot_cnt = (uint16_t)(_slot_cnt - 1u);
    if (_slot_cnt & 0x8000u) {
        _slot_cnt = 5;
        /* acc5..acd2: the +3 step runs only while [0x6267] is set; on the
         * room path the _slot stays parked (the board-stack holds).  Either
         * way the 0 terminator wraps to _slot 0. */
        uint32_t s = _slot;
        if (_name_screen) s += 3u;
        if (s >= 61u || _slot_tab[s] == 0u) s = 0u; /* the terminator */
        _slot = s;
    }
    if (_name_screen) _scroll = (uint16_t)((_slot_tab[_slot] & 0x0Fu) - 4u);
}

/** @brief Open the high-score name entry screen. */
static void _name_screen_enter(void)
{
    _name_screen = 1;
    _page_draw = 0x2800; /* [0x5AC4] := 0x2800 (AF92)  */
    _page_disp = 0x7D00;
    _split_row = 0x88; /* [0x5B0A] := 0x88: the split */
    /* the transition's two strip blits (AFBD..AFDF): the room's LEFT view
     * (_code2 rows 0..135) onto the window base, and the humans' stage
     * (_code2+0x15400, 56 rows — the strip bank 0x1540 PARAGRAPHS in) onto
     * the fixed band at absolute 0. */
    if (_code2 && _code2_size >= 0x1E000u) {
        /* AF92's order is load-bearing: the stage strip first (rows 0..55),
         * then the character (band rows 13..39), then the main art (rows
         * 40..175, starting just below him).  Drawing the strip after the
         * character — as an earlier revision did — paints over him. */
        _planarise_rows(_code2 + 0x15400u, _code2_size - 0x15400u, 0, 56u);
        /* afcf: the character draw runs here, with [0x5AC4] held 0 — the
         * 0x820-band branch in _draw_character fires and the walked-in
         * human lands on the fixed band's hill (screen rows 149..175). */
        _draw_character();
        _planarise_rows(_code2, _code2_size, 0x2800, 136u);
    }
    _draw_humans();
    _draw_name_line();
    _phase = FE_NAME;
}

/** --- the phases ---------------------------------------------------------- */
static void _start_exit(void)
{
    /* the event loop's exit tail (AA86..): the fade to the 0x48BC ramp
     * (black) div 0x23 — the original then resets the video mode, which
     * the port's level flow owns. */
    palette_build_fade(0, 256, 0x23, _ramp0);
    _phase = FE_EXITFADE;
}

/** @brief The RESTORE/LOAD slot keyboard: pick and load a save. */
static void _do_load(void)
{
    uint8_t *textSpr = NULL;
    video_set_mode(VIDEO_MODE_X); /* frontend_room_setup: reset_display_mode
                                        * (1000:82c6) — the 320x192 field   */
    _code2 = res_load_chunk("LBM\\CODE2.RAW", &_code2_size, NULL);
    _codehum = res_load_chunk("APL\\CODEHUM.SPR", &_codehum_size, NULL);
    _codes = res_load_chunk("LEVELS\\CODES.BIN", &_codes_size, NULL);
    _codeb1 = res_load_chunk("APL\\CODEB1.SPR", &_codeb1_size, NULL);
    _codeb2 = res_load_chunk("APL\\CODEB2.SPR", &_codeb2_size, NULL);
    _restore = res_load_chunk("APL\\RESTORE.SPR", &_restore_size, NULL);
    textSpr = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(textSpr);

    /* the room's opening state (frontend_room_setup's seeds) */
    _page_draw = 0;
    _page_disp = 0x7D00;
    _split_row = 200; /* [0x5B0A] := 0xC8 (no split)  */
    _cell = 0;
    _walk_state = 0xFFFF;
    _char_x = 0x1D2;
    _char_y = 0xA0;
    _frame = 0x13;
    _frame_cnt = 3;
    _slot = 0x12;
    _slot_cnt = 5;
    /* _opt_counter is deliberately NOT reseeded: the image ships the four
     * bytes zero and the setup never writes them (0x5E5B..0x5E79), so the
     * settings survive every re-entry into the room for the whole run. */
    _name_screen = 0;
    _fire_latch = 0;
    _name_reset();
    _set_scroll();

    if (_code2 && _code2_size >= 0x1E000u + 768u) {
        /* the palette: the caller computes the source segment as
         * [0x625D] + 0x1E00 PARAGRAPHS = byte offset 0x1E000 — the
         * file's tail, right after the 640x192 image (0x1E000 bytes).
         * palette_load_6bit (1000:8D24) masks each byte & 0x3F. */
        palette_load_working(_code2 + 0x1E000u);
        /* 200 rows (room_backdrop_build 1000:AF07, CX=0xC8): rows 192..199
         * are the chunk's palette tail, then the pool — copied from
         * `_code2_size` here.  reset_display_mode's 320x192 field (1000:82C6,
         * called at 1000:A544) ends on row 191, so the original never shows
         * them either. */
        _planarise_rows(_code2, _code2_size, 0, 200u);
        _planarise_rows(_code2, _code2_size, 0x7D00, 200u);
        /* the setup's 2-_frame loop (A5A1..A5DB): the full _frame — the
         * badges, the character, the name line — drawn on BOTH pages
         * before the fade, so the fade-in holds a complete static _frame
         * while the _compose alternates (drawing one page only made the
         * character flicker through the fade).  The original then waits
         * on the fade with no further draws. */
        for (uint32_t f = 0; f < 2u; f++) {
            _draw_option_sprites();
            _draw_character();
            _draw_name_line();
            _compose(1);
        }
        palette_build_fade(0, 256, 0x23, palette_working());
        _phase = FE_FADEIN;
    } else {
        _phase = FE_ROOM; /* the no-data path: no palette, no backdrop */
    }
}

/** @brief One frontend tick: the room, the name screen or the load. */
int32_t frontend_tick(const InputState *in)
{
    switch (_phase) {
    case FE_IDLE:
        _phase = FE_LOAD;
        /* fall through: the load takes this first tick */
    case FE_LOAD: _do_load(); return 1;
    case FE_FADEIN:
        _compose(1);
        if (!palette_fade_active()) _phase = FE_ROOM;
        return 1;
    case FE_ROOM: {
        /* the _frame web's head (1000:A5E8): the MUSIC board's counter
         * [0x5E5B] stops the music when non-zero, resumes it when zero */
        audio_music_gate(frontend_option(FRONTEND_OPT_MUSIC));

        if ((in->joyBits & 0x80u) && (_walk_state & 0x8000u) && !_fire_latch) {
            _cycle_option();
            _fire_latch = 1;
        }
        if (!(in->joyBits & 0x80u)) _fire_latch = 0;

        _set_scroll();
        _draw_option_sprites(); /* place_4_sprites_to_vga (A684) */
        _draw_character();
        _draw_name_line();
        _anim_frame(); /* the walk loop's acb9: keeps the
                                         * board-stack's middle pair in _vram
                                         * (the event loop's idle) */
        _draw_humans();
        _room_walk(in);
        _compose(1);

        /* the tail's x-dispatch (A890..) */
        if (_char_x == 0x280u) {
            (void)_code_match(); /* the right-edge exit ignores it */
            _start_exit();
        } else if (_char_x == 0x108u) {
            if (_scroll != 0u) _phase = FE_PAN; /* the pan-out first */
            else _name_screen_enter();
        }
        return 1;
    }
    case FE_PAN:
        /* the event loop's head (AA2C..): the _scroll runs down to 0 */
        if (_scroll == 0u) {
            _name_screen_enter();
            return 1;
        }
        _scroll = (uint16_t)(_scroll - 1u);
        _draw_character();
        _compose(1);
        return 1;
    case FE_NAME:
        _anim_frame();
        _draw_humans(); /* the fixed band's two walkers */
        _draw_name_line();
        _compose(0); /* the name screen is single-buffered */
        if (in->lastKey) {
            uint8_t k = in->lastKey;
            /* AA4E/AA50: the event loop reads AND clears [0x48A3] — one
             * consume per key event; a held key re-fires only via the
             * (BIOS/SDL) typematic repeat. Without the clear one backspace
             * tap spans several ticks and erases the whole name. */
            ((InputState *)in)->lastKey = 0u;
            if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') {
                _name_addchar(k);
            } else if (k == 0x1Bu) { /* ESC: clear the name, no exit */
                memset(_name_buf, 0, sizeof _name_buf);
                _draw_name_line();
            } else if (k == 0x0Du) { /* Enter: the match gates the exit */
                if (_code_match()) _start_exit();
            } else if (k == 0x0Eu) {
                _name_erase();
            }
        }
        return 1;
    case FE_EXITFADE:
        /* The fade holds the _frame the screen last presented.  This used to
         * pass 1, which swapped _page_draw to 0x7D00 - the ROOM's second page,
         * where the setup's 2-_frame loop (1000:A5A1) had drawn the seeded
         * "DARWIN".  The name screen is single-buffered (its cursor+present
         * double-swap cancels, so it composes with 0), so swapping here put
         * the stale room page on screen after the first fade tick: a correct
         * code started the right level, but the old DARWIN showed through the
         * fade-out.  Keep the name screen's page (0x2800). */
        _compose(0);
        if (!palette_fade_active()) return 0; /* the room has exited */
        return 1;
    }
    return 1;
}
