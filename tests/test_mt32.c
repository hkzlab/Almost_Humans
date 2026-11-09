/* tests/test_mt32.c - the MT-32 driver module end to end.
 *
 * White-box (the driver module is included) so the .RLD geometry can be
 * checked: every pattern decodes to 64 rows and
 * ends 0..15 bytes before the next pattern's paragraph - and the whole INT
 * 60h conversation is then rendered through a real mt32emu ROM set.
 *
 * The ROMs are not part of the repository, so the render half skips cleanly
 * when HUMANS_MT32_ROMS is unset:
 *
 *   HUMANS_MT32_ROMS=/path/to/roms ctest -R mt32
 */
#include "audio/mt32/sound_driver_mt32.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *_read(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

/**
 * @brief 64 rows decode inside the pattern's span.
 *
 * @param drv the loaded driver
 * @param order the order-list index
 * @param span the bytes to the next pattern paragraph
 * @return 1 when the 64 rows stay in the span, 0 otherwise
 */
static int _check_pattern(SoundDriver *drv, unsigned order, uint32_t span)
{
    Mt32Record rec;
    unsigned i;

    memset(&rec, 0, sizeof(rec));
    rec.tracks = 8;
    rec.order = (uint8_t)order;
    for (i = 0; i < MT32_SLOTS; i++) rec.track_slot[i] = (uint8_t)i;
    for (i = 0; i < MT32_ROWS; i++) _decode_row(drv, &rec);
    if (rec.row != MT32_ROWS) return 0;
    if (rec.cursor > span) return 0;
    if (rec.cursor + 16u < span) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *fix = argc > 1 ? argv[1] : "";
    const char *roms = getenv("HUMANS_MT32_ROMS");
    char path[1024];
    uint8_t *rld;
    uint32_t size = 0;
    SoundDriver *drv;
    uint16_t voice;
    int32_t peak = 0;
    unsigned i;
    unsigned j;

    snprintf(path, sizeof(path), "%s/HUMANS.000/MUS/TITLE.RLD", fix);
    rld = _read(path, &size);
    if (!rld) {
        printf("SKIP: no %s\n", path);
        return 0;
    }

    if (!roms || !roms[0]) {
        printf("SKIP: HUMANS_MT32_ROMS is not set\n");
        free(rld);
        return 0;
    }

    sound_driver_mt32_set_roms(roms);
    drv = g_sound_driver_mt32.create(48000);
    if (!drv) {
        fprintf(stderr, "FAIL: the MT-32 driver did not install\n");
        free(rld);
        return 1;
    }
    g_sound_driver_mt32.loadTune(drv, rld, size);

    /* Every order TITLE's record 0 plays must decode in its span. */
    if (drv->off_base != MT32_OFF_NEW) {
        fprintf(stderr, "FAIL: TITLE.RLD was read as the old format\n");
        g_sound_driver_mt32.destroy(drv);
        free(rld);
        return 1;
    }
    for (i = 0; i < 15u; i++) {
        uint32_t span;
        unsigned pattern = drv->order_list[i];
        uint32_t base = drv->off_base + (uint32_t)drv->offset[pattern] * 16u;
        uint32_t next = drv->bank_base;
        for (j = 0; j < 12u; j++) {
            uint32_t other = drv->off_base + (uint32_t)drv->offset[j] * 16u;
            if (other > base && other < next) next = other;
        }
        span = next - base;
        if (!_check_pattern(drv, i, span)) {
            fprintf(stderr, "FAIL: order %u does not decode in %u bytes\n", i,
                    span);
            g_sound_driver_mt32.destroy(drv);
            free(rld);
            return 1;
        }
    }

    voice = g_sound_driver_mt32.startEvent(drv, 0);
    if (voice == 0xFFFFu) {
        fprintf(stderr, "FAIL: record 0 was refused\n");
        g_sound_driver_mt32.destroy(drv);
        free(rld);
        return 1;
    }

    /* Drive the 2-of-3 music clock the way the core does, rendering as we
     * go, and look for the tune's output. */
    for (i = 0; i < 400u; i++) {
        int16_t buf[1024 * 2];
        int k;
        g_sound_driver_mt32.tick(drv);
        g_sound_driver_mt32.generate(drv, buf, 1024);
        for (k = 0; k < 1024 * 2; k++) {
            int32_t v = buf[k];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
    }
    g_sound_driver_mt32.releaseEvent(drv, voice);

    /* TITLE sets reverb mode 3 / time 7 / level 7 at order 0 rows 0-1.  The
     * mode must survive from command 7 into command 8's message (the driver
     * keeps it in the same 3-byte block), otherwise the port sends a room
     * reverb (mode 0) where the driver sends a tap delay. */
    if (drv->system_block[0] != 0x03u || drv->system_block[1] != 0x07u ||
        drv->system_block[2] != 0x07u) {
        fprintf(stderr, "FAIL: reverb block is %02x %02x %02x\n",
                drv->system_block[0], drv->system_block[1],
                drv->system_block[2]);
        g_sound_driver_mt32.destroy(drv);
        free(rld);
        return 1;
    }

    /* 0x0363: a second effect while the first is still playing must take the
     * next free slot, not fail because slot 0 is busy.  HUMFX's record 0 is
     * the action-row ding (the game calls driver_call(0) with music off). */
    free(rld);
    rld = NULL;
    snprintf(path, sizeof(path), "%s/HUMANS.001/MUS/HUMFX.RLD", fix);
    rld = _read(path, &size);
    if (!rld) {
        printf("SKIP: no %s (slot-scan check)\n", path);
    } else {
        uint16_t handles[MT32_SLOTS];
        unsigned k;
        g_sound_driver_mt32.loadTune(drv, rld, size);
        free(rld);
        rld = NULL;
        handles[0] = g_sound_driver_mt32.startEvent(drv, 0);
        for (k = 0; k < 3u; k++) g_sound_driver_mt32.tick(drv);
        handles[1] = g_sound_driver_mt32.startEvent(drv, 0);
        if (handles[0] == 0xFFFFu || handles[1] == 0xFFFFu ||
            handles[0] == handles[1]) {
            fprintf(stderr, "FAIL: the second ding was refused\n");
            g_sound_driver_mt32.destroy(drv);
            return 1;
        }
        for (k = 2; k < MT32_SLOTS; k++)
            handles[k] = g_sound_driver_mt32.startEvent(drv, 0);
        if (g_sound_driver_mt32.startEvent(drv, 0) != 0xFFFFu) {
            fprintf(stderr, "FAIL: nine effects were accepted\n");
            g_sound_driver_mt32.destroy(drv);
            return 1;
        }
        for (k = 0; k < MT32_SLOTS; k++) {
            if (handles[k] != 0xFFFFu)
                g_sound_driver_mt32.releaseEvent(drv, handles[k]);
        }
    }

    g_sound_driver_mt32.destroy(drv);
    free(rld);

    if (peak == 0) {
        fprintf(stderr, "FAIL: TITLE.RLD rendered silence\n");
        return 1;
    }
    printf("ok: geometry and TITLE.RLD render (peak %d, %u ticks)\n", (int)peak,
           i);
    return 0;
}
