/* frontend.c — the front-end room: the walkable level-select / game-start
 * menu (subsystem 8; frontend_room_setup 1000:A48E + the frame web +
 * level_select_event_loop 1000:AA2C), ported from re_map.md -> "The
 * front-end room (the level-select / game-start menu) — the core decoded
 * (2026-10-26)" and its "Closed since" list.
 *
 * THE ROOM.  A 640-px-wide walkable side view on the original's unchained
 * mode-X space: two pages at byte offsets 0 / 0x7D00, 160 bytes per row
 * (byte mode: 1 address byte = 4 pixels, one per plane; 160*4 = 640), the
 * scroll word [0x0] (set_scroll_x 1000:AA05) picking the 320-px window the
 * CRTC scans out.  The name/code screen is another window at page offset
 * 0x2800 (name_screen_show 1000:AF92).  Every present composes the window
 * into the flat page 0 — game_main's videoPresent shows it (the attract's
 * contract).
 *
 * THE SELECT.  The typed name is matched against levels\codes.bin (160
 * NUL-terminated strings, string i = level i, #1 "DARWIN" — menu_code_match
 * 1000:AB39); the matched index [0x5F09] becomes the level: entry's PUSH AX
 * at 004B / POP AX at 00BA feed [0x22].  The exits: walk off the right edge
 * (x=0x280, via cell 6's R rail) — the match runs, the result is NOT tested,
 * the fade happens either way; or Enter on the name screen, which exits
 * only on a match (a miss resets the name to DARWIN and stays).  ESC clears
 * the name and never exits (0x5F11's act0); Backspace erases the last
 * char; A-Z 0-9 space type.  There is no key path back to the attract.
 *
 * THE WALK.  [0x5ACE] bits -> dir (bit2 left=0 > bit3 right=1 > bit0 up=2 >
 * bit1 down=3, update_object_fields 1000:A75B).  At rest a dir key tests
 * the rail table at 202F:0x5ECD (7 cells x 4 words (L,R,U,D) =
 * (target_cell << 12) | target_coord); a nonzero rail arms the walk
 * (state := dir, frame := the cycle low, the origin published to
 * [0x5F05]/[0x5F07]).  Mid-walk, only the opposite dir re-aims (the +-1
 * reverse gate); otherwise the steppers (walk_step_horizontal 1000:A8D3 /
 * _vertical 1000:A963 via the presets at 0x5EBD) advance 1 px per tick
 * toward the rail's target, arriving on coord equality (cell := the high
 * nibble, state := 0xFFFF = at rest).  The floor table at 202F:0x5E7E pins
 * y for x < 0x138.  The tail dispatches on x: 0x280 -> the right-edge exit,
 * 0x108 -> the name screen's pan-out.
 *
 * THE OPTIONS.  The ACTION key at rest cycles the cell's option counter
 * ([0x5E46] = (struct-idx, count); the structs' +7 byte at 0x5E5B/0x5E65/
 * 0x5E6F/0x5E79 IS the badge's frame, so the setting and the picture are
 * one byte).  One cycle per press (the original spins on the held key).
 * The counters PERSIST for the run: frontend_room_setup (1000:A48E) never
 * writes 0x5E5B..0x5E79 and the image ships them zero, so re-entering the
 * room must not reset them.  The four boards (the signs are painted in
 * code2.raw's backdrop; the sprites are the badges):
 *   cell 2 = LEVEL, EASY/NORMAL/HARD -> [0x5E65]: entry (1000:009F) maps
 *            0/1/2 to the 12/10/8 tribe and render_level_frame (1000:958C)
 *            picks the time limit A/B/C;
 *   cell 3 = MUSIC, ON/OFF -> [0x5E5B]: the frame web (1000:A5E8) stops
 *            the music when non-zero and resumes it when zero;
 *   cell 5 = ANIMATIONS, ON/OFF -> [0x5E79]: cd_file_op (1000:AFFD) RETs
 *            at once when non-zero — the .ANI cutscenes are off;
 *   cell 6 = RESTORE GAME, OFF/1..20 -> [0x5E6F]: the exit hands it to
 *            entry 1000:004C in BL, which appends 'A'+n-1 to
 *            "\humans\cdrom\save" and loads that save slot.
 * frontend_option() is the read side's one accessor.
 *
 * DEVIATIONS (all port-only, each waiting on its named step; the
 * authoritative list is port_deviations.md — D-01/D-19/D-20 here):
 *  - the INT 60h calls (music_resume/stop 83F7/8389, the cmd-0xA pump) are
 *    not made: the audio backend is an M5 stub.  audio_musicGate ports the
 *    gate and the [0x40BB] latch; only the tune handle is missing (M6).
 *  - the CRTC split ([0x5B0A]) is modelled in the compose: rows >= the
 *    split display the FIXED band from absolute vram 0, unpanned (the
 *    level's status bar and the name screen's human stage both live
 *    there); the original latches the same at vsync.
 *  - the name line is the AB8D render proper (2026-10-27): the typed
 *    name goes to the arena via draw_text and the WM2 bit-7 copy loop
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
#include "game.h"
#include <stdlib.h>
#include <string.h>

/* --- the planar space (A000, unchained, byte mode) -------------------- */

static uint8_t vram[65536][4];   /* [address][plane]: 1 byte = 4 pixels   */
static unsigned pageDraw, pageDisp;   /* [0x5AC4]/[0x5AC6] — page bases   */
static unsigned scroll;               /* [0x0]: the front-end scroll      */

/* --- the shipped tables, verbatim -------------------------------------- */

/* 202F:0x5ECD — 7 cells x 4 words (L,R,U,D):
 * (target_cell << 12) | target_coord (x for L/R, y for U/D). */
static const uint16_t rails[7][4] = {
    {0x0108, 0x6208, 0x1037, 0x0000},
    {0x2190, 0x0000, 0x4020, 0x00A0},
    {0x316A, 0x11D2, 0x0000, 0x0000},
    {0x0000, 0x2190, 0x0000, 0x0000},
    {0x0000, 0x521B, 0x0000, 0x1037},
    {0x41D2, 0x0000, 0x0000, 0x0000},
    {0x01D2, 0x0280, 0x0000, 0x0000},
};

/* 202F:0x5E46 — per cell (struct-idx, count); count 0 = no option.
 * cell 2 = LEVEL (3 states), cell 3 = MUSIC (2), cell 5 = ANIMATIONS (2),
 * cell 6 = RESTORE GAME (21 = 0x15). */
static const uint8_t cellOpts[7][2] = {
    {0, 0}, {0, 0}, {1, 3}, {0, 2}, {0, 0}, {3, 2}, {2, 0x15},
};

/* 202F:0x5EC5 — the walk cycle's frame low per dir; the presets' cycle
 * high (the CX at 0x5EBD's entries) alongside. */
static const uint16_t dirFrameLow[4]  = {0x0B, 0x03, 0x14, 0x16};
static const uint16_t dirCycleHigh[4] = {0x13, 0x0B, 0x17, 0x13};

/* 202F:0x5E7E — the floor table, indexed (x - 0xFD); y = byte - 0x18. */
static const uint8_t floorTab[0x3B] = {
    0xB1,0xB1,0xB0,0xB0,0xAF,0xAF,0xAF,0xAE,0xAE,0xAE,0xAE,0xAE,0xAE,
    0xAE,0xAE,0xAE,0xAE,0xAE,0xAE,0xAF,0xAF,0xAF,0xB0,0xB0,0xB1,0xB1,
    0xB2,0xB2,0xB2,0xB3,0xB3,0xB3,0xB4,0xB4,0xB4,0xB4,0xB4,0xB5,0xB5,
    0xB5,0xB5,0xB5,0xB5,0xB5,0xB6,0xB6,0xB6,0xB6,0xB6,0xB6,0xB6,0xB6,
    0xB6,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,
};

/* 202F:0x61A3 — the anim slot table: 60 data bytes, then the 0x00 wrap
 * terminator at [60] (verbatim from the image; an earlier embed had a
 * duplicated 0x99 there, which made the +3 slot walk run past the table
 * and never wrap — the humans' cut column and the name screen's pan both
 * went garbage-derived: the humans vanished behind the range guard and
 * the window scrolled wildly). */
static const uint8_t slotTab[61] = {
    0x88,0x88,0x88,0x77,0x77,0x77,0x66,0x66,0x66,0x55,0x55,0x66,
    0x44,0x55,0x66,0x44,0x55,0x66,0x44,0x55,0x66,0x55,0x55,0x66,
    0x66,0x66,0x66,0x77,0x77,0x77,0x88,0x88,0x88,0x99,0x99,0x99,
    0xAA,0xAA,0xAA,0xBB,0xBB,0xAA,0xCC,0xBB,0xAA,0xCC,0xBB,0xAA,
    0xCC,0xBB,0xAA,0xBB,0xBB,0xAA,0xAA,0xAA,0xAA,0x99,0x99,0x99,
    0x00,   /* [60] = the wrap terminator, verbatim from the image      */
};

/* --- the module state (the DS analogs in comments) -------------------- */

typedef enum FePhase {
    FE_IDLE = 0, FE_LOAD, FE_FADEIN, FE_ROOM, FE_PAN, FE_NAME, FE_EXITFADE
} FePhase;

static FePhase  phase = FE_IDLE;
static uint16_t cell;            /* [0x4220]: the rail node              */
static uint16_t walkState;       /* [0x4222]: bit15 = at rest, 0..3 walk */
static uint16_t charX, charY;    /* [0x4224]/[0x4226]: the world x/y     */
static uint16_t frame, frameCnt; /* [0x4228]/[0x422A]                    */
static uint16_t pubX, pubY;      /* [0x5F05]/[0x5F07]: the walk origin   */
static char     nameBuf[15];     /* 0x61E0: 14 chars + the 0x61EE NUL   */
static unsigned codeIndex;       /* [0x5F09]: the matched code's index   */
static uint8_t  optCounter[4];   /* the structs' +7 byte (0x5E5B..0x5E79)
                                  * = FRONTEND_OPT_*; persists for the run */
static int      fireLatch;        /* the action key's held-spin           */
static uint16_t slotCnt;         /* [0x619F]: the anim countdown (5)     */
static unsigned slot;            /* [0x61A1]: the anim slot (parked 0x12)*/
static uint8_t  nameScreen;      /* [0x6267]: the name-screen flag       */

static uint8_t *code2, *codehum, *codes;
static uint8_t *codeb1, *codeb2, *restore;
static uint32_t code2Size, codehumSize, codesSize;
static uint32_t codeb1Size, codeb2Size, restoreSize;
static uint8_t  arena[16000];    /* the font arena (the [0x1E20] analog) */
static const uint8_t ramp0[768]; /* the 0x48BC ramp: zeroed = black     */
static unsigned splitRow = 200;  /* [0x5B0A]: the CRTC split (0x88 on the
                                   * name screen — the fixed band below) */

int frontend_init(const OSystem *osys) { (void)osys; return 0; }

unsigned frontend_selectedLevel(void) { return codeIndex; }

/* entry's JMP LAB_002B: the room's setup runs again from the top (the
 * counters persist — frontend_room_setup seeds none of them).  The next
 * frontend_tick re-loads the assets (FE_LOAD). */
void frontend_restart(void) { phase = FE_LOAD; fireLatch = 0; }

/* The four option counters — the read side's one accessor.  Consumers:
 * level.c (LEVEL: the time-limit pick + the 12/10/8 tribe), audio.c (MUSIC:
 * the frame web's gate), scene.c (ANIMATIONS: cd_file_op's early RET).  The
 * RESTORE GAME slot is carried out here and consumed by entry 1000:004C's
 * save load, which the port's save.c does not have yet (M6). */
unsigned frontend_option(unsigned idx)
{
    return idx < 4u ? optCounter[idx] : 0u;
}

/* [0x4220] — the rail node, exposed for the option read-side test's walk
 * legs (the same role as attract_phaseForProbe). */
unsigned frontend_cellForProbe(void) { return cell; }

/* the name screen's phase and typed buffer, for the input-law tests */
int frontend_nameScreenForProbe(void) { return phase == FE_NAME; }
const char *frontend_nameForProbe(void) { return nameBuf; }

/* --- the VRAM primitives ---------------------------------------------- */

static void put_pixel(unsigned rowBase, unsigned px, uint8_t v)
{
    vram[(rowBase + (px >> 2)) & 0xFFFFu][px & 3u] = v;
}

/* blit_planar_strip (1000:AEAD): planarise chunky 640-px rows into the
 * page at `base` — dest[base + r*160 + c] plane p = src[r*640 + c*4 + p].
 * `size` is the whole loaded chunk, so the 200-row backdrop copy reads the 8
 * rows past the 192-row image out of its palette tail, as `1000:AEDB`'s row
 * loop does; only the bytes past the chunk (the original's uninitialised
 * pool) are written as zeros.  The room's 320x192 field never shows them. */
static void planarise_rows(const uint8_t *src, uint32_t size,
                           unsigned base, unsigned rows)
{
    for (unsigned r = 0; r < rows; r++)
        for (unsigned c = 0; c < 160u; c++) {
            unsigned a = (base + r * 160u + c) & 0xFFFFu;
            uint32_t off = (uint32_t)r * 640u + (uint32_t)c * 4u;
            if (off + 3u < size) {
                vram[a][0] = src[off];
                vram[a][1] = src[off + 1u];
                vram[a][2] = src[off + 2u];
                vram[a][3] = src[off + 3u];
            } else {
                vram[a][0] = vram[a][1] = 0u;
                vram[a][2] = vram[a][3] = 0u;
            }
        }
}

/* The present: the 200-row window, flat — pixel x of the window = the
 * world pixel (scroll + x), i.e. set_display_ptr's byte start (882D:
 * [0x5AC4] + [0x2]*88 + [0x0]/4) plus the pel-pan phase ([0x0] & 3 —
 * 8848/884a).  Rows >= splitRow ([0x5B0A]) are the CRTC split's FIXED
 * band: they display from ABSOLUTE vram 0 (the level's status bar and the
 * name screen's human stage both live there), unpanned.  The room is
 * double-buffered (present_page 1000:8809 swaps after showing); the name
 * screen's cursor+present double-swap cancels, so it passes 0. */
static void compose(int swapPages)
{
    uint8_t *page = video_page(0);
    if (page) {
        for (unsigned y = 0; y < 200u; y++) {
            uint8_t *d = page + y * 320u;
            if (y < splitRow) {
                unsigned rowBase = pageDraw + y * 160u;
                for (unsigned x = 0; x < 320u; x++) {
                    unsigned w = scroll + x;   /* the world pixel */
                    d[x] = vram[(rowBase + (w >> 2)) & 0xFFFFu][w & 3u];
                }
            } else {                     /* the fixed band: from vram 0 */
                unsigned rowBase = (y - splitRow) * 160u;
                for (unsigned x = 0; x < 320u; x++)
                    d[x] = vram[(rowBase + (x >> 2)) & 0xFFFFu][x & 3u];
            }
        }
    }
    if (swapPages) {
        unsigned t = pageDraw; pageDraw = pageDisp; pageDisp = t;
    }
}

/* place_4_sprites_to_vga (1000:A684) — the option-value badges at the
 * MUSIC / LEVEL / RESTORE GAME / ANIMATIONS signs.  The entry table at
 * 202F:0x5E54
 * (10 bytes apart, verbatim): {u16 dest, u16 srcOff, u8 w, u8 h,
 * u8 cell, u16 &srcSeg} — and the +7 `cell` field IS the option counter
 * (the "option structs" and this table are the same bytes: 0x5E5B /
 * 0x5E65 / 0x5E6F / 0x5E79).  The sprite = w*h chunky bytes at
 * srcOff + cell*w*h, nonzero-transparent, at the page byte `dest`
 * (+[0x5AC4], 160-pitch).  The shipped files close the cell counts:
 * codeb1 6144 = 4x(48x32) (MUSIC's ON/OFF + ANIMATIONS' ON/OFF), codeb2
 * 7680 = 3x(80x32) (LEVEL's EASY/NORMAL/HARD), restore 32256 =
 * 21x(48x32) (RESTORE GAME's OFF/1..20); the 4th entry is codeb1's cells
 * 2..3 at srcOff 0xC00. */
static void draw_option_sprites(void)
{
    static const struct {
        uint16_t dest, srcOff; uint8_t w, h, cellIdx;
    } e[4] = {
        {0x1D14, 0x0000, 48, 32, 0},  /* MUSIC        (2, codeb1 cells 0..1) */
        {0x1C80, 0x0000, 80, 32, 1},  /* LEVEL        (3, codeb2)            */
        {0x5BBC, 0x0000, 48, 32, 2},  /* RESTORE GAME (21, restore.spr)      */
        {0x0F88, 0x0C00, 48, 32, 3},  /* ANIMATIONS   (2, codeb1 cells 2..3) */
    };
    const uint8_t *file[4] = {codeb1, codeb2, restore, codeb1};
    const uint32_t size[4] = {codeb1Size, codeb2Size, restoreSize, codeb1Size};
    for (unsigned i = 0; i < 4u; i++) {
        const uint8_t *src = file[i];
        if (!src) continue;
        unsigned w = e[i].w, h = e[i].h;
        unsigned cell = optCounter[e[i].cellIdx];
        unsigned off = e[i].srcOff + cell * w * h;
        if (off + w * h > size[i]) continue;
        /* the stager's inner pass (A6CB..A6F5) FIRST patches the badge
         * into the LOADED code2.raw image itself — ES := [0x625D] +
         * dest/4 PARAGRAPHS = the chunky byte offset dest*4, w bytes per
         * row at +640 stride, nonzero-transparent — so the character's
         * backdrop-restored box carries the badge and both the occlusion
         * and the 2-col page-alternation trail stay clean.  The cells'
         * transparent masks are uniform within each badge's cell group,
         * so cycling overwrites cleanly. */
        if (code2 && (uint32_t)e[i].dest * 4u < code2Size &&
            (unsigned)e[i].dest * 4u + (h - 1u) * 640u + w < code2Size) {
            unsigned pOff = (unsigned)e[i].dest * 4u;
            for (unsigned r = 0; r < h; r++)
                for (unsigned c = 0; c < w; c++) {
                    uint8_t v = src[off + r * w + c];
                    if (v) code2[pOff + r * 640u + c] = v;
                }
        }
        unsigned destRow = e[i].dest / 160u, destCol = e[i].dest % 160u;
        if (destCol * 4u + w > 640u) continue;
        for (unsigned r = 0; r < h; r++) {
            unsigned base = pageDraw + (destRow + r) * 160u;
            for (unsigned c = 0; c < w; c++) {
                uint8_t v = src[off + r * w + c];
                if (v) put_pixel(base, destCol * 4u + c, v);
            }
        }
    }
}

/* name_screen_anim_frame's two human draws (via name_entry_render's ADB6
 * entry): a 20x20 cut of code2's human strip bank — rows 0..19 at column
 * (0x9E - nibble), the nibble from the NEXT TWO slot bytes — copied to
 * the SAME x; on the name screen to the fixed band (rows 0..19/16..35),
 * on the room screen to [0x5AC4]+0x5500 + those (page rows 136..155/
 * 152..171 — the middle pair of the four-human stack holding the board).
 * The stager's OVERLAY pass (AE6E..AE9E) then composites codehum frame 2
 * (CS:[0xadb4]=0, 16 rows of 16 bytes, nonzero-only) at scratch rows 1..16,
 * cols 2..17 — ported here as the same overlay. */
static void draw_humans(void)
{
    if (!code2 || code2Size < 0x17C00u + 20u * 640u) return;
    static const unsigned bankBase[2] = {0x15400u, 0x17C00u};
    static const unsigned destRow[2] = {0u, 16u};
    for (unsigned h = 0; h < 2u; h++) {
        unsigned nibble = slotTab[(slot + 1u + h) % 61u] & 0x0Fu;
        unsigned cx = 0x9Eu - nibble;             /* 0x9E - nibble */
        /* ADB6's flag pick: [0x6267] set -> DI used directly (the fixed
         * band's rows 0..19/16..35); clear -> DI += 0x5500 (page row 136)
         * + [0x5AC4] — the room screen: page rows 136..155/152..171 at the
         * same world x, the art's empty gap between the painted rail-
         * gripper (rows 112..138) and the floored one (166..188).  It is
         * exactly this middle pair that turns the screen's hanging human
         * into the FOUR-human stack holding the board while the 320-px
         * window slides across the 640-px room during the pan. */
        for (unsigned r = 0; r < 20u; r++) {
            unsigned srcRow = bankBase[h] + r * 640u;
            unsigned base = nameScreen
                ? (destRow[h] + r) * 160u
                : pageDraw + (136u + destRow[h] + r) * 160u;
            for (unsigned c = 0; c < 20u; c++) {
                uint8_t v = code2[srcRow + cx + c];
                if (v) put_pixel(base, cx + c, v);
            }
        }
        if (codehum && codehumSize >= 16u * 16u) {
            for (unsigned r = 0; r < 16u; r++) {
                /* the overlay's scratch offset CS:[0xaeab] is 0 for the
                 * anim draws (ada2's room draw leaves it 0) — the sprite
                 * lands at box rows 0..15, cols 2..17 (0x5F65 = base+2) */
                unsigned base = nameScreen
                    ? (destRow[h] + r) * 160u
                    : pageDraw + (136u + destRow[h] + r) * 160u;
                for (unsigned c = 0; c < 16u; c++) {
                    uint8_t v = codehum[r * 16u + c];
                    if (v) put_pixel(base, cx + 2u + c, v);
                }
            }
        }
    }
}

/* --- the name ----------------------------------------------------------- */

/* name_entry_blit (1000:AB8D) — the typed name IS displayed: the last
 * claim ("never displayed, live-verified") misidentified this routine's
 * purpose; its callers were verified across the room path (addchar,
 * backspace, ESC-clear, the match's miss path, the room backdrop build)
 * and the loop's raw bytes settle the shape: 10 rows x 40 copy-pairs —
 * the ARENA (draw_text already wrote the glyphs at rows 4..13, bytes
 * 18..57) read at 0x152+80/row, written to the page at [0x5AC4]+0x38F5
 * +160/row under vga_set_write_mode2's bit-mask 0x80 (299b: OUT DX,AX
 * with AH=0x80).  Each write sets the color-index TOP bit of the 4 dest
 * pixels from the arena byte's bits 3..0 — the bright-variant palette
 * (0x80+n) the dark bar's pixels sit in.  Ported verbatim. */
static void draw_name_line(void)
{
    /* AB8D's exact sequence: clear the arena's first 1600 bytes, draw the
     * name into it (draw_text centres at x 0x98, rows 0..15 — the glyphs
     * land at arena rows 4..13), then the WM2 copy: 10 rows x 40 bytes,
     * source arena 0x152+80/row, dest [0x5AC4]+0x38F5+0xA0/row — AB8D
     * writes 40 bytes (DI +40) then skips 0x78 (120): 40+0x78 = 0xA0. */
    memset(arena, 0, 1600u);
    font_drawText(arena, nameBuf, 0x98u, 0);
    for (unsigned r = 0; r < 10u; r++)
        for (unsigned c = 0; c < 40u; c++) {
            uint8_t v = arena[0x152u + r * 80u + c];
            unsigned addr = (pageDraw + 0x38F5u + r * 0xA0u + c) & 0xFFFFu;
            for (unsigned p = 0; p < 4u; p++)
                vram[addr][p] = (uint8_t)((vram[addr][p] & 0x7F)
                                          | ((v >> p) & 1u) << 7);
        }
}

/* name_entry_init (1000:AAC9): the "DARWIN" seed AND four zero words at
 * 0x61E6..0x61ED - the whole 14-byte buffer, not just the terminator. */
static void name_reset(void)
{
    memset(nameBuf, 0, sizeof nameBuf);
    memcpy(nameBuf, "DARWIN", 6u);
    draw_name_line();
}

/* name_entry_addchar (1000:AB1E): append at the first NUL.  The BX = 13 walk
 * writes the 14th character at index 13; a full buffer overwrites index 13.
 * (The original's buffer is 0x61E0..0x61ED, terminated by the fixed zero at
 * 0x61EE; nameBuf[14] is that zero.) */
static void name_addchar(uint8_t c)
{
    unsigned i = 0;
    int bx = 13;

    while (nameBuf[i]) {
        bx--;
        if (bx < 0) break;
        i++;
    }
    nameBuf[i] = (char)c;
    draw_name_line();
}

/* menu_action_backspace (1000:AB00): erase the last char. */
static void name_erase(void)
{
    int i = 13;
    while (i > 0 && nameBuf[i] == 0) i--;
    nameBuf[i] = 0;
    draw_name_line();
}

/* menu_code_match (1000:AB39): walk codes.bin's NUL strings against the
 * name; a full match returns nonzero with codeIndex = the 1-based index
 * (string i = level i); exhausted -> reset DARWIN, codeIndex = 161 (the
 * original's own edge: [0x5F09] counts past every miss). */
static int code_match(void)
{
    codeIndex = 1;
    if (!codes || codesSize < 2u) {
        return 1;   /* the no-data path: DARWIN at index 1 */
    }
    const char *p = (const char *)codes;
    const char *end = (const char *)codes + codesSize;
    unsigned remaining = 160u;   /* [0x6193] = 0xA0 */
    while (remaining-- && p < end) {
        const char *c = p, *n = nameBuf;
        while (*n && *c == *n) { c++; n++; }
        if (*c == *n) return 1;
        codeIndex++;
        p += strlen(p) + 1;
    }
    name_reset();
    return 0;
}

/* --- the character draw (room_draw_character 1000:AD40) ---------------- */

/* The floor adjust (the tail A85A..A885) + the record publish + the draw.
 * The frame is the record's unless x < 0x10B (the idle pose), the draw x
 * clamped >= 0x108.  The box (sprite_stage_rows 1000:AE39 +
 * sprite_blit_4plane 1000:ADC8) is the BACKDROP RESTORED FROM code2.raw
 * ITSELF — the stager copies code2's own 20-byte strips at the character's
 * position (DS := code2seg + (y-1)*0x28 paragraphs, SI := x, +0x26C after
 * 20 bytes = one 640-byte row per strip) into the scratch, then overlays
 * only the sprite's NONZERO bytes (box rows 1..24, cols 2..17); the blit
 * writes all 27 rows x 20 cols unconditionally.  The 2-col backdrop
 * margins are what make the page alternation seam-free: the 2-px-per-
 * 2-frames trail lands on pure backdrop. */
static void draw_character(void)
{
    uint16_t rx = charX, ry = charY;
    if (rx < 0x138u) {
        if (rx >= 0x108u) ry = (uint16_t)(floorTab[rx - 0xFDu] - 0x18u);
        else ry = 0x96u;
    }
    if (!codehum || codehumSize < 8064u) return;
    if (!code2  || code2Size < 0x1E000u) return;
    uint16_t dframe = (rx < 0x10Bu) ? 0x13u : frame;
    if (dframe < 2u || dframe > 22u) return;
    uint16_t dx = (rx < 0x108u) ? 0x108u : rx;
    const uint8_t *spr = codehum + (size_t)(dframe - 2u) * 384u;
    for (unsigned r = 0; r < 27u; r++) {
        unsigned srcRow = (unsigned)(ry - 1u) + r;
        /* AD78..AD7F: on the name screen ([0x6267]) the blit dest skips the
         * [0x5AC4] page add entirely (screen show holds [0x5AC4]=0 across
         * the call) and is forced to ABSOLUTE 0x820 — page rows 13..39 of
         * the fixed band, i.e. screen rows 149..175 at the char's own x:
         * the human standing on the small hill at the bottom right. */
        unsigned base = nameScreen ? 0x820u + r * 160u
                                   : pageDraw + srcRow * 160u;
        for (unsigned c = 0; c < 20u; c++) {
            uint8_t v = (srcRow < 192u)
                       ? code2[srcRow * 640u + dx + c] : 0u;
            if (r >= 1u && r < 25u && c >= 2u && c < 18u) {
                uint8_t s = spr[(r - 1u) * 16u + (c - 2u)];
                if (s) v = s;
            }
            put_pixel(base, dx + c, v);
        }
    }
}

/* --- the walk (update_object_fields 1000:A75B + the steppers) ----------- */

/* set_scroll_x (1000:AA05): y >= 0x64 -> min(x-0x92, 0x140), else 0x140. */
static void set_scroll(void)
{
    unsigned s = 0x140u;
    if (charY >= 0x64u) {
        s = (uint16_t)(charX - 0x92u);
        if (s >= 0x140u) s = 0x140u;
    }
    scroll = s;
}

/* walk_step_horizontal (1000:A8D3) / _vertical (1000:A963), entered via
 * the presets at 0x5EBD: step = -1 left/up, +1 right/down; the frame
 * cycle and the rail-arrival tests verbatim. */
static void walk_step(unsigned dir)
{
    int16_t  step = (dir == 1u || dir == 3u) ? 1 : -1;
    unsigned high = dirCycleHigh[dir];
    uint16_t rail = rails[cell][dir];
    uint16_t target = rail & 0x0FFFu;

    if (dir < 2u) {                                   /* A8D3 */
        if (frame != 0x13u) charX = (uint16_t)(charX + step);
        frameCnt--;
        if (frameCnt & 0x8000u) {
            frameCnt = 3;
            frame = (uint16_t)(frame + 1u);
            if (frame >= high) frame = (uint16_t)(high - 8u);
        }
        if (charX == target) {
            cell = rail >> 12; walkState = 0xFFFF;
            frame = 0x13; frameCnt = 3;
        } else if (charX == pubX) {
            walkState = 0xFFFF; frame = 0x13; frameCnt = 3;
        }
    } else {                                          /* A963 */
        if (frame != 0x13u) charY = (uint16_t)(charY + step);
        step = (int16_t)-step;
        frameCnt--;
        if (frameCnt & 0x8000u) {
            frameCnt = 3;
            int flip = (frame == 0x13u);
            if (!flip) {
                frame = (uint16_t)(frame + step);
                flip = (frame == high);
            }
            if (flip) {
                step = (int16_t)-step;
                frame = (uint16_t)(high + step);
                frame = (uint16_t)(frame + step);
                frame = (uint16_t)(frame + step);
            }
        }
        if (charY == target) {
            cell = rail >> 12; walkState = 0xFFFF;
            frame = 0x13; frameCnt = 3;
        } else if (charY == pubY) {
            walkState = 0xFFFF; frame = 0x13; frameCnt = 3;
        }
    }
}

static void room_walk(const InputState *in)
{
    unsigned dir = 4;
    if      (in->joyBits & 0x04u) dir = 0;   /* left  — the cascade's order */
    else if (in->joyBits & 0x08u) dir = 1;   /* right                        */
    else if (in->joyBits & 0x01u) dir = 2;   /* up                           */
    else if (in->joyBits & 0x02u) dir = 3;   /* down                         */

    if (dir != 4u) {
        if (walkState & 0x8000u) {                    /* at rest: the rail */
            uint16_t rail = rails[cell][dir];
            if (rail) {                               /* A818..A854 */
                pubX = charX; pubY = charY;
                walkState = (uint16_t)dir;
                frame = dirFrameLow[dir]; frameCnt = 3;
            }
        } else if ((uint16_t)(((walkState & 1u) ? -1 : 1)
                             + (int16_t)walkState) == dir) {
            /* A7DB..A816: the reverse gate re-aims the walk */
            walkState = (uint16_t)dir;
            if (dir < 2u) { frame = 0x13; frameCnt = 3; }
        } else {
            walk_step(walkState & 3u);                /* A7C6 */
        }
    } else if (!(walkState & 0x8000u)) {
        walk_step(walkState & 3u);                    /* the stepper runs on */
    }
    /* at rest + no key: nothing (A7C3) */
}

/* --- the options (the A5DE action block) -------------------------------- */

/* The action key at rest cycles the cell's counter, once per press (the
 * original spins on the held key). */
static void cycle_option(void)
{
    unsigned idx = cellOpts[cell][0], cnt = cellOpts[cell][1];
    if (idx < 4u && cnt) {
        optCounter[idx] = (uint8_t)(optCounter[idx] + 1u);
        if (optCounter[idx] == cnt) optCounter[idx] = 0;
    }
}

/* --- the name screen ----------------------------------------------------- */

/* name_screen_anim_frame (1000:ACB9): the slot advances +3 every 5 ticks
 * (a 0 table byte wraps to slot 0); the pan := (byte & 0xF) - 4.  Its two
 * draws are the two humans (1000:ADB6 -> sprite_stage_rows), which the port
 * makes in draw_humans: the 20x20 strips at code2 + 0x15400 / + 0x17C00,
 * source column 0x9E - nibble, into the name page's 0x000 / 0xA00 windows or
 * (room) page rows 136.. through ADB6's [0x6267]-clear offset.  (An earlier
 * note here called them "decor sprites ... not ported": that was stale - the
 * routine below is their port.) */
static void anim_frame(void)
{
    slotCnt = (uint16_t)(slotCnt - 1u);
    if (slotCnt & 0x8000u) {
        slotCnt = 5;
        /* acc5..acd2: the +3 step runs only while [0x6267] is set; on the
         * room path the slot stays parked (the board-stack holds).  Either
         * way the 0 terminator wraps to slot 0. */
        unsigned s = slot;
        if (nameScreen) s += 3u;
        if (s >= 61u || slotTab[s] == 0u) s = 0u;   /* the terminator */
        slot = s;
    }
    if (nameScreen)
        scroll = (uint16_t)((slotTab[slot] & 0x0Fu) - 4u);
}

static void name_screen_enter(void)
{
    nameScreen = 1;
    pageDraw = 0x2800;                 /* [0x5AC4] := 0x2800 (AF92)  */
    pageDisp = 0x7D00;
    splitRow = 0x88;                   /* [0x5B0A] := 0x88: the split */
    /* the transition's two strip blits (AFBD..AFDF): the room's LEFT view
     * (code2 rows 0..135) onto the window base, and the humans' stage
     * (code2+0x15400, 56 rows — the strip bank 0x1540 PARAGRAPHS in) onto
     * the fixed band at absolute 0. */
    if (code2 && code2Size >= 0x1E000u) {
        /* AF92's order is load-bearing: the stage strip first (rows 0..55),
         * then the character (band rows 13..39), then the main art (rows
         * 40..175, starting just below him).  Drawing the strip after the
         * character — as an earlier revision did — paints over him. */
        planarise_rows(code2 + 0x15400u, code2Size - 0x15400u, 0, 56u);
        /* afcf: the character draw runs here, with [0x5AC4] held 0 — the
         * 0x820-band branch in draw_character fires and the walked-in
         * human lands on the fixed band's hill (screen rows 149..175). */
        draw_character();
        planarise_rows(code2, code2Size, 0x2800, 136u);
    }
    draw_humans();
    draw_name_line();
    phase = FE_NAME;
}

/* --- the phases ---------------------------------------------------------- */

static void start_exit(void)
{
    /* the event loop's exit tail (AA86..): the fade to the 0x48BC ramp
     * (black) div 0x23 — the original then resets the video mode, which
     * the port's level flow owns. */
    palette_buildFade(0, 256, 0x23, ramp0);
    phase = FE_EXITFADE;
}

static void do_load(void)
{
    uint8_t *textSpr = NULL;
    video_setMode(VIDEO_MODE_X);       /* frontend_room_setup: reset_display_mode
                                        * (1000:82c6) — the 320x192 field   */
    code2   = res_loadChunk("LBM\\CODE2.RAW",   &code2Size,   NULL);
    codehum = res_loadChunk("APL\\CODEHUM.SPR", &codehumSize, NULL);
    codes   = res_loadChunk("LEVELS\\CODES.BIN", &codesSize,  NULL);
    codeb1  = res_loadChunk("APL\\CODEB1.SPR",  &codeb1Size,  NULL);
    codeb2  = res_loadChunk("APL\\CODEB2.SPR",  &codeb2Size,  NULL);
    restore = res_loadChunk("APL\\RESTORE.SPR", &restoreSize, NULL);
    textSpr = res_loadChunk("APL\\TEXT.SPR",    NULL,         NULL);
    font_publish(textSpr);

    /* the room's opening state (frontend_room_setup's seeds) */
    pageDraw = 0; pageDisp = 0x7D00;
    splitRow = 200;                    /* [0x5B0A] := 0xC8 (no split)  */
    cell = 0; walkState = 0xFFFF;
    charX = 0x1D2; charY = 0xA0;
    frame = 0x13; frameCnt = 3;
    slot = 0x12; slotCnt = 5;
    /* optCounter is deliberately NOT reseeded: the image ships the four
     * bytes zero and the setup never writes them (0x5E5B..0x5E79), so the
     * settings survive every re-entry into the room for the whole run. */
    nameScreen = 0; fireLatch = 0;
    name_reset();
    set_scroll();

    if (code2 && code2Size >= 0x1E000u + 768u) {
        /* the palette: the caller computes the source segment as
         * [0x625D] + 0x1E00 PARAGRAPHS = byte offset 0x1E000 — the
         * file's tail, right after the 640x192 image (0x1E000 bytes).
         * palette_load_6bit (1000:8D24) masks each byte & 0x3F. */
        palette_loadWorking(code2 + 0x1E000u);
        /* 200 rows (room_backdrop_build 1000:AF07, CX=0xC8): rows 192..199
         * are the chunk's palette tail, then the pool — copied from
         * `code2Size` here.  reset_display_mode's 320x192 field (1000:82C6,
         * called at 1000:A544) ends on row 191, so the original never shows
         * them either. */
        planarise_rows(code2, code2Size, 0,      200u);
        planarise_rows(code2, code2Size, 0x7D00, 200u);
        /* the setup's 2-frame loop (A5A1..A5DB): the full frame — the
         * badges, the character, the name line — drawn on BOTH pages
         * before the fade, so the fade-in holds a complete static frame
         * while the compose alternates (drawing one page only made the
         * character flicker through the fade).  The original then waits
         * on the fade with no further draws. */
        for (unsigned f = 0; f < 2u; f++) {
            draw_option_sprites();
            draw_character();
            draw_name_line();
            compose(1);
        }
        palette_buildFade(0, 256, 0x23, palette_working());
        phase = FE_FADEIN;
    } else {
        phase = FE_ROOM;   /* the no-data path: no palette, no backdrop */
    }
}

int frontend_tick(const InputState *in)
{
    switch (phase) {
    case FE_IDLE:
        phase = FE_LOAD;
        /* fall through: the load takes this first tick */
    case FE_LOAD:
        do_load();
        return 1;
    case FE_FADEIN:
        compose(1);
        if (!palette_fadeActive()) phase = FE_ROOM;
        return 1;
    case FE_ROOM: {
        /* the frame web's head (1000:A5E8): the MUSIC board's counter
         * [0x5E5B] stops the music when non-zero, resumes it when zero */
        audio_musicGate(frontend_option(FRONTEND_OPT_MUSIC));

        if ((in->joyBits & 0x80u) && (walkState & 0x8000u) && !fireLatch) {
            cycle_option();
            fireLatch = 1;
        }
        if (!(in->joyBits & 0x80u)) fireLatch = 0;

        set_scroll();
        draw_option_sprites();          /* place_4_sprites_to_vga (A684) */
        draw_character();
        draw_name_line();
        anim_frame();                   /* the walk loop's acb9: keeps the
                                         * board-stack's middle pair in vram
                                         * (the event loop's idle) */
        draw_humans();
        room_walk(in);
        compose(1);

        /* the tail's x-dispatch (A890..) */
        if (charX == 0x280u) {
            (void)code_match();          /* the right-edge exit ignores it */
            start_exit();
        } else if (charX == 0x108u) {
            if (scroll != 0u) phase = FE_PAN;   /* the pan-out first */
            else name_screen_enter();
        }
        return 1;
    }
    case FE_PAN:
        /* the event loop's head (AA2C..): the scroll runs down to 0 */
        if (scroll == 0u) { name_screen_enter(); return 1; }
        scroll = (uint16_t)(scroll - 1u);
        draw_character();
        compose(1);
        return 1;
    case FE_NAME:
        anim_frame();
        draw_humans();                  /* the fixed band's two walkers */
        draw_name_line();
        compose(0);                      /* the name screen is single-buffered */
        if (in->lastKey) {
            uint8_t k = in->lastKey;
            /* AA4E/AA50: the event loop reads AND clears [0x48A3] — one
             * consume per key event; a held key re-fires only via the
             * (BIOS/SDL) typematic repeat. Without the clear one backspace
             * tap spans several ticks and erases the whole name. */
            ((InputState *)in)->lastKey = 0u;
            if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') {
                name_addchar(k);
            } else if (k == 0x1Bu) {    /* ESC: clear the name, no exit */
                memset(nameBuf, 0, sizeof nameBuf);
                draw_name_line();
            } else if (k == 0x0Du) {     /* Enter: the match gates the exit */
                if (code_match()) start_exit();
            } else if (k == 0x0Eu) {
                name_erase();
            }
        }
        return 1;
    case FE_EXITFADE:
        /* The fade holds the frame the screen last presented.  This used to
         * pass 1, which swapped pageDraw to 0x7D00 - the ROOM's second page,
         * where the setup's 2-frame loop (1000:A5A1) had drawn the seeded
         * "DARWIN".  The name screen is single-buffered (its cursor+present
         * double-swap cancels, so it composes with 0), so swapping here put
         * the stale room page on screen after the first fade tick: a correct
         * code started the right level, but the old DARWIN showed through the
         * fade-out.  Keep the name screen's page (0x2800). */
        compose(0);
        if (!palette_fadeActive()) return 0;   /* the room has exited */
        return 1;
    }
    return 1;
}
