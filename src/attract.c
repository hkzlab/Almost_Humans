/* attract.c — the ATTRACT loop: game_loop (1000:131E) + the four intro
 * effects, ported from re_map.md § "The ATTRACT loop decoded end to end
 * (2026-10-25)"; the data formats and the shipped tables are in
 * re_formats.md §9.4.
 *
 * The machine model: the original's 64-KB mode-X address space (the CRTC
 * byte mode bios_set_mode_13h installed — one address byte = 4 pixels, one
 * per plane; 80 bytes per scanline; the start address a byte offset) becomes
 * vram[addr][plane]; the CRTC start becomes displayStart; every present
 * composes the 200-row window at displayStart into the flat page 0
 * (game_main's per-tick videoPresent shows it — the same contract
 * scene_playBootIntro uses).
 *
 * attract_tick() advances the machine by exactly ONE display period (the
 * original's wait_vsync cadence).  The exit (attract_exit 1000:13E9: the
 * configured action key [0x5ACE bit 7] or any held key [0x48A3]) is polled
 * EXACTLY where the original polls it — at the fade waits (fade_wait
 * 1000:13E0) and the holds (fade_step_frames 1000:13FB), once per tick of
 * those phases; the no-poll windows (the two wipes, the frame draws, the
 * slide's tail) see no key until their next poll, as in the listing.  The
 * local polls (effect 3's slide skip, effect 4's scroller skip) stay local
 * exactly as in the listing — they skip the current hold, they do NOT exit
 * the attract.
 *
 * The one port-only escape point is the ASSET-LOAD-FAILURE skip: the
 * original has no such path (a failed INT 63h prompts for the disc), and
 * the port's headless tests run without data — so the skip consumes the
 * escape to stay drivable.  With data present that path never runs and the
 * polling is the original's.  (Load failures otherwise skip the effect —
 * the scene player's established pattern.)
 *
 * The escape itself (the longjmp 1000:13B2): the saved page pair is
 * restored, [0x1E7C]:=0x0A, one fade of all 256 DAC entries to the zeroed
 * 0x48BC ramp (black) div 0x23, waited WITHOUT further polling
 * (wait_palette_fade_done 1000:8B8B); its completion is the hand-off to
 * the front-end (FLOW_MENU — the original's RET into entry LAB_002B).
 *
 * Effect 4's credits scroller is the real machine (decoded 2026-10-25,
 * re_map.md "Effect 4" items 3-6): the fill builds the 192-KB strip at
 * [0xD76] (clear_bitmaps_6) as a vertical bitmap of 80-byte rows — [0]
 * the signature, [1] the write-mode-2 mask, [20..79] the presented 60
 * bytes — one 15-row tile per text line (BP += 75 paras) or 16-row blank
 * tile (BP += 80 paras), [0xD39] counting rows; blit_scroll (1000:1B58)
 * then presents ONE strip row per tick into the draw page's scanline
 * bytes 20..79 (screen x 80..319) in write mode 2 with the row's own bit
 * mask (0x20: the ink sets palette bit 5 over the background; 0xE0: the
 * background clamped to index & 0x1F), skipping rows whose signature
 * matches the 200-byte cache at CS:0x1CF7 (shipped 0x80), until
 * [0xD78] == [0xD39] - 0xC8; more credtext re-fills the next screenful,
 * else the tail ([0x1E7C]:=0x0A, fade to black div 0x1E).
 */
#include "game.h"
#include <stdlib.h>
#include <string.h>

/* --- the planar address space (A000, unchained, byte mode) ------------- */

static uint8_t vram[65536][4];   /* [address][plane]: 1 byte = 4 pixels   */
static unsigned pageDraw, pageDisp;   /* [0x5AC4]/[0x5AC6]                */
static unsigned displayStart;          /* [0x4106]: the CRTC start (bytes) */
static unsigned savedDraw, savedDisp;  /* game_loop's PUSH pair (1387)     */

/* --- the shipped DS tables (re_formats.md §9.4, extracted 2026-10-25) -- */

/* 202F:0x4DE — 10 x 18 B: u32 score, name at +4.  These are the shipped
 * defaults the attract shows on a fresh boot; HISCORES.HUM (re_formats.md
 * §11) rewrites them once a game has been played — that loader belongs to
 * the hi-score step, not to the attract. */
static const struct { uint32_t score; char name[15]; } hiscore[10] = {
    {100000u, "STEVE JONES"},   {90000u, "NIKKI"},
    {80000u,  "NATASHA"},       {70000u, "MARV"},
    {60000u,  "JAMIE IRVINE"},  {50000u, "STEVE OLDACRE"},
    {40000u,  "KEV MACINTOSH"}, {30000u, "IAN HOWIE"},
    {20000u,  "SIMON HIRST"},   {10000u, "CRAIG MASSEY"}
};

/* the shipped default table, exposed to the MAP's high-score list */
unsigned attract_hiscoreCount(void) { return 10u; }
uint32_t attract_hiscoreScore(unsigned i)
{
    return i < 10u ? hiscore[i].score : 0u;
}
const char *attract_hiscoreName(unsigned i)
{
    return i < 10u ? hiscore[i].name : "";
}

/* 202F:0x38C — 160 per-level records of [seconds][minutes] (two
 * pseudo-decimal nibbles / one digit), shipped as 99 09 ("9:99") for every
 * level. */
static const uint8_t fastest_times[160][2] = {
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
    {0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},{0x99,0x09},
};

/* the strings at 202F:0xC38/0xC4D/0xC73/0xC79 (re_formats.md §9.4) */
static const char STR_TIMES_HEADER[]  = "FASTEST LEVEL TIMES!";
static const char STR_TIMES_LABEL[]   = "LEVEL         MINUTES,    SECONDS";
static const char STR_SCORE_HEADER[]  = "TOP TEN TRIBES";

/* the 0x48BC ramp analog: it enters the attract zeroed (the boot's
 * clear_fade_ramp) and nothing in the attract ever writes it, so every
 * "fade to 0x48BC" is a fade to black. */
static const uint8_t ramp0[768];

/* --- the module state --------------------------------------------------- */

static AttractPhase phase = ATTRACT_IDLE;
static unsigned counter;          /* the multi-tick phase countdown        */
static unsigned frame;            /* effect 2's frame: 0 = scores, 1..16   */
static unsigned wipeIter;         /* effect 2's 79-step wipe iteration     */
static unsigned d3b;              /* [0xD3B]: the slide's display row      */
static uint8_t arena[16000];      /* [0xC9D]: the text arena (200 x 80)    */
static uint8_t pageB[64000];      /* [0xC9B]: the composed display page    */
static uint8_t *imgBuf;           /* [0xC32]: the loaded image             */
static uint8_t *textSpr;          /* [0x1E1C] analog: the loaded font      */
static uint8_t colour1881 = 0x43; /* CS:0x1881: blit_80x200's colour base  */

/* --- effect 4's credits strip (re_map.md "Effect 4") ----------------------
 * The 192-KB work area [0xD76] (clear_bitmaps_6 1000:1B3B, re-zeroed per
 * screenful): a vertical bitmap of 80-byte rows — [0] the row signature,
 * [1] the write-mode-2 bit mask, [2..19] pad, [20..79] the presented 60
 * bytes = arena pixels 80..319. */
static uint8_t e4strip[196608];
static uint8_t rowSig[200];       /* CS:0x1CF7: the row-signature cache    */
static int rowSigInit;            /* the shipped 0x80 fill runs once       */
static uint8_t *credtext;         /* [0xC9F]: the loaded credtext.bin      */
static uint32_t credtextSize;
static unsigned credCursor;       /* [0xD6C]: the credtext cursor          */
static uint16_t rowsBuilt;        /* [0xD39]: the strip rows built         */
static uint16_t scrollRow;        /* [0xD78]: the scroll window's start row */

/* [0x1E7C] and [0xD3D] are not carried: in the decoded image they are
 * write-only (the cycle/effect tag and the skip flag — the writers are in
 * re_map.md § "The ATTRACT loop decoded"; no reader is traced), so the
 * port records their semantics in comments instead of dead state. */

int attract_init(const OSystem *osys) { (void)osys; return 0; }

unsigned attract_phaseForProbe(void) { return (unsigned)phase; }

/* the strip probe: the 192-KB work area and [0xD39] (the rows built) —
 * the byte-exact fixture test compares these against the sim's dump. */
const uint8_t *attract_stripForProbe(unsigned *rowsOut)
{
    if (rowsOut) *rowsOut = rowsBuilt;
    return e4strip;
}

/* --- the VRAM primitives ------------------------------------------------ */

/* clear_vram (1000:833A): map mask 0xF, 64 KB zeroed. */
static void clear_vram(void) { memset(vram, 0, sizeof vram); }

/* blit_planar_to_vga (1000:888E): one 320x200 chunky image planarised into
 * the 16000-byte block at `dest` — dest[base + r*80 + c] plane p
 * = src[r*320 + c*4 + p]. */
static void blit_planar(const uint8_t *src, unsigned dest)
{
    for (unsigned r = 0; r < 200u; r++) {
        unsigned a = dest + r * 80u;
        if (a + 79u >= 65536u) break;         /* never taken: dest is safe */
        const uint8_t *s = src + r * 320u;
        for (unsigned c = 0; c < 80u; c++) {
            vram[a + c][0] = s[c * 4u];
            vram[a + c][1] = s[c * 4u + 1u];
            vram[a + c][2] = s[c * 4u + 2u];
            vram[a + c][3] = s[c * 4u + 3u];
        }
    }
}

/* begin_page_flip (1000:8821) + the swap (1000:87C7): the display target is
 * the drawing page, then the pair swaps.  The original's spin until the ISR
 * applies [0x4106] is implicit: the port composes from displayStart at the
 * tick's present. */
static void flip_page(void)
{
    unsigned t = pageDraw;
    displayStart = pageDraw;
    pageDraw = pageDisp;
    pageDisp = t;
}

/* wait_page (1000:195B): displayStart := [0xD3B]*80, no page-pair change. */
static void wait_page(unsigned row) { displayStart = row * 80u; }

/* The present: the 200-row window at displayStart, flat. */
static void compose_window(void)
{
    uint8_t *page = video_page(0);
    if (!page) return;
    for (unsigned y = 0; y < 200u; y++) {
        const unsigned row = (displayStart + y * 80u) & 0xFFFFu;
        uint8_t *d = page + y * 320u;
        for (unsigned x = 0; x < 320u; x++)
            d[x] = vram[(row + (x >> 2)) & 0xFFFFu][x & 3u];
    }
}

/* --- effect 1's wipe: intro_wipe_step (1000:14C3) -----------------------
 * The damped oscillator on [0xC35]=pos, [0xC36]=dir (0=shrink), [0xC37]=speed:
 * trace 199->0 (speed 32), grow 16 -> peak 136, shrink -> 0, grow 8 -> 36,
 * -> 0, 4 -> 10, 2 -> 3, 1 -> 1, 0 -> exit (pos==0 && dir==0). */
static unsigned wipePos, wipeSpeed;
static int wipeDir;

static void wipe_step(void)
{
    unsigned pos = wipePos, speed = wipeSpeed;
    if (wipeDir) {                              /* grow: 14e0 */
        pos += speed;
        if (pos > 0xC7u) pos = 0xC7u;
        if (speed == 0u) { wipeDir = 0; wipeSpeed = 0u; wipePos = pos; return; }
        speed--;
    } else {                                    /* shrink: 14f6 */
        unsigned r = pos - speed;               /* SUB AX,CX flags */
        if (r > 0xFFu || r == 0u) {             /* negative or exact zero */
            wipePos = 0u; wipeDir = 1;
            wipeSpeed = speed >> 1;
            return;
        }
        pos = r;
        if (speed < 0x20u) speed++;
    }
    wipePos = pos; wipeSpeed = speed;
}

/* --- the text helpers --------------------------------------------------- */

/* format_decimal (1000:0AEC): value DX:CX; >= 10000 prints the quotient
 * then exactly 4 zero-padded remainder digits, else the plain digits. */
static void format_decimal(uint32_t v, char *out)
{
    unsigned n = 0;
    char t[8];
    unsigned m = 0;
    if (v >= 10000u) {
        unsigned hi = v / 10000u, lo = v % 10000u;
        if (!hi) t[m++] = '0';
        while (hi) { t[m++] = (char)('0' + hi % 10u); hi /= 10u; }
        while (m) out[n++] = t[--m];
        for (unsigned i = 0; i < 4u; i++) { out[n++] = (char)('0' + lo % 10u); lo /= 10u; }
    } else {
        if (!v) t[m++] = '0';
        while (v) { t[m++] = (char)('0' + v % 10u); v /= 10u; }
        while (m) out[n++] = t[--m];
    }
    out[n] = 0;
}

/* draw_score_block (1000:0651): [0x36]=0x10, [0x38]=0x18 (set by
 * intro_frame_1); rank x=0x20, name x=0x82, score x=0x102. */
static void draw_score_block(uint8_t *ar)
{
    unsigned row = 0x18u;
    for (unsigned k = 0; k < 10u; k++) {
        char buf[12];
        format_decimal(k + 1u, buf);
        font_drawText(ar, buf, 0x20u, row);
        font_drawText(ar, hiscore[k].name, 0x82u, row);
        format_decimal(hiscore[k].score, buf);
        font_drawText(ar, buf, 0x102u, row);
        row += 0x10u;
    }
}

/* draw_number_hex's string builders (1000:1693..16D0): the 3-digit level
 * number, the minutes digit behind 0xC73's '0', the seconds nibbles. */
static void draw_times_row(uint8_t *ar, unsigned levelMinus1,
                           const uint8_t *rec, unsigned row)
{
    unsigned v = levelMinus1 + 1u;
    char num[5], mins[3], secs[3];
    num[0] = (char)('0' + v / 100u);
    num[1] = (char)('0' + (v / 10u) % 10u);
    num[2] = (char)('0' + v % 10u);
    num[3] = 0;
    mins[0] = '0';
    mins[1] = (char)('0' + rec[1]);
    mins[2] = 0;
    secs[0] = (char)('0' + (rec[0] >> 4));
    secs[1] = (char)('0' + (rec[0] & 0xFu));
    secs[2] = 0;
    font_drawText(ar, STR_TIMES_LABEL, 0x92u, row);   /* 16D3, redrawn per row */
    font_drawText(ar, num, 0x3Du, row);
    font_drawText(ar, mins, 0x74u, row);
    font_drawText(ar, secs, 0xD8u, row);
}

/* --- effect 2's present path -------------------------------------------- */

/* blit_80x200 (1000:17FB): the arena's bits 0..3 (4 px per byte, bit 0 the
 * leftmost) splat colour over pageB, 320 B per row from dest offset 4. */
static void blit_80x200(const uint8_t *ar)
{
    unsigned dl = 0xFDu;
    for (unsigned row = 0; row < 200u; row++) {
        uint8_t *d = pageB + 4u + row * 320u;
        const uint8_t *s = ar + row * 80u;
        for (unsigned k = 0; k < 80u; k++) {
            uint8_t b = s[k];
            for (unsigned bit = 0; bit < 4u; bit++) {
                if (b & 1u) d[k * 4u + bit] = (uint8_t)dl;
                b >>= 1;
            }
        }
        unsigned cx = 200u - row;               /* the pushed row countdown */
        if (cx > 0xB0u)      dl = 0xF0u | ((dl + 1u) & 0xFu);
        else if (cx == 0xB0u) dl = colour1881;
        else { dl = (dl + 1u) & 0xFFu; if (!dl) dl = 0x40u; }
    }
    /* the tail: [CS:0x1881] += 0xA0, +0x40 while < 0x40 (1862) */
    colour1881 = (uint8_t)((colour1881 + 0xA0u) & 0xFFu);
    if (colour1881 < 0x40u) colour1881 = (uint8_t)(colour1881 + 0x40u);
}

/* blit_sprite_planes (1000:1795): an 8x16 chunk of pageB planarised into
 * vram at pageDraw + row*80 + x/4, plane (x+j)&3.  The source/dest read
 * past a row's edge into the next row exactly as the original's flat
 * offsets do (the right-sweeping columns start at 319). */
static void blit_strip8(unsigned x, unsigned row)
{
    for (unsigned j = 0; j < 8u; j++) {
        unsigned px = x + j;
        for (unsigned r = 0; r < 16u; r++) {
            unsigned sy = row + r;
            uint8_t v = pageB[sy * 320u + px];
            vram[(pageDraw + sy * 80u + (px >> 2)) & 0xFFFFu][px & 3u] = v;
        }
    }
}

/* wipe_row (1000:1722): eleven 16-row chunks tiling rows 8..183,
 * alternating between the right-sweeping column (the arg_swap variant,
 * x = 319-4k) and the left-sweeping one (x = 4k) — the criss-cross. */
static void wipe_row(unsigned k)
{
    static const unsigned rows[11] = {
        0x8u, 0x18u, 0x28u, 0x38u, 0x48u, 0x58u, 0x68u, 0x78u, 0x88u, 0x98u, 0xA8u
    };
    unsigned xs = 4u * k;
    unsigned xd = 0x13Fu - 4u * k;
    for (unsigned i = 0; i < 11u; i++)
        blit_strip8((i & 1u) ? xs : xd, rows[i]);
}

/* --- the per-effect loads ------------------------------------------------ */

static void unload_img(void) { free(imgBuf); imgBuf = NULL; }

/* effect 1 (1000:1406): the load + the wipe setup.  Returns 0 = skip. */
static int e1_load(void)
{
    clear_vram();
    uint32_t sz = 0;
    unload_img();
    imgBuf = res_loadChunk("LBM\\GAMEINT.BIT", &sz, NULL);
    if (!imgBuf || sz < 64000u + 768u) { unload_img(); return 0; }

    palette_loadWorking(imgBuf + 64000u);
    blit_planar(imgBuf, pageDraw);  flip_page();   /* the image at 0 and   */
    blit_planar(imgBuf, pageDraw);  flip_page();   /* at 0x8000           */

    pageDraw = 0x3E80u;                            /* 142F/1435: the blank */
    pageDisp = 0xBE80u;                            /* start, past the copy */
    flip_page();

    wipePos = 0xC7u; wipeDir = 0; wipeSpeed = 0x20u;   /* 143e: 199/shrink/32 */
    palette_buildFade(0u, 256u, 0x46u, palette_working());
    return 1;
}

/* effect 3 (1000:18CA + intro_setup 1000:1903).  Returns 0 = skip. */
static int e3_load(void)
{
    clear_vram();
    uint32_t sz = 0;
    unload_img();
    imgBuf = res_loadChunk("LBM\\PCTITLE.RAW", &sz, NULL);
    if (!imgBuf || sz < 112768u) { unload_img(); return 0; }

    blit_planar(imgBuf, 0x3E80u);                  /* the title: rows 200-400 */
    blit_planar(imgBuf + 48000u, 0x6D60u);         /* the promo: rows 350-550 */
    palette_loadWorking(imgBuf + 112000u);
    wait_page(d3b);                                /* the stale [0xD3B]       */
    palette_buildFade(0u, 256u, 0x23u, palette_working());
    return 1;
}

/* effect 2 (1000:1514): the greyscale gameint + scorepal.  Returns 0 = skip. */
static int e2_load(void)
{
    pageDraw = 0u; pageDisp = 0x8000u;
    uint32_t sz = 0;
    unload_img();
    imgBuf = res_loadChunk("LBM\\GAMEINT.BIT", &sz, NULL);
    if (!imgBuf || sz < 64000u + 768u) { unload_img(); return 0; }

    palette_loadWorking(imgBuf + 64000u);

    /* blit_fullscreen 1000:15B1: two 75-pixel, 200-row strips */
    for (unsigned r = 0; r < 200u; r++) {
        memcpy(imgBuf + r * 320u,        imgBuf + 0x9Eu + r * 320u, 75u);
        memcpy(imgBuf + 0xF5u + r * 320u, imgBuf + 0x55u + r * 320u, 75u);
    }
    /* decode_page_in_place 1000:15E6 + hash_three_bytes 1000:43D5: the
     * luma conversion through gameint's own palette */
    {
        const uint8_t *pal = palette_working();
        for (unsigned i = 0; i < 64000u; i++) {
            const uint8_t *p = pal + imgBuf[i] * 3u;
            imgBuf[i] = (uint8_t)((77u * p[0] + 151u * p[1] + 28u * p[2]) >> 8);
        }
    }
    blit_planar(imgBuf, pageDraw);  flip_page();
    blit_planar(imgBuf, pageDraw);  flip_page();

    uint8_t *palBuf = res_loadChunk("LBM\\SCOREPAL.PAL", &sz, NULL);
    if (palBuf) {
        palette_loadWorking(palBuf);
        free(palBuf);
    }
    palette_buildFade(0u, 256u, 0x1Eu, palette_working());
    return 1;
}

/* fill_pattern (1000:1AFC) — draw_char_advance's blank path: the tile's 16
 * rows of [sig 0xFF, mask 0x20]; the data bytes keep the zeroed strip. */
static void fill_pattern(uint8_t *tile)
{
    for (unsigned r = 0; r < 16u; r++) {
        tile[r * 80u] = 0xFFu;
        tile[r * 80u + 1u] = 0x20u;
    }
}

/* fill_pattern_clip (1000:1B13) — the text path: rows 0..3 the [0xFF, 0xE0]
 * band above the text (the presenter clamps the background to index & 0x1F
 * there, erasing stale ink), rows 4..14 the [0..10, 0x20] per-row
 * signatures (adjacent glyph rows always mismatch the cache, so ink rows
 * are always redrawn).  The original's dead MOV BX,0xD2E (1b1a — BX pushed
 * and never read) is dropped. */
static void fill_pattern_clip(uint8_t *tile)
{
    for (unsigned r = 0; r < 15u; r++) {
        tile[r * 80u]     = (r < 4u) ? 0xFFu : (uint8_t)(r - 4u);
        tile[r * 80u + 1u] = (r < 4u) ? 0xE0u : 0x20u;
    }
}

/* the screenful of 1A07: clear_bitmaps_6 (the 192-KB zero), [0xD39]:=0,
 * the fill loop — strcpy_cae (1AA4) + draw_char_advance (1AB9) per line
 * until the next credtext byte is 0xFF or rowsBuilt >= 0x989 (the max
 * build 2456 rows * 80 B fits the strip exactly) — and the INC SI at
 * 1a37 that walks the cursor onto the terminator (the shipped file's
 * four-byte 0xFF run absorbs it). */
static void e4_fill(void)
{
    char scratch[1024];                 /* 0xCAE; bounded only in the port */
    memset(e4strip, 0, sizeof e4strip);
    rowsBuilt = 0;
    do {
        unsigned n = 0;                 /* strcpy_cae: line -> scratch    */
        while (credCursor < credtextSize) {
            uint8_t c = credtext[credCursor++];
            if (n + 1u < sizeof scratch) scratch[n++] = (char)c;
            if (c == 0u) break;          /* the NUL is copied too          */
        }
        scratch[n] = 0;

        unsigned tile = rowsBuilt;      /* BP: the tile's base row          */
        if (scratch[0] == 0) {           /* 1ac1: the empty-line test       */
            rowsBuilt = (uint16_t)(rowsBuilt + 0x10u);
            fill_pattern(e4strip + tile * 80u);
        } else {                         /* 1adf: the text path             */
            rowsBuilt = (uint16_t)(rowsBuilt + 0xFu);
            fill_pattern_clip(e4strip + tile * 80u);
            /* 1ae9..1af4: [0x1E20] := BP, draw_text(scratch, 0xC8, row 0)
             * — the tile is the arena at row `tile`, so blit_glyph's 16th
             * row lands on the NEXT tile's row 0 exactly as the spill does */
            font_drawText(e4strip, scratch, 0xC8u, tile);
        }
        if (credCursor < credtextSize &&
            credtext[credCursor] == 0xFFu) break;      /* 1a28            */
    } while (rowsBuilt < 0x989u);                       /* 1a2f            */
    credCursor++;                       /* 1a37: INC SI                    */
}

/* blit_scroll (1000:1B58): present strip row `row` — the 200-row window
 * written into the draw page's scanline bytes 20..79 (screen x 80..319),
 * VGA write mode 2 with the row's own bit mask: each 1-bpp source byte
 * sets 4 pixels, plane j := (latch & ~M) | (bit j of the byte ? M : 0).
 * A row whose signature matches the cache at CS:0x1CF7 is skipped whole.
 * The source modulo is the port's one bound: the original's source
 * segment walks past the 192-KB strip into other heap memory only for
 * malformed credtext (a scroll target below 200 rows), unreachable with
 * the shipped data (rowsBuilt = 1579).  The original's trailing [0x5AC8]
 * spin (one ISR tick) and JMP flip_page_swap are this port's tick
 * boundary and the caller's flip_page(). */
static void blit_scroll(unsigned row)
{
    unsigned src = (row * 80u) % sizeof e4strip;
    unsigned di = pageDraw + 0x14u;
    for (unsigned i = 0; i < 200u; i++) {
        uint8_t sig = e4strip[src];
        if (sig != rowSig[i]) {
            rowSig[i] = sig;
            uint8_t mask = e4strip[src + 1u];
            for (unsigned k = 0; k < 60u; k++) {
                uint8_t v = e4strip[(src + 0x14u + k) % sizeof e4strip];
                uint8_t *px = vram[di + k];
                for (unsigned j = 0; j < 4u; j++)
                    px[j] = (uint8_t)((px[j] & (uint8_t)~mask) |
                             (((v >> j) & 1u) ? mask : 0u));
            }
        }
        src = (src + 80u) % sizeof e4strip;
        di += 80u;
    }
}

/* effect 4 (1000:19AF): the loads, the fades, and the first screenful's
 * fill (the 1a01..1a38 stretch runs inside the load instant, with the
 * fade-in animating on the tick, exactly as the original's straight-line
 * code).  Returns 0 = skip. */
static int e4_load(void)
{
    /* 19b9/19be: [0xD3D]:=0, [0x1E7C]:=8 — both write-only (no reader is
     * traced; see the note by the module state) */
    free(credtext);  credtext = NULL;
    credtext = res_loadChunk("CREDTEXT.BIN", &credtextSize, NULL);
    if (!credtext) return 0;
    uint32_t sz = 0;
    unload_img();
    imgBuf = res_loadChunk("LBM\\INTCRED4.BIT", &sz, NULL);
    if (!imgBuf || sz < 64000u + 768u) { unload_img(); return 0; }

    palette_loadWorking(imgBuf + 64000u);
    blit_planar(imgBuf, pageDraw);  flip_page();
    blit_planar(imgBuf, pageDraw);  flip_page();
    palette_buildFade(0u, 256u, 0x1Eu, palette_working());

    /* 1a01/1a07: the cursor at 0, then the first screenful.  The signature
     * cache mirrors the shipped CS:0x1CF7 sentinel (0x80 x200, a value no
     * real signature matches) and — like the original's CS bytes — is
     * never reset after the first fill: it persists across cycles. */
    credCursor = 0;
    if (!rowSigInit) { memset(rowSig, 0x80, sizeof rowSig); rowSigInit = 1; }
    e4_fill();
    return 1;
}

/* --- the machine --------------------------------------------------------- */

static void next_effect(void);   /* below: the 1->3->2->4 cycle            */

/* game_loop's prologue + LAB_000E's disc reset: run at every (re)entry. */
static void attract_restart(void)
{
    res_setDisc(0u);                     /* LAB_000E: [0xF7E] := 0         */
    free(textSpr);
    textSpr = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(textSpr);                /* [0x1E1C] — may be NULL (headless) */

    savedDraw = pageDraw;  savedDisp = pageDisp;   /* 1387: the PUSH pair */
    pageDraw = 0u; pageDisp = 0x8000u;             /* 138F/1395           */
    phase = ATTRACT_E1_LOAD;
}

static void next_effect(void)
{
    /* the cycle boundary (13AB/13BE in the original) also sets the
     * write-only [0x1E7C]:=0x0A — see the note by the module state */
    switch (phase) {
    case ATTRACT_E1_OUT: phase = ATTRACT_E3_LOAD; break;
    case ATTRACT_E3_OUT: phase = ATTRACT_E2_LOAD; break;
    case ATTRACT_E2_OUT: phase = ATTRACT_E4_LOAD; break;
    case ATTRACT_E4_OUT:
        phase = ATTRACT_E1_LOAD;
        break;
    default: phase = ATTRACT_E1_LOAD; break;
    }
    /* the next effect's LOAD runs on the following tick */
}

/* the local poll (the skip tests inside the slide 198C..1998 and effect
 * 4's scroll 1A58..1A67): the same bits as the escape, a different
 * consequence — it skips the current hold, it does NOT exit the attract. */
static int local_key(const InputState *in)
{
    return in->lastKey != 0u || (in->joyBits & 0x80u) != 0u;
}

/* attract_exit (1000:13E9): the escape test — the configured action key
 * ([0x5ACE] bit 7) or any held key ([0x48A3]).  Polled ONLY at the
 * original's poll points: the fade waits (fade_wait 1000:13E0) and the
 * holds (fade_step_frames 1000:13FB), once per tick of those phases. */
static int escape_test(const InputState *in)
{
    return (in->joyBits & 0x80u) != 0u || in->lastKey != 0u;
}

/* the longjmp 1000:13B2: restore the saved page pair, the one fade of all
 * 256 entries to the zeroed 0x48BC ramp (black) div 0x23 — the running
 * fade/state is abandoned exactly as the longjmp does — then wait without
 * further polling (wait_palette_fade_done 1000:8B8B). */
static void latch_escape(void)
{
    pageDraw = savedDraw;  pageDisp = savedDisp;   /* 13b6/13ba */
    palette_buildFade(0u, 256u, 0x23u, ramp0);    /* 13c3..13cf */
    phase = ATTRACT_ESCAPE;
}

int attract_tick(const InputState *in)
{
    if (phase == ATTRACT_DONE || phase == ATTRACT_IDLE)
        attract_restart();               /* a new epoch: game_loop's prologue */

    switch (phase) {
    /* --- effect 1: the gameint wipe (1000:1406) ------------------------ */
    case ATTRACT_E1_LOAD:
        if (!e1_load()) {
            /* the port-only skip path (see the header): consume the escape
             * so the headless flow tests stay drivable */
            if (escape_test(in)) { latch_escape(); break; }
            phase = ATTRACT_E3_LOAD;
        } else {
            phase = ATTRACT_E1_FADEWAIT;
        }
        break;
    case ATTRACT_E1_FADEWAIT:                    /* fade_wait 145f */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        phase = ATTRACT_E1_WIPE;
        break;
    case ATTRACT_E1_WIPE: {                      /* the loop at 1462 */
        wipe_step();
        unsigned bx = wipePos * 80u;
        if (pageDraw < 0x8000u) { pageDraw = bx; pageDisp = 0x8000u + bx; }
        else                   { pageDraw = 0x8000u + bx; pageDisp = bx; }
        flip_page();
        if (wipePos != 0u || wipeDir != 0) break; /* 149c/14a4 */
        counter = 700u;                           /* 14aa: fade_step_frames */
        phase = ATTRACT_E1_HOLD;
        break;
    }
    case ATTRACT_E1_HOLD:                        /* fade_step_frames 14aa */
        if (escape_test(in)) { latch_escape(); break; }
        if (--counter != 0u) break;
        palette_buildFade(0u, 256u, 0x1Eu, ramp0);   /* 14b0: fade to black */
        phase = ATTRACT_E1_OUT;
        break;
    case ATTRACT_E1_OUT:                         /* fade_wait 14bf */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        next_effect();
        break;

    /* --- effect 3: the PCTITLE scroller (1000:18CA) --------------------- */
    case ATTRACT_E3_LOAD:
        if (!e3_load()) {
            if (escape_test(in)) { latch_escape(); break; }
            phase = ATTRACT_E2_LOAD;
        } else {
            phase = ATTRACT_E3_FADEWAIT;
        }
        break;
    case ATTRACT_E3_FADEWAIT:                    /* fade_wait 18e5 */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        d3b = 600u;                               /* 1980 */
        phase = ATTRACT_E3_SLIDE;
        break;
    case ATTRACT_E3_SLIDE:                       /* pctitle_slide_hold 1986 */
        wait_page(d3b);
        if (local_key(in)) {                      /* 198c: the skip poll   */
            /* 19a9: the original sets [0xD3D]:=0xFF (the write-only
             * skip flag) and returns — the hold follows, the slide is cut  */
            counter = 70u;
            phase = ATTRACT_E3_HOLD;
            break;
        }
        if (--d3b != 0u) break;
        phase = ATTRACT_E3_TAIL;                  /* 19a1: the normal exit */
        break;
    case ATTRACT_E3_TAIL:
        wait_page(d3b);                           /* 19a1: d3b == 0       */
        counter = 70u;                            /* 18eb: the 70-tick hold */
        phase = ATTRACT_E3_HOLD;
        break;
    case ATTRACT_E3_HOLD:                         /* fade_step_frames 18eb */
        if (escape_test(in)) { latch_escape(); break; }
        if (--counter != 0u) break;
        palette_buildFade(0u, 256u, 0x23u, ramp0);   /* 18f1: fade to black */
        phase = ATTRACT_E3_OUT;
        break;
    case ATTRACT_E3_OUT:                         /* JMP fade_wait 1900 */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        next_effect();
        break;

    /* --- effect 2: the hi-score + times screens (1000:1514) ------------- */
    case ATTRACT_E2_LOAD:
        if (!e2_load()) {
            if (escape_test(in)) { latch_escape(); break; }
            phase = ATTRACT_E4_LOAD;
        } else {
            frame = 0u;
            phase = ATTRACT_E2_FADEWAIT;
        }
        break;
    case ATTRACT_E2_FADEWAIT:                    /* fade_wait 1581 */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        phase = ATTRACT_E2_FRAME;
        break;
    case ATTRACT_E2_FRAME: {                     /* one intro_frame body   */
        memcpy(pageB, imgBuf, 64000u);           /* copy_page 18a4 */
        memset(arena, 0, sizeof arena);          /* clear_screen_buf 1882 */
        if (frame == 0u) {                       /* intro_frame_1 1625 */
            font_drawText(arena, STR_SCORE_HEADER, 0xA0u, 4u);
            draw_score_block(arena);
        } else {                                 /* intro_frame_2 1652 */
            unsigned base = (frame - 1u) * 10u;
            font_drawText(arena, STR_TIMES_HEADER, 0xA0u, 4u);
            for (unsigned r = 0; r < 10u; r++)
                draw_times_row(arena, base + r,
                               fastest_times[base + r], 0x18u + r * 0x10u);
        }
        blit_80x200(arena);
        wipeIter = 0u;
        phase = ATTRACT_E2_WIPE;
        break;
    }
    case ATTRACT_E2_WIPE: {                      /* wipe_diagonal 16fa */
        wipe_row(wipeIter);
        flip_page();
        if (++wipeIter < 79u) break;
        blit_planar(pageB, pageDraw);            /* 171c: the full present */
        flip_page();
        counter = 490u;                          /* 164c/168a: the hold    */
        phase = ATTRACT_E2_HOLD;
        break;
    }
    case ATTRACT_E2_HOLD:                         /* fade_step_frames 164c/168a */
        if (escape_test(in)) { latch_escape(); break; }
        if (--counter != 0u) break;
        if (++frame <= 16u) { phase = ATTRACT_E2_FRAME; break; }
        palette_buildFade(0u, 256u, 0x1Eu, ramp0);   /* 159f: fade to black */
        phase = ATTRACT_E2_OUT;
        break;
    case ATTRACT_E2_OUT:                         /* JMP fade_wait 15ae */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        next_effect();
        break;

    /* --- effect 4: the credits (1000:19AF) -------------------------------- */
    case ATTRACT_E4_LOAD:
        if (!e4_load()) {
            if (escape_test(in)) { latch_escape(); break; }
            phase = ATTRACT_E1_LOAD;
        } else {
            phase = ATTRACT_E4_FADEWAIT;
        }
        break;
    case ATTRACT_E4_FADEWAIT:                   /* fade_wait 1a3c */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        scrollRow = 0;                           /* 1a3f: [0xD78] := 0    */
        phase = ATTRACT_E4_SCROLL;
        break;
    case ATTRACT_E4_SCROLL:                      /* the loop at 1a45      */
        /* blit_scroll + its trailing one-tick spin + the JMP flip: one
         * strip row per display period, the page pair alternating        */
        blit_scroll(scrollRow);
        flip_page();
        scrollRow++;
        if (scrollRow == (uint16_t)(rowsBuilt - 0xC8u)) {
            /* 1a72: the terminator check; more credtext -> the next
             * screenful in the same instant (the clear + the fill + the
             * fade_wait's one poll, the fade being done), then row 0 on
             * the next tick.  The bound mirrors the original's heap-read
             * past the buffer only for data without the 0xFF terminator,
             * which the shipped file always has. */
            unsigned next = (credCursor < credtextSize)
                          ? credtext[credCursor] : 0xFFu;
            if (next != 0xFFu) {
                e4_fill();
                if (escape_test(in)) { latch_escape(); break; }
                scrollRow = 0;
                break;
            }
            /* 1a82: the tail — [0x1E7C]:=0x0A (write-only) */
            palette_buildFade(0u, 256u, 0x1Eu, ramp0);
            phase = ATTRACT_E4_OUT;
            break;
        }
        /* 1a58: the LOCAL poll — a key ends the effect via the tail
         * ([0xD3D]:=0xFF, write-only); it is not the attract escape      */
        if (local_key(in)) {
            palette_buildFade(0u, 256u, 0x1Eu, ramp0);
            phase = ATTRACT_E4_OUT;
        }
        break;
    case ATTRACT_E4_OUT:                         /* fade_wait 1a96 */
        if (escape_test(in)) { latch_escape(); break; }
        if (palette_fadeActive()) break;
        next_effect();                           /* -> effect 1: the cycle */
        break;

    /* --- the escape: the longjmp 13B2's fade, waited without polling ---- */
    case ATTRACT_ESCAPE:
        if (palette_fadeActive()) break;         /* 8b8b: wait_palette_fade_done */
        unload_img();
        free(credtext);  credtext = NULL;        /* [0xC9F] released       */
        free(textSpr);  textSpr = NULL;
        font_publish(NULL);
        phase = ATTRACT_DONE;
        break;

    case ATTRACT_DONE:
    case ATTRACT_IDLE:
        break;
    }

    compose_window();
    return phase != ATTRACT_DONE;
}
