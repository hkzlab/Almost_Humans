/* diag_name.c — drive the front-end into the name screen and dump the
 * composed frame as PPM (a one-off diagnostic for the DARWIN-blit bug;
 * not wired into ctest). */
#include "font.h"
#include "frontend.h"
#include "osystem.h"
#include "palette.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>

extern const OSystem g_osys_null;

static void _dump_ppm(const char *path, const uint8_t *page,
                      const uint8_t *pal6)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "_dump_ppm: cannot open %s\n", path);
        return;
    }
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

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *out_dir = argc > 2 ? argv[2] : "/tmp/opencode";
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume at %s\n", vol_dir);
        return 0;
    }
    fclose(f);

    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    font_init(&g_osys_null);
    frontend_init(&g_osys_null);

    InputState in = { 0, 0, 0 };
    uint32_t t;
    /* load + fade-in (~35 ticks, stepped by palette_tick) */
    for (t = 0; t < 2000; t++) {
        palette_tick();
        frontend_tick(&in);
        if (t > 60) break;
    }
    /* hold LEFT until the pan ends and the name screen settles */
    in.joyBits = 0x04;
    uint32_t zeros = 0;
    for (t = 0; t < 4000; t++) {
        palette_tick();
        int32_t r = frontend_tick(&in);
        if (!r) zeros++;
        if (t > 300)
            in.joyBits = 0x00;      /* stop walking: the pan may still run */
        if (t % 500 == 0) {
            char ppath[512];
            snprintf(ppath, sizeof ppath, "%s/probe_%04u.ppm", out_dir, t);
            _dump_ppm(ppath, video_page(0), palette_working());
        }
        if (t > 1500) break;
    }
    const uint8_t *pal = palette_working();
    const uint8_t *pg = video_page(0);
    uint32_t nz = 0;
    for (uint32_t i = 0; i < 320u * 200u; i++)
        if (pg[i]) nz++;
    fprintf(
        stderr,
        "zeros=%u fadeActive=%d pageNZ=%u pal[0x33]=%02x %02x %02x pal[0xB3]=%02x %02x %02x\n",
        zeros, palette_fade_active(), nz, pal[0x33 * 3], pal[0x33 * 3 + 1],
        pal[0x33 * 3 + 2], pal[0xB3 * 3], pal[0xB3 * 3 + 1], pal[0xB3 * 3 + 2]);
    in.joyBits = 0;
    for (t = 0; t < 30; t++) {
        palette_tick();
        frontend_tick(&in);
    }

    snprintf(path, sizeof path, "%s/frame_name.ppm", out_dir);
    _dump_ppm(path, video_page(0), pal);
    printf("dumped\n");
    return 0;
}
