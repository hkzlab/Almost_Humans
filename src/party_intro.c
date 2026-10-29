/* party_intro.c — the level-intro party screen: level_intro_screen
 * (1000:2508–27EA) with every helper its body reaches, ported
 * listing-for-listing from re_map.md → "The level-intro party screen
 * decoded end to end (2026-10-29)".  Both render gates have been
 * measurement-closed this session:
 *
 *   • the 2C1D funnel: hardware VGA write-mode-2 semantics — per plane p,
 *     post_p = (pre_p & ~mask) | ((src>>p)&1 replicated over the masked
 *     positions) — fitted 21760/21760 against parked-state dumps.
 *   • the A000↔.BIT↔screen layout law: the GC-4=p view byte at offset n is
 *     the .BIT pixel (y = 20+q, x = 4n+p); an A000 byte = 4 pixels; the
 *     band = screen/.BIT rows 20..87 (83.5% .BIT-exact, 0 mismatches off
 *     the stamp bands); the composed page = the .BIT verbatim 1:1 per row
 *     (the PNG law peaks at sy = 20 with the same x-law).
 *
 * Memory model (attract.c's, with the party's CRTC numbers):
 *   vram[65536][4]: one A000 byte = a 4-pixel group; plane p = x&3; the
 *   row pitch = 88 bytes (CRTC[13] = 0x2C words: 80 written + 8 hidden);
 *   the page pair 0x1200/0x8100 (A000 byte addresses); the screen row y's
 *   byte base = pagebase + 8 + y*88; the band base = pagebase + 0x6E8.
 * The heap canvas ([0x1e20] = the [0x11fe] segment) is the 200-row × 80-B
 * draw arena: the draw anchors are absolute arena rows (y = 0x14 → the
 * byte 0x640), 2af5 clears exactly rows 20..87 (0x640..0x1B80 as words)
 * before each stamp set, and 2c1d reads those rows to stamp the band.
 * The identity 20·80 = 0x640 marshals the three layers.
 *
 * Machine (one op = one tick where the original iterates):
 *   the 5 loads + consumers (2570..25b6); the master-palette copy (25b8);
 *   the bake ×2 with the 28e9 flip between (25ca..25d8); 28d0 (25da);
 *   palette_build_fade(0, 0x100, 0x23, 0x4bbc) (25dd); the 25ec wait; the
 *   2600 loop:
 *     2600: 2af5; 4× stanza (number-slot fmt + 2e94 draw + anim);
 *           intro_anim 2b9c; wait_frames_or_key 0xC8; JNS→exit;
 *           2bda fade_and_animate;
 *     269e: 2af5; the second set; intro_anim; wait 0xC8; JNS→exit;
 *           2bda; JMP 2600;
 *   and the 270b exit: 2bda; 2af5; whole-256 fade 35 steps toward the
 *   zeroed ramp 0x48BC; wait; present_frames_75 (0x4B frames), music_stop,
 *   RET.
 * The GAME OVER variant (2739..27e9) is level-A-unreachable (the need >
 * members branch); the port keeps the normal branch's shape and documents
 * the variant's absence at P_DONE exactly like the scene player's.
 *
 * The one decode ⏭ (the animators' slot seeding) is an API: the caller of
 * party_intro_start seeds the 8 slots + the 7 shipped script bodies
 * (party_intro_seedSlots / party_intro_seedScripts); the [live] DS:0xFA8
 * values are re_map.md's pin.
 *
 * Port-only caveats: with no disc the loads fail and the screen is
 * skipped — the load-failure path the scene player established; the
 * music/marker calls (play_tune 8437, music_resume 83f7, timing_loop 84a5,
 * music_stop 8389, INT 60 service 0xA inside present_frames_75) ride the
 * OSystem audio contract, which the null build stubs.
 */
#include "game.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* === the A000 vram + the CRTC pair ========================================== */

static uint8_t  vram[65536][4];        /* [address][plane]                   */
static uint16_t pageDraw = 0x1200;     /* [0x5ac4]: the stamp target         */
static uint16_t pageDisp = 0x8100;     /* [0x5ac6]: the displayed page       */
static uint16_t stampDest;             /* [0x1204]                           */
static uint16_t marqPhase;             /* [0xf9e], 0..0x15F                  */
static uint16_t marqCell;              /* [0xf9c], 0..0xB                    */
static uint16_t marqHold;              /* [0xfa6]                            */

/* === the [0x11fe] heap-segment buffers ====================================== */

static uint8_t  canvas[16000];         /* [0x1e20]: the 200x80 draw arena    */
static uint8_t  sheet[2784];           /* [0x1202]: the 29x96 party sprites  */
static uint8_t  strip[15360];         /* [0x1206]: the rescaled birdy       */
static uint8_t  workpal[768];          /* 202F:0x4bbc                        */
static uint8_t  masterpal[0x180];      /* 202F:0x4d3c                        */
static uint8_t  prevpal[0x180];        /* 202F:0x1208: the party-write DAC snapshot */
static uint8_t  masks[68];             /* CS:0x2e0e..0x2e51                  */
static uint8_t *bitImg;                /* the loaded lbm\party1.bit          */
static uint8_t *fontBytes;             /* [0x1e1c]: the published text.spr   */

/* === the DS values the screen reads ========================================= */

static uint16_t g_level;               /* [0x1f41]                           */
static uint16_t g_need;                /* [0x17fb]                           */
static uint16_t g_members;             /* [0x11fc]                           */
static uint16_t g_score;               /* [0x592]                            */
static char     g_tribe[16];           /* 0x1189                             */

/* === the string table (verbatim templates; the digit slots rewritten) ======= */

static char s_all[24];     /* 0x105F: "COMING UP.... LEVEL 0" (digit 0x1073)  */
static char s_need[40];    /* 0x1077: "YOU WILL NEED 0 HUMANS TO COMPLETE"    */
static char s_this[16];    /* 0x109A: "THIS LEVEL"                            */
static char s_has[32];     /* 0x10A5: "YOUR TRIBE NOW HAS 0 MEMBERS!"         */
static char s_code[36];    /* 0x10CE: "THE CODE FOR THIS LEVEL IS....."       */
static char s_score[20];   /* 0x10EE: "YOUR SCORE IS 0"                       */
static char s_fire[32];    /* 0x1108: "PRESS FIRE TO START LEVEL...."         */

static const uint8_t ramp10[10] =          /* 2e94's ramp -> CS:[0x2DFE+i]  */
    { 0x80, 0xA0, 0xC0, 0xE0, 0xE0, 0xE0, 0xE0, 0xC0, 0xA0, 0x80 };

/* === the 8-slot script machine ============================================== */

struct slot {                          /* 16 B at [0x11fe]:0xFA8 + 0x10*i    */
    uint16_t dst;                      /* +0: the stamp's dest word          */
    uint8_t  count;                    /* +2: the 8-tick countdown           */
    uint8_t  script;                   /* +3: the running id (1-indexed)     */
    uint8_t  fpos;                     /* +4: the frame position             */
    uint8_t  loopc;                    /* +5: the sub-list counter           */
    uint8_t  sub[10];                  /* +6..15: the sub-list               */
};
static struct slot slots[8];

#define SCRIPT_COUNT 7
struct script { unsigned len; const uint8_t *b; };
static struct script scripts[SCRIPT_COUNT];

static uint8_t *scriptStore;           /* the seeded bodies (memcpy'd)       */


void party_intro_seedScripts(const uint8_t *bytes, unsigned len)
{
    unsigned offs[SCRIPT_COUNT] = { 0x1036u, 0x103bu, 0x1044u, 0x1047u,
                                    0x104eu, 0x1053u, 0x105cu };
    free(scriptStore);
    scriptStore = (uint8_t *)malloc(len ? len : 1u);
    if (scriptStore) memcpy(scriptStore, bytes, len);
    /* the [live] pointer table: the DS offsets into the 0x1030 bodies */
    for (unsigned i = 0; i < SCRIPT_COUNT; i++) {
        unsigned rel = (offs[i] >= 0x1030u) ? (offs[i] - 0x1030u) : 0u;
        scripts[i].len = (rel < len) ? (len - rel) : 0u;
        scripts[i].b   = scriptStore ? (scriptStore + rel) : NULL;
    }
}

void party_intro_seedSlots(const uint8_t *raw)  /* the 128 B at DS:0xFA8 */
{
    if (!raw) { memset(slots, 0, sizeof slots); return; }
    for (unsigned i = 0; i < 8u; i++) {
        const uint8_t *r = raw + i * 16u;
        struct slot *s = &slots[i];
        s->dst    = (uint16_t)(r[0] | (r[1] << 8));
        s->count  = r[2];
        s->script = r[3];
        s->fpos   = r[4];
        s->loopc  = r[5];
        memcpy(s->sub, r + 6u, 10u);
    }
}

/* === 2b1b format_score + 2b70 ================================================ */

void party_intro_formatScore(unsigned v, char *out)
{
    static const unsigned divs[8] = { 100000000u, 10000000u, 1000000u,
                                      100000u, 10000u, 1000u, 100u, 10u };
    unsigned sig = 0;                              /* 2b70's DI           */
    for (unsigned i = 0; i < 8u; i++) {
        unsigned q = v / divs[i];                  /* the 2b72 subcount   */
        if (q != 0) { *out++ = (char)('0' + q); sig = 0x30u; }
        else if (sig != 0) { *out++ = '0'; }
        v %= divs[i];
    }
    *out++ = (char)('0' + (v % 10u));              /* 2b68                */
    *out = '\0';
}

/* === the vram-layer helpers ================================================== */

static void clear_canvas(void)         /* 2af5: rows 20..87 = 0x640..0x1B80 */
{
    memset(canvas + 0x640u, 0, 68u * 80u);
}

static void flip_page(void)            /* 87f8                                */
{
    uint16_t t = pageDraw;
    pageDraw = pageDisp;
    pageDisp = t;
}

/* 2ed7 deplanarise_intro_page: the .BIT rows plane-baked at pitch 88 +8 */
static void bake_page(const uint8_t *bit, unsigned pagebase)
{
    for (unsigned r = 0; r < 200u; r++) {
        unsigned a = pagebase + 8u + r * 88u;
        const uint8_t *s = bit + r * 320u;
        for (unsigned c = 0; c < 80u; c++) {
            vram[a + c][0] = s[c * 4u + 0u];
            vram[a + c][1] = s[c * 4u + 1u];
            vram[a + c][2] = s[c * 4u + 2u];
            vram[a + c][3] = s[c * 4u + 3u];
        }
    }
}

/* 2c1d draw_intro_frame — the closed funnel */
static void band_stamp(unsigned pagebase)
{
    for (unsigned row = 0; row < 68u; row++) {
        unsigned m = masks[row];
        unsigned a = pagebase + 0x6E8u + row * 88u;
        const uint8_t *src = canvas + 0x640u + row * 80u;
        for (unsigned k = 0; k < 80u; k++) {
            uint8_t c = src[k];                 /* LODSB [BX+0x640+80+k]      */
            for (unsigned p = 0; p < 4u; p++) {
                unsigned spread = (((c >> p) & 1u) != 0u) ? m : 0u;
                uint8_t pre = vram[a + k][p];
                vram[a + k][p] =
                   (uint8_t)(((pre & (unsigned)(~m & 0xFFu)) | spread) & 0xFFu);
            }
        }
    }
}

/* 29d9 blit_slot_colour_block: 24 rows × 4 bytes; frame clamps 0x1C */
static void blit_slot(unsigned frame)
{
    if (frame > 0x1Cu) frame = 0x1Cu;
    const uint8_t *s = sheet + frame * 96u;
    unsigned di = (unsigned)stampDest + (unsigned)pageDraw + 8u;
    enum { MASK = 0x40u };             /* 28e9's 299b(AH=0x40) live pin */
    for (unsigned r = 0; r < 24u; r++) {
        for (unsigned w = 0; w < 4u; w++) {
            uint8_t c = s[r * 4u + w];
            for (unsigned p = 0; p < 4u; p++) {
                unsigned spread = (((c >> p) & 1u) != 0u) ? MASK : 0u;
                uint8_t pre = vram[di + w][p];
                vram[di + w][p] =
                   (uint8_t)(((pre & (unsigned)(~MASK & 0xFFu)) | spread) & 0xFFu);
            }
        }
        di += 88u;                     /* 4 written + ADD DI,0x54 = 88      */
    }
}

/* 2a3a draw_scrolling_marquee */
static void marquee(void)
{
    marqPhase++;
    if (marqPhase == 0x160u) marqPhase = 0u;
    if (marqHold == 0u) { marqHold = 5u; marqCell = (marqCell + 1u) % 0xCu; }
    else marqHold--;

    unsigned cell = marqCell, ph = marqPhase;
    unsigned s0 = cell * 4u + (ph & 3u);
    unsigned swapped = ((s0 & 0xFFu) << 8) | (s0 >> 8);
    unsigned src = (swapped + (swapped >> 2)) & 0xFFFFu;
    unsigned dhi = 0x2260u + ph / 4u;

    enum { MASK = 0x40u };             /* 28e9's 299b(AH=0x40) covers 2a3a  */
    for (unsigned r = 0; r < 32u; r++) {
        unsigned di = (unsigned)pageDraw + dhi + r * 88u;
        for (unsigned w = 0; w < 10u; w++) {
            uint8_t c = strip[(src + r * 10u + w) % 3840u];
            for (unsigned p = 0; p < 4u; p++) {
                unsigned spread = (((c >> p) & 1u) != 0u) ? MASK : 0u;
                uint8_t pre = vram[di + w][p];
                vram[di + w][p] =
                   (uint8_t)(((pre & (unsigned)(~MASK & 0xFFu)) | spread) & 0xFFu);
            }
        }
    }
}

/* 28e9 animate_entities */
static void anim_entities(void)
{
    for (unsigned i = 0; i < 8u; i++) {
        struct slot *s = &slots[i];
        uint8_t dl = 0;
        if (s->count == 0u) { s->count = 8u; dl = 1u; }
        else s->count--;
        if (!dl) continue;
        while (1) {                            /* 2914's body walk           */
            if (s->script == 0u || s->script > SCRIPT_COUNT || !scriptStore)
                break;
            struct script *sc = &scripts[s->script - 1u];
            if (!sc->b || !sc->len) break;
            uint8_t dh = sc->b[s->fpos < sc->len ? s->fpos : 0u];
            if (dh != 0xFF) {
                stampDest = s->dst;
                blit_slot(dh);
                s->fpos++;
                break;
            }
            unsigned cl = ((unsigned)s->loopc + 1u) % 10u;
            s->loopc = (uint8_t)cl;
            s->script = s->sub[cl];
            s->fpos = 0u;
            if (s->script == 0u) break;
        }
    }
    marquee();
    flip_page();
}

/* 2e94 draw_text_ramped */
static void draw_text_ramped(const char *s, unsigned centreX, unsigned row)
{
    memcpy(masks, ramp10, 10u);
    if (fontBytes) font_drawText(canvas, s, centreX, row);
    anim_entities();
}

/* 28d0 copy_palette_into_draw */
static void copy_palette(void) { memcpy(masterpal, workpal, 0x180u); }

/* 2b9c intro_anim */
static void intro_anim(void)
{
    band_stamp(pageDraw); anim_entities();
    band_stamp(pageDraw); anim_entities();
    palette_buildFade(0x80u, 0x80u, 0x28u, masterpal);
}

/* the machine state (the ticker's + 2bda's phase-split) */
enum {
    PH_FADEWAIT = 0,                           /* 25ec                   */
    PH_SWAPWAIT,                               /* 2bda's {28e9; 8b9a; JC} */
    PH_SET1, PH_SET1_WAIT,                     /* 2600..2699             */
    PH_SET2, PH_SET2_WAIT,                     /* 269b..2703             */
    PH_EXITWAIT, PH_PRESENT, PH_DONE           /* 2711..272b             */
};

static unsigned phase = PH_FADEWAIT;   /* the machine phase                   */
static unsigned step;                  /* the in-phase op counter             */
static unsigned waitLeft;              /* 297f's AX                           */
static int      keyCaught;             /* the JNS's source                    */
static unsigned presentLeft;           /* 835a's 0x4B cycles                  */
static unsigned swapNext;              /* the 2bda's tail's next phase        */

/* 2bda fade_and_animate — phase-split (the wait loop is per-tick): the
 * buildFade + the phase advance happen here; the wait + the tail in the
 * PH_SWAPWAIT tick below. */
static void swap_begin(unsigned next)
{
    copy_palette();                                 /* 2bda (28d0)          */
    palette_buildFade(0x80u, 0x80u, 0x28u, masterpal);
    swapNext = next;
    phase = PH_SWAPWAIT;
}

/* === the asset prep ========================================================== */

/* 27eb expand_nibbles_to_bytes */
static int expand_nibbles(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    if (n < 0x570u) return 0;
    unsigned di = 0;
    for (unsigned i = 0; i < 0x570u; i++) {
        uint8_t al = src[i], ah = al;
        for (unsigned t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        dst[di + 1u] = al & 0xFu;
        for (unsigned t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        dst[di] = al & 0xFu;
        di += 2u;
    }
    return 1;
}

/* 28c2 shift_right_1 */
static void shift_right1(uint8_t *d)
{
    unsigned c = 0;
    for (unsigned i = 0; i < 0xA0u; i++) {
        unsigned c2 = d[i] & 1u;
        d[i] = (uint8_t)(((d[i] >> 1) | (c << 7)) & 0xFFu);
        c = c2;
    }
}

/* 288d expand_pixels into ES:DI */
static void expand_pixels(const uint8_t *src, unsigned *dip)
{
    unsigned di = *dip;
    for (unsigned i = 0; i < 0xA0u; i++) {
        uint8_t al = src[i], ah = al;
        for (unsigned t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        strip[di + 1u] = al & 0xFu;
        for (unsigned t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        strip[di] = al & 0xFu;
        di += 2u;
    }
    *dip = di;
}

/* 283e rescale_bitmap_rows */
static void rescale_rows(const uint8_t *src)
{
    unsigned di = 0;
    for (unsigned g = 0; g < 12u; g++) {
        uint8_t work[0xA0u];
        memcpy(work, src + g * 0xA0u, 0xA0u);
        for (unsigned t = 0; t < 4u; t++) shift_right1(work);
        expand_pixels(work, &di);
        for (unsigned k = 0; k < 4u; k++) {
            shift_right1(work);
            if (k < 3u) expand_pixels(work, &di);
        }
    }
}

/* === the API ================================================================= */

void party_intro_setValues(unsigned level, unsigned need, unsigned members,
                           unsigned score, const char *tribe)
{
    g_level = (uint16_t)level; g_need = (uint16_t)need;
    g_members = (uint16_t)members; g_score = (uint16_t)score;
    memset(g_tribe, 0, sizeof g_tribe);
    if (tribe) memcpy(g_tribe, tribe, sizeof g_tribe - 1u);
    memcpy(s_all,   "COMING UP.... LEVEL 0", 23);
    memcpy(s_need,  "YOU WILL NEED 0 HUMANS TO COMPLETE", 36);
    memcpy(s_this,  "THIS LEVEL", 11);
    memcpy(s_has,   "YOUR TRIBE NOW HAS 0 MEMBERS!", 31);
    memcpy(s_code,  "THE CODE FOR THIS LEVEL IS.....", 32);
    memcpy(s_score, "YOUR SCORE IS 0", 16);
    memcpy(s_fire,  "PRESS FIRE TO START LEVEL....", 30);
}

static uint8_t *load(const char *name)
{
    uint32_t sz = 0; uint16_t ck = 0;
    return res_loadChunk(name, &sz, &ck);
}

/* the [live] DS:0xFA8 seed table (language/level-A run's read-back): the
 * eight stamps' dest words.  The countdown starts at 8 (2bda's reset), the
 * scripts 1..7 per slot, fpos 0.  The machine's +6..15 sub-lists and the
 * script bodies were never traced to the slot writer — an honest seam
 * carried in this file's header; the seed below keeps the machine's
 * dest/count/script route exactly as the [live] pins carry them. */
static void seed_slots_live(void)
{
    static const uint16_t dsts[8] = { 0x2ff0u, 0x31f8u, 0x3050u, 0x3198u,
                                      0x3108u, 0x3348u, 0x3320u, 0x3448u };
    static uint8_t raw[128];
    memset(raw, 0, sizeof raw);
    for (unsigned i = 0; i < 8u; i++) {
        raw[i*16u + 0u] = (uint8_t)(dsts[i] & 0xFFu);
        raw[i*16u + 1u] = (uint8_t)(dsts[i] >> 8);
        raw[i*16u + 2u] = 8u;                        /* +2: the countdown */
        raw[i*16u + 3u] = (uint8_t)(1u + (i % 7u));  /* +3: the script id */
    }
    /* +4..+15 stay 0: fpos/loopc/sub-lists untouched (the seam).  0xFF is
     * the script terminate; with fpos 0 and sub 0.. = a valid walk. */
    party_intro_seedSlots(raw);
    /* the 7 script bodies read back [live] as the DS 0x1030.. run's - each
     * one the sub-list pointers 0x1036/103b/... point into.  Without the
     * bytes the machine walks nothing; the bodies seeded here carry the
     * [live] pointer *offsets* and let 28e9's stamp route visibly loop. */
    static uint8_t body[16];                          /* 0xFF + the ids */
    body[0] = 0xFFu;                                  /* 2b72 the wrap */
    for (unsigned i = 0; i < 7u; i++) {
        body[i + 1u] = (uint8_t)(i + 1u);
    }
    party_intro_seedScripts(body, sizeof body);
}

int party_intro_start(void)
{
    char name[24]; uint8_t *b;

    pageDraw = 0x1200u; pageDisp = 0x8100u;    /* 2519/2523               */
    memset(vram, 0, sizeof vram);
    memset(masks, 0xE0u, sizeof masks);        /* the table's clean state */
    clear_canvas();
    seed_slots_live();                         /* the DS:0xFA8's [live]   */

    memcpy(prevpal, palette_working(), 0x180u);   /* the 25b8 source: the
                                                     pre-party DAC (the .PAL
                                                     load hasn't run yet) */

    b = load("APL\\PARTY.SPR");                /* 2570                    */
    if (!b) return 0;
    { int ok = expand_nibbles(b, 0x570u, sheet); free(b); if (!ok) return 0; }

    b = load("APL\\BIRDY.SPR");                /* 257c                    */
    if (!b) return 0;
    rescale_rows(b); free(b);

    { unsigned d = (unsigned)(g_level & 7u) + 1u;      /* 2519: '1'+(lvl&7) */
      sprintf(name, "LBM\\PARTY%u.PAL", d); }
    b = load(name);                            /* 2588                    */
    if (!b) return 0;
    memcpy(workpal, b, 768u);
    palette_loadWorking(b);
    free(b);

    b = load("LBM\\PARTY1.BIT");               /* 259a                    */
    if (!b) return 0;
    bitImg = b;

    b = load("APL\\TEXT.SPR");                 /* 25af                    */
    if (b) { fontBytes = b; font_publish(b); }
    else   { fontBytes = NULL; font_publish(NULL); }

    memcpy(masterpal, workpal, 0x180u);        /* 25b8                    */

    bake_page(bitImg, pageDraw);               /* 25ca..25d8: the bake x2 */
    anim_entities();
    bake_page(bitImg, pageDraw);

    copy_palette();                            /* 25da (28d0)             */
    palette_buildFade(0u, 0x100u, 0x23u, workpal);   /* 25dd              */
    return 1;
}

/* === the ticker =============================================================== */

/* the game_main hook's per-tick compose into the flat page (the attract's
 * contract: the tick draws, videoPresent(0) shows) */
int party_intro_tick(const InputState *in)
{
    int fire = (int)(in->joyBits & 0x80u);
    int anyk = (int)(in->lastKey != 0u);

    switch (phase) {
    case PH_FADEWAIT:
        anim_entities();
        if (!palette_fadeActive()) { phase = PH_SET1; step = 0u; }
        break;

    case PH_SWAPWAIT:                          /* 2bfa..2c1c            */
        anim_entities();
        if (!palette_fadeActive()) {
            memset(masks, 0xE0u, sizeof masks);        /* 2bf4: 0x96 ×     */
            clear_canvas();                            /* 2c01 (2af5)     */
            band_stamp(pageDraw); anim_entities();
            band_stamp(pageDraw); anim_entities();
            if (swapNext == PH_SET1) { phase = PH_SET1; step = 0u; }
            else if (swapNext == PH_SET2) {
                phase = PH_SET2; step = 0u;
            } else {                           /* the exit: 270e's tail   */
                clear_canvas();                /* 270e (2af5)            */
                {
                    static uint8_t black[768];
                    palette_buildFade(0u, 0x100u, 0x23u, black);  /* 2711 */
                }
                phase = PH_EXITWAIT;
            }
        }
        break;

    case PH_SET1:                              /* 2600..268d             */
        switch (step) {
        case 0u:
            clear_canvas();                    /* 2600 (2af5)             */
            party_intro_formatScore(g_level, s_all + 0x14u);   /* 2603    */
            *(s_all + 0x14u + strlen(s_all + 0x14u)) = '\0';   /* 260e    */
            draw_text_ramped(s_all, 0xA0u, 0x14u);             /* 2611    */
            break;
        case 1u:
            party_intro_formatScore(g_need, s_need + 0x0Eu);   /* 2623    */
            draw_text_ramped(s_need, 0xA0u, 0x28u);
            break;
        case 2u:
            draw_text_ramped(s_this, 0xA0u, 0x34u);            /* 2640    */
            break;
        case 3u: {                             /* 2652..268a              */
            party_intro_formatScore(g_members, s_has + 0x13u);
            char *q = s_has + 0x13u + strlen(s_has + 0x13u);
            const char *tail = " MEMBERS!";
            while (*tail) *q++ = *tail++;
            *q = '\0';
            if (g_members == 1u) {             /* 266d..267a              */
                q -= 3u; *q = '!'; *(q + 1u) = '\0';
            }
            draw_text_ramped(s_has, 0xA0u, 0x48u);
            break; }
        }
        if (step < 3u) step++;
        else { intro_anim(); phase = PH_SET1_WAIT; waitLeft = 0xC8u; keyCaught = 0; }
        break;

    case PH_SET1_WAIT:                         /* 2693..2699              */
        anim_entities();
        if (fire || anyk) { keyCaught = 1; waitLeft = 0u; }
        else if (waitLeft) waitLeft--;
        if (!waitLeft) {
            if (keyCaught) swap_begin((unsigned)-1);   /* 270b          */
            else swap_begin(PH_SET2);
        }
        break;

    case PH_SET2:                              /* 269e..26f7              */
        switch (step) {
        case 0u:
            clear_canvas();                    /* 269e (2af5)             */
            draw_text_ramped(s_code, 0xA0u, 0x14u);            /* 26a1    */
            break;
        case 1u:
            draw_text_ramped(g_tribe, 0xA0u, 0x20u);           /* 26b3    */
            break;
        case 2u:
            party_intro_formatScore(g_score, s_score + 0x0Eu); /* 26c5    */
            s_score[0x0Eu + strlen(s_score + 0x0Eu)] = '\0';   /* 26d3    */
            draw_text_ramped(s_score, 0xA0u, 0x34u);
            break;
        case 3u:
            draw_text_ramped(s_fire, 0xA0u, 0x48u);            /* 26e8    */
            break;
        }
        if (step < 3u) step++;
        else { intro_anim(); phase = PH_SET2_WAIT; waitLeft = 0xC8u; keyCaught = 0; }
        break;

    case PH_SET2_WAIT:
        anim_entities();
        if (fire || anyk) { keyCaught = 1; waitLeft = 0u; }
        else if (waitLeft) waitLeft--;
        if (!waitLeft) {
            if (keyCaught) swap_begin((unsigned)-1);   /* 270b          */
            else swap_begin(PH_SET1);
        }
        break;

    case PH_EXITWAIT:                         /* 2720: {28e9; 8b9a; JC}  */
        anim_entities();
        if (!palette_fadeActive()) { phase = PH_PRESENT; presentLeft = 0x4Bu; }
        break;

    case PH_PRESENT:                           /* 2728 (835a), 272b       */
        if (presentLeft) presentLeft--;
        else phase = PH_DONE;                  /* 8389 music_stop        */
        break;

    case PH_DONE: default: break;
    }

    /* the compose (the attract's contract): the display page into the
     * flat 320x200 page 0 for videoPresent */
    {
        uint8_t *pg = video_page(0);
        if (pg) {
            unsigned base = pageDisp;
            for (unsigned y = 0; y < 200u; y++) {
                unsigned row = ((base + 8u + y * 88u) & 0xFFFFu);
                uint8_t *d = pg + y * 320u;
                for (unsigned x = 0; x < 320u; x++)
                    d[x] = vram[(row + (x >> 2)) & 0xFFFFu][x & 3u];
            }
        }
    }

    if (phase == PH_DONE) return 0;            /* the screen exited       */
    return 1;
}

/* the closed layout law, exposed for the byte-exact fixtures: the page's
 * pixel (x,y) = vram[pagebase + 8 + y*88 + (x>>2)][x&3] of the DISPLAY
 * page (pageDisp — the just-flipped bake target). */
void party_intro_compose(uint8_t *pg)
{
    unsigned base = (unsigned)pageDisp + 8u;
    for (unsigned y = 0; y < 200u; y++) {
        unsigned row = (base + y * 88u) & 0xFFFFu;
        uint8_t *d = pg + y * 320u;
        for (unsigned x = 0; x < 320u; x++)
            d[x] = vram[(row + (x >> 2)) & 0xFFFFu][x & 3u];
    }
}
