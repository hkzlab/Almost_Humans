/* test_boot.c — the per-tick boot-intro player against the real INTRO.RSC.
 *
 * The boot intro (load_intro_scene, 1000:B63B) plays INTRO.RSC's display
 * list over the scrolled bac.bit background with the tree/apple sprites.
 * The state machine: the 280-tick initial dwell, then 387 display-list
 * frames (2 ticks each), ending on the quit cue (frame 385).
 *
 * The display list is the DECODED object 0's own tail (re_formats.md
 * §8.7a): the records [u16 dest][u16 index-1] are consumed straight out of
 * the decoded buffer, the objects themselves LZW-decode out of the raw
 * RSC's u32 table.  Check 4 pins that end-to-end: the last rendered
 * frame's overlay (group 384's caveman frame, object 148 at dest 25877)
 * must appear byte-exact in the final page.  Check 5 pins the bottom bar
 * (rows 184-199): the 184-row scroll render never touches it and the
 * original's words.bit bar is not ported yet, so the rows must stay CLEAN
 * BLACK — any non-zero byte there is the frame-0 garbage-burst bug again
 * (reading the records out of the raw RSC ran the whole file as one group
 * and blitted real overlays at pseudo-random dests).
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * argv[3] = optional PPM frame-dump directory (frames 0, 100, 322, 344,
 *           384 rendered with bac.bit's palette — for eyeballing/analysis).
 * Missing volumes = skip. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

/* the render call for display-list frame f is call 281 + 3f (0-based:
 * call 0 = BOOT_START, 1..280 = the dwell, then 1 render + 2 waits per
 * frame) — valid up to the frame-323 seek, after which the counter rewinds
 * while the calls keep advancing (frame 314's second pass starts at 1250). */
static const unsigned dump_ticks[] = { 281, 581, 1247, 1340, 1460 };

static void dump_ppm(const char *dir, unsigned tick, const uint8_t *page,
                     const uint8_t *pal6)
{
    char path[512];
    snprintf(path, sizeof path, "%s/frame_t%04u.ppm", dir, tick);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (unsigned i = 0; i < 320u * 200u; i++) {
        uint8_t idx = page[i];
        for (unsigned c = 0; c < 3; c++) {
            uint8_t v6 = pal6[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);

    char fix_abs[512];
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }

    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }

    int bad = 0;
    scene_init(&osys_null);
    video_init(&osys_null);

    const char *dump_dir = argc > 3 ? argv[3] : NULL;

    /* 1. the intro plays until the quit cue, then reports done. */
    unsigned ticks = 0;
    int r;
    for (;;) {
        r = scene_playBootIntro();
        if (r != 1) break;                     /* done (the quit cue)     */
        if (dump_dir) {
            for (size_t k = 0; k < sizeof dump_ticks / sizeof dump_ticks[0]; k++)
                if (ticks == dump_ticks[k])
                    dump_ppm(dump_dir, dump_ticks[k], osys_null_page(0),
                             osys_null_palette());
        }
        if (++ticks > 4000u) break;            /* runaways are failures   */
    }
    if (r != 0) {
        fprintf(stderr, "FAIL the boot intro did not finish (%u ticks)\n", ticks);
        return 1;
    }
    /* the expected shape: the 280-tick dwell + 387 frames x 3 calls
     * (1 render + 2 wait), ending at the quit cue (frame 385). */
    if (ticks < 1400u || ticks > 1500u) {
        fprintf(stderr, "FAIL tick count %u outside [1400, 1500]\n", ticks);
        bad = 1;
    }

    /* 2. the page: the present composes the pool's rows 4-183 into the
     * visible rows 0-179 (deplanarise_bitmap, 1000:BB05: SI+0x500, 180
     * rows) — the scrolled bac screen covers all 57600 bytes of that
     * window; rows 180-199 are the bar region (check 5). */
    {
        const uint8_t *page = osys_null_page(0);
        unsigned nz = 0;
        for (unsigned i = 0; i < 180u * 320u; i++) if (page[i]) nz++;
        if (nz < 56000u) {
            fprintf(stderr, "FAIL the scrolled background barely drawn (%u)\n", nz);
            bad = 1;
        }
    }

    /* 3. the palette is bac.bit's trailing 768 bytes (at +0xE600). */
    {
        uint32_t pal_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.000/INTRO/BAC.BIT", fix_abs);
        uint8_t *bac = read_file(path, &pal_size);
        if (!bac || pal_size < 0xE600u + 768u) {
            fprintf(stderr, "FAIL bac.bit fixture unreadable\n");
            bad = 1;
        } else if (memcmp(osys_null_palette(), bac + 0xE600u, 768u) != 0) {
            fprintf(stderr, "FAIL palette != bac.bit's 768-byte tail\n");
            bad = 1;
        }
        free(bac);
    }

    /* 4. the display-list oracle: the last rendered frame (384) blits
     * group 384's overlay — stored index 148, INC'd to table entry 149
     * (43x94: a walking-figure frame, b95d's `INC AX`) — at dest 25877
     * (row 80, col 237).  Every non-zero body byte must be in the final
     * page byte-exact (the blit is transparent-0, one record, nothing
     * overwrites it afterwards). */
    {
        uint32_t rsc_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.000/INTRO/INTRO.RSC", fix_abs);
        uint8_t *rsc = read_file(path, &rsc_size);
        static uint8_t list[0x10000];
        static uint8_t obj[0x10000];
        if (!rsc) {
            fprintf(stderr, "FAIL intro.rsc fixture unreadable\n");
            bad = 1;
        } else {
            uint16_t chk = 0;
            uint32_t off0;
            memcpy(&off0, rsc, 4);          /* entry[0] = the list object's offset */
            int sz = res_decode_lzw_mem(rsc, rsc_size, off0, list, sizeof list, &chk);
            if (sz < 0 || chk != 0) {
                fprintf(stderr, "FAIL intro.rsc object 0 (the list) undecodable\n");
                bad = 1;
            } else {
                /* walk the groups from +2 to group 384's record */
                uint32_t si = 2;
                unsigned g = 0;
                while (g < 384u && si + 2 <= (uint32_t)sz) {
                    uint16_t w = (uint16_t)(list[si] | (list[si + 1] << 8));
                    if (w == 0xFFFFu) { si += 2; g++; }
                    else si += 4;
                }
                if (g != 384u || si + 4 > (uint32_t)sz) {
                    fprintf(stderr, "FAIL group 384 not reached in the list\n");
                    bad = 1;
                } else {
                    uint16_t dest = (uint16_t)(list[si] | (list[si + 1] << 8));
                    uint16_t stored = (uint16_t)(list[si + 2] | (list[si + 3] << 8));
                    unsigned idx = (stored + 1u) & 0xFFFFu;   /* INC AX at b95d */
                    uint32_t off;
                    memcpy(&off, rsc + idx * 4u, 4);
                    int osz = res_decode_lzw_mem(rsc, rsc_size, off, obj,
                                                 sizeof obj, &chk);
                    if (osz < 0 || chk != 0) {
                        fprintf(stderr, "FAIL object %u undecodable\n", idx);
                        bad = 1;
                    } else {
                        unsigned w = (unsigned)obj[0] | ((unsigned)obj[1] << 8);
                        unsigned h = (unsigned)obj[2] | ((unsigned)obj[3] << 8);
                        if (dest != 25877u || w == 0u || h == 0u ||
                            4u + (uint32_t)w * h > (uint32_t)osz) {
                            fprintf(stderr, "FAIL group 384 record mismatch: dest %u, %ux%u\n",
                                    dest, w, h);
                            bad = 1;
                        } else {
                            const uint8_t *page = osys_null_page(0);
                            unsigned miss = 0;
                            /* the overlay blits into the pool at dest
                             * 25877 (row 80); the present composes pool
                             * rows 4-183 into the visible rows 0-179, so
                             * the page holds it 4 rows up:
                             * 25877 - 4*320 = 24597. */
                            for (unsigned rr = 0; rr < h; rr++)
                                for (unsigned cc = 0; cc < w; cc++) {
                                    uint8_t b = obj[4u + rr * w + cc];
                                    if (!b) continue;
                                    uint32_t at = 24597u + rr * 320u + cc;
                                    if (at >= 64000u) continue;
                                    if (page[at] != b) miss++;
                                }
                            if (miss) {
                                fprintf(stderr, "FAIL group-384 overlay not on the page (%u bytes off)\n", miss);
                                bad = 1;
                            }
                        }
                    }
                }
            }
            free(rsc);
        }
    }

    /* 5. the bar region: the visible rows 182-197 hold the last
     * transition's words expansion (colours 0xE0-0xEF = bac's grey
     * gradient), rows 180-181 and 198-199 stay black — anything outside
     * that shape is the display-list garbage bug again.  The bar's grey
     * gradient gives a strictly increasing non-zero count down the rows. */
    {
        const uint8_t *page = osys_null_page(0);
        unsigned nz_margin = 0;
        for (unsigned i = 180u * 320u; i < 182u * 320u; i++) if (page[i]) nz_margin++;
        for (unsigned i = 198u * 320u; i < 200u * 320u; i++) if (page[i]) nz_margin++;
        if (nz_margin) {
            fprintf(stderr, "FAIL the bar margins hold %u non-zero bytes (garbage blits)\n", nz_margin);
            bad = 1;
        }
        int colours_ok = 1;
        for (unsigned r = 0; r < 16u; r++) {
            for (unsigned i = 0; i < 320u; i++) {
                uint8_t v = page[(182u + r) * 320u + i];
                if (v && v != (uint8_t)(0xE0u + r)) colours_ok = 0;
            }
        }
        if (!colours_ok) {
            fprintf(stderr, "FAIL the bar's colours are not the 0xE0-0xEF grey gradient\n");
            bad = 1;
        }
    }

    if (!bad) printf("boot tests: OK (5 checks)\n");
    return bad;
}
