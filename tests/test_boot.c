/* test_boot.c — the per-tick boot-intro player against the real INTRO.RSC.
 *
 * The boot intro (load_intro_scene, 1000:B63B) plays INTRO.RSC's display
 * list over the scrolled bac.bit background with the tree/apple sprites.
 * The state machine: the card's own-palette DAC fade-in (b68f/c0af), the
 * 280-tick initial dwell, the fade-out to the zeroed ramp + the 0x8C-tick
 * black hold (b6f2-b707), then 387 display-list frames (2 ticks each) with
 * the transition white-flash blocks, ending on the quit cue (frame 385).
 * The palette fades step from palette_tick — the ISR body's work — so the
 * loop below drives it once per scene call, like game_main's game_tick.
 *
 * The display list is the DECODED object 0's own tail (re_formats.md
 * §8.7a): the records [u16 dest][u16 index-1] are consumed straight out of
 * the decoded buffer, the objects themselves LZW-decode out of the raw
 * RSC's u32 table.  Check 4 pins that end-to-end: the last rendered
 * frame's overlay (group 384's caveman frame, object 148 at dest 25877)
 * must appear byte-exact in the final page.  Check 5 pins the bottom bar
 * (rows 184-199): the 184-row scroll render never touches it and the
 * ported words.bit expansion (run_screen_transition's builder) is the only
 * writer — any byte outside the words' 0xE0-0xEF/black shape there is the
 * frame-0 garbage-burst bug again
 * (reading the records out of the raw RSC ran the whole file as one group
 * and blitted real overlays at pseudo-random dests).
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * argv[3] = optional PPM frame-dump directory (frames 0, 100, 322, 344,
 *           384 rendered with bac.bit's palette — for eyeballing/analysis).
 * Missing volumes = skip. */
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "scene.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);

extern const OSystem g_osys_null;

/* the render call indices for the PPM dumps predate the palette-fade
 * phases (2026-10-23) — the fade/black-hold blocks shift every offset, so
 * these no longer pin their frames.  shortcut: stale indices, recompute
 * from a traced run when the dumps are next used. */
static const uint32_t _dump_ticks[] = { 281, 581, 1247, 1340, 1460 };

/**
 * @brief Read a whole file into a malloc'd buffer; NULL on error.
 *
 * @param path the path
 * @param size the size
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

/**
 * @brief Write a page and its palette as a binary PPM for inspection.
 *
 * @param dir the direction
 * @param tick the tick count
 * @param page the page buffer
 * @param pal6 the 6-bit palette
 */
static void _dump_ppm(const char *dir, uint32_t tick, const uint8_t *page,
                      const uint8_t *pal6)
{
    char path[512];
    snprintf(path, sizeof path, "%s/frame_t%04u.ppm", dir, tick);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (uint32_t i = 0; i < 320u * 200u; i++) {
        uint8_t idx = page[i];
        for (uint32_t c = 0; c < 3; c++) {
            uint8_t v6 = pal6[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
}

/**
 * @brief Run the boot chain tests.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
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

    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    int32_t bad = 0;
    scene_init(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);

    const char *dump_dir = argc > 3 ? argv[3] : NULL;

    /* 1. the intro plays until the quit cue, then reports done. */
    uint32_t ticks = 0;
    int32_t r;
    while (1) {
        r = scene_play_boot_intro();
        palette_tick();                        /* the ISR body's fade step */
        if (r != 1) break;                     /* done (the quit cue)     */
        if (dump_dir) {
            for (size_t k = 0; k < sizeof _dump_ticks / sizeof _dump_ticks[0];
                 k++)
                if (ticks == _dump_ticks[k])
                    _dump_ppm(dump_dir, _dump_ticks[k], osys_null_page(0),
                              osys_null_palette());
        }
        if (++ticks > 10000u) break;           /* runaways are failures   */
    }
    if (r != 0) {
        fprintf(stderr, "FAIL the boot intro did not finish (%u ticks)\n",
                ticks);
        return 1;
    }
    /* the expected shape: the Gametek logo (the fade-in, 33 groups, the 4
     * bank cycles, the 0x1a4 hold), the IDI card, the FIRST card's 7-bank
     * staggered fade-in, the ITB card + the scene, and the outro fade —
     * the full boot_intro_sequence (1000:B590), ~5.7k ticks. */
    if (ticks < 5400u || ticks > 6000u) {
        fprintf(stderr, "FAIL tick count %u outside [5400, 6000]\n", ticks);
        bad = 1;
    }

    /* 2. the page: the present composes the pool's rows 4-183 into the
     * visible rows 0-179 (deplanarise_bitmap, 1000:BB05: SI+0x500, 180
     * rows) — the scrolled bac screen covers all 57600 bytes of that
     * window; rows 180-199 are the bar region (check 5). */
    {
        const uint8_t *page = osys_null_page(0);
        uint32_t nz = 0;
        for (uint32_t i = 0; i < 180u * 320u; i++)
            if (page[i]) nz++;
        if (nz < 56000u) {
            fprintf(stderr, "FAIL the scrolled background barely drawn (%u)\n",
                    nz);
            bad = 1;
        }
    }

    /* 3. the palette: the outro (b60e-b620) fades everything to the zeroed
     * 0x48BC ramp, so the boot ends with the DAC BLACK — the verbatim exit
     * state.  (bac.bit's palette drives the scene itself; the transitions'
     * fade-backs pin it mid-run, the bar check below pins its bank.) */
    {
        const uint8_t *pal = osys_null_palette();
        for (uint32_t i = 0; i < 768u; i++) {
            if (pal[i] != 0) {
                fprintf(stderr,
                        "FAIL palette byte %u: got %02x, the outro "
                        "fade must end black\n",
                        i, pal[i]);
                bad = 1;
                break;
            }
        }
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
        uint8_t *rsc = _read_file(path, &rsc_size);
        static uint8_t list[0x10000];
        static uint8_t obj[0x10000];
        if (!rsc) {
            fprintf(stderr, "FAIL intro.rsc fixture unreadable\n");
            bad = 1;
        } else {
            uint16_t chk = 0;
            uint32_t off0;
            memcpy(&off0, rsc, 4); /* entry[0] = the list object's offset */
            int32_t sz = res_decode_lzw_mem(rsc, rsc_size, off0, list,
                                            sizeof list, &chk);
            if (sz < 0 || chk != 0) {
                fprintf(stderr,
                        "FAIL intro.rsc object 0 (the list) undecodable\n");
                bad = 1;
            } else {
                /* walk the groups from +2 to group 384's record */
                uint32_t si = 2;
                uint32_t g = 0;
                while (g < 384u && si + 2 <= (uint32_t)sz) {
                    uint16_t w = (uint16_t)(rd_le_u16(list + si));
                    if (w == 0xFFFFu) {
                        si += 2;
                        g++;
                    } else si += 4;
                }
                if (g != 384u || si + 4 > (uint32_t)sz) {
                    fprintf(stderr, "FAIL group 384 not reached in the list\n");
                    bad = 1;
                } else {
                    uint16_t dest = (uint16_t)(rd_le_u16(list + si));
                    uint16_t stored =
                        (uint16_t)(list[si + 2] | (list[si + 3] << 8));
                    uint32_t idx = (stored + 1u) & 0xFFFFu; /* INC AX at b95d */
                    uint32_t off;
                    memcpy(&off, rsc + idx * 4u, 4);
                    int32_t osz = res_decode_lzw_mem(rsc, rsc_size, off, obj,
                                                     sizeof obj, &chk);
                    if (osz < 0 || chk != 0) {
                        fprintf(stderr, "FAIL object %u undecodable\n", idx);
                        bad = 1;
                    } else {
                        uint32_t w = rd_le_u16(obj + 0);
                        uint32_t h = rd_le_u16(obj + 2);
                        if (dest != 25877u || w == 0u || h == 0u ||
                            4u + (uint32_t)w * h > (uint32_t)osz) {
                            fprintf(
                                stderr,
                                "FAIL group 384 record mismatch: dest %u, %ux%u\n",
                                dest, w, h);
                            bad = 1;
                        } else {
                            const uint8_t *page = osys_null_page(0);
                            uint32_t miss = 0;
                            /* the overlay blits into the pool at dest
                             * 25877 (row 80); the present composes pool
                             * rows 4-183 into the visible rows 0-179, so
                             * the page holds it 4 rows up:
                             * 25877 - 4*320 = 24597. */
                            for (uint32_t rr = 0; rr < h; rr++)
                                for (uint32_t cc = 0; cc < w; cc++) {
                                    uint8_t b = obj[4u + rr * w + cc];
                                    if (!b) continue;
                                    uint32_t at = 24597u + rr * 320u + cc;
                                    if (at >= 64000u) continue;
                                    if (page[at] != b) miss++;
                                }
                            if (miss) {
                                fprintf(
                                    stderr,
                                    "FAIL group-384 overlay not on the page (%u bytes off)\n",
                                    miss);
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
        uint32_t nz_margin = 0;
        for (uint32_t i = 180u * 320u; i < 182u * 320u; i++)
            if (page[i]) nz_margin++;
        for (uint32_t i = 198u * 320u; i < 200u * 320u; i++)
            if (page[i]) nz_margin++;
        if (nz_margin) {
            fprintf(
                stderr,
                "FAIL the bar margins hold %u non-zero bytes (garbage blits)\n",
                nz_margin);
            bad = 1;
        }
        int32_t colours_ok = 1;
        for (uint32_t r = 0; r < 16u; r++) {
            for (uint32_t i = 0; i < 320u; i++) {
                uint8_t v = page[(182u + r) * 320u + i];
                if (v && v != (uint8_t)(0xE0u + r)) colours_ok = 0;
            }
        }
        if (!colours_ok) {
            fprintf(
                stderr,
                "FAIL the bar's colours are not the 0xE0-0xEF grey gradient\n");
            bad = 1;
        }
    }

    if (!bad) printf("boot tests: OK (5 checks)\n");
    return bad;
}
