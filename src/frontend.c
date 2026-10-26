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
 * ([0x5E46] = (struct-idx, count); the structs' byte 0 at 0x5E5B/0x5E65/
 * 0x5E6F/0x5E79): cell 2 cycles the 3-state difficulty [0x5E65] (entry maps
 * 0/1/2 -> 12/10/8), cell 3 the 2-state music gate [0x5E5B].  One cycle per
 * press (the original spins on the held key); the counters persist for the
 * level flow to consume.
 *
 * DEVIATIONS (all port-only, each waiting on its named step):
 *  - the INT 60h calls (music_resume/stop 83F7/8389, the cmd-0xA pump) are
 *    not made: the audio backend is an M5 stub.  The gate state ports.
 *  - the name screen's CRTC split ([0x5B0A]=0x88) has no backend equivalent:
 *    the window presents unsplit.
 *  - the two animated decor sprites (code2.raw+0x1540/+0x17C0,
 *    name_screen_anim_frame's ADB6 draws) and place_4_sprites_to_vga's four
 *    room sprites are not drawn yet; restore/codeb1/codeb2 are not loaded.
 *  - the typed name renders through the font arena at the page row of the
 *    original's name-line offset (0x38F5/160), centred on 0x98 as
 *    draw_text's call, in ink 0x0F: the original's packed-blob copy
 *    (the DS:0x152 staging + 1000:299b) is not reproduced and the exact
 *    ink is unsettled.  The fidelity pass pins both.
 *  - the character's draw is byte-faithful to the stager+blit pair
 *    (the backdrop-restored 27x20 box, the sprite overlay at rows 1..24,
 *    cols 2..17); rows past code2's 192 read as 0 where the original
 *    reads pool bytes.
 *  - the backdrop planarises code2.raw's 192 chunky rows; the original's
 *    200-row strip copy reads 8 rows past the image into the palette
 *    bytes (the port leaves them zero).
 *  - the compose folds the CRTC start + the pel-pan phase into one
 *    pixel-exact window (the original latches both at vsync).
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

/* 202F:0x5E46 — per cell (struct-idx, count); count 0 = no option. */
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

/* 202F:0x61A3 — the anim slot table (60 bytes, 0x00 wraps to slot 0);
 * the name screen's scroll := (slot byte & 0xF) - 4. */
static const uint8_t slotTab[61] = {
    0x88,0x88,0x88,0x77,0x77,0x77,0x66,0x66,0x66,0x55,0x55,0x66,
    0x44,0x55,0x66,0x44,0x55,0x66,0x44,0x55,0x66,0x55,0x55,0x66,
    0x66,0x66,0x66,0x77,0x77,0x77,0x88,0x88,0x88,0x88,0x99,0x99,
    0x99,0xAA,0xAA,0xAA,0xBB,0xBB,0xAA,0xCC,0xBB,0xAA,0xCC,0xBB,
    0xAA,0xCC,0xBB,0xAA,0xBB,0xBB,0xAA,0xAA,0xAA,0xAA,0x99,0x99,
    0x99, /* the 0x00 terminator is slotTab[60] */
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
static char     nameBuf[14];     /* 0x61E0: 13 chars + NUL               */
static unsigned codeIndex;       /* [0x5F09]: the matched code's index   */
static uint8_t  optCounter[4];   /* the structs' byte 0 (0x5E5B..0x5E79)*/
static int      fireLatch;        /* the action key's held-spin           */
static uint16_t slotCnt;         /* [0x619F]: the anim countdown (5)     */
static unsigned slot;            /* [0x61A1]: the anim slot (parked 0x12)*/
static uint8_t  nameScreen;      /* [0x6267]: the name-screen flag       */

static uint8_t *code2, *codehum, *codes;
static uint32_t code2Size, codehumSize, codesSize;
static uint8_t  arena[16000];    /* the font arena (the [0x1E20] analog) */
static const uint8_t ramp0[768]; /* the 0x48BC ramp: zeroed = black     */

int frontend_init(const OSystem *osys) { (void)osys; return 0; }

unsigned frontend_selectedLevel(void) { return codeIndex; }

/* --- the VRAM primitives ---------------------------------------------- */

static void put_pixel(unsigned rowBase, unsigned px, uint8_t v)
{
    vram[(rowBase + (px >> 2)) & 0xFFFFu][px & 3u] = v;
}

/* blit_planar_strip (1000:AEAD): planarise chunky 640-px rows into the
 * page at `base` — dest[base + r*160 + c] plane p = src[r*640 + c*4 + p]. */
static void planarise_rows(const uint8_t *src, unsigned base,
                           unsigned rows)
{
    for (unsigned r = 0; r < rows; r++)
        for (unsigned c = 0; c < 160u; c++) {
            unsigned a = (base + r * 160u + c) & 0xFFFFu;
            const uint8_t *s = src + r * 640u + c * 4u;
            vram[a][0] = s[0]; vram[a][1] = s[1];
            vram[a][2] = s[2]; vram[a][3] = s[3];
        }
}

/* The present: the 200-row window, flat — pixel x of the window = the
 * world pixel (scroll + x), i.e. set_display_ptr's byte start plus the
 * sub-byte pel-pan phase ([0x0] is in pixels; the original latches both
 * at vsync — the port's compose folds them).  The room is double-buffered
 * (present_page 1000:8809 swaps after showing); the name screen's
 * cursor+present double-swap cancels, so it passes 0. */
static void compose(int swapPages)
{
    uint8_t *page = video_page(0);
    if (page) {
        for (unsigned y = 0; y < 200u; y++) {
            unsigned rowBase = pageDraw + y * 160u;
            uint8_t *d = page + y * 320u;
            for (unsigned x = 0; x < 320u; x++) {
                unsigned w = scroll + x;   /* the world pixel */
                d[x] = vram[(rowBase + (w >> 2)) & 0xFFFFu][w & 3u];
            }
        }
    }
    if (swapPages) {
        unsigned t = pageDraw; pageDraw = pageDisp; pageDisp = t;
    }
}

/* --- the name ----------------------------------------------------------- */

static void draw_name_line(void)
{
    /* font_drawText draws nothing when no TEXT.SPR was published — the
     * headless no-data case. */
    memset(arena, 0, 80u);         /* just the row the text lands in */
    font_drawText(arena, nameBuf, 0x98u, 0);
    unsigned prow = ((pageDraw + 0x38F5u) / 160u) & 0xFFu;
    unsigned base = pageDraw + prow * 160u;
    for (unsigned b = 0; b < 80u; b++) {
        uint8_t bits = arena[b];
        for (unsigned k = 0; k < 4u; k++)
            if (bits & (1u << k))
                put_pixel(base, b * 4u + k, 0x0Fu);
    }
}

/* name_entry_init (1000:AAC9): the "DARWIN" seed. */
static void name_reset(void)
{
    strcpy(nameBuf, "DARWIN");
    draw_name_line();
}

/* name_entry_addchar (1000:AB1E): append at the NUL, 13-char bound. */
static void name_addchar(uint8_t c)
{
    unsigned i = 0;
    while (i < 13u && nameBuf[i]) i++;
    if (i < 13u) { nameBuf[i] = (char)c; draw_name_line(); }
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
        unsigned base = pageDraw + srcRow * 160u;
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
 * (a 0 table byte wraps to slot 0); the pan := (byte & 0xF) - 4.  The two
 * decor sprite draws are the documented deviation. */
static void anim_frame(void)
{
    slotCnt = (uint16_t)(slotCnt - 1u);
    if (slotCnt & 0x8000u) {
        slotCnt = 5;
        unsigned s = slot + 3u;
        if (slotTab[s] == 0u) s = 0;
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
    code2   = res_loadChunk("LBM\\CODE2.RAW",   &code2Size,   NULL);
    codehum = res_loadChunk("APL\\CODEHUM.SPR", &codehumSize, NULL);
    codes   = res_loadChunk("LEVELS\\CODES.BIN", &codesSize,  NULL);
    textSpr = res_loadChunk("APL\\TEXT.SPR",    NULL,         NULL);
    font_publish(textSpr);

    /* the room's opening state (frontend_room_setup's seeds) */
    pageDraw = 0; pageDisp = 0x7D00;
    cell = 0; walkState = 0xFFFF;
    charX = 0x1D2; charY = 0xA0;
    frame = 0x13; frameCnt = 3;
    slot = 0x12; slotCnt = 5;
    memset(optCounter, 0, sizeof optCounter);
    nameScreen = 0; fireLatch = 0;
    name_reset();
    set_scroll();

    if (code2 && code2Size >= 0x1E000u + 768u) {
        /* the palette: the caller computes the source segment as
         * [0x625D] + 0x1E00 PARAGRAPHS = byte offset 0x1E000 — the
         * file's tail, right after the 640x192 image (0x1E000 bytes).
         * palette_load_6bit (1000:8D24) masks each byte & 0x3F. */
        palette_loadWorking(code2 + 0x1E000u);
        planarise_rows(code2, 0,      192u);
        planarise_rows(code2, 0x7D00, 192u);
        draw_character();
        draw_name_line();
        compose(1);
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
        if ((in->joyBits & 0x80u) && (walkState & 0x8000u) && !fireLatch) {
            cycle_option();
            fireLatch = 1;
        }
        if (!(in->joyBits & 0x80u)) fireLatch = 0;

        set_scroll();
        draw_character();
        draw_name_line();
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
        draw_name_line();
        compose(0);                      /* the name screen is single-buffered */
        if (in->lastKey) {
            uint8_t k = in->lastKey;
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
        compose(1);
        if (!palette_fadeActive()) return 0;   /* the room has exited */
        return 1;
    }
    return 1;
}
