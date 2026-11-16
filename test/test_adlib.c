/* test/test_adlib.c - the AdLib sound-driver module against a real tune.
 *
 * Drives the module through the INT 60h ABI the game uses (loadTune, cmd 5,
 * cmd 2, cmd 0x0A) and checks what the port can be held to without a
 * reference capture:
 *
 *   - a loaded, started tune reaches the OPL and produces audible samples;
 *   - releasing it lets the voices fall silent;
 *   - out-of-range commands are refused rather than corrupting state.
 *
 * Usage: test_adlib <data-dir>   (the dir holding MUS/TITLE.ALB)
 */
#include "audio/adlib/sound_driver_adlib.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE   48000u
#define FRAMES 1024u

static uint8_t *_read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
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
 * @brief Render some audio and return the largest absolute sample.
 *
 * @param drv the driver
 * @return the peak magnitude
 */
static uint32_t _peak(SoundDriver *drv)
{
    int16_t frames[FRAMES * 2u];
    uint32_t peak = 0;
    uint32_t i;

    g_sound_driver_adlib.generate(drv, frames, FRAMES);
    for (i = 0; i < FRAMES * 2u; i++) {
        int32_t v = frames[i];
        if (v < 0) v = -v;
        if ((uint32_t)v > peak) peak = (uint32_t)v;
    }
    return peak;
}

int main(int argc, char **argv)
{
    char path[1024];
    uint32_t size = 0;
    uint8_t *image;
    SoundDriver *drv;
    uint32_t ticks;
    uint32_t loud = 0;
    uint32_t quiet = 0;
    uint16_t handle;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <data-dir>\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/HUMANS.000/MUS/TITLE.ALB", argv[1]);
    image = _read_file(path, &size);
    if (!image) {
        fprintf(stderr, "test_adlib: %s missing, skipping\n", path);
        return 0;
    }

    drv = g_sound_driver_adlib.create(RATE);
    assert(drv != NULL);

    /* A command before the tune is loaded must be refused, not crash. */
    assert(g_sound_driver_adlib.startEvent(drv, 0) == 0xFFFF);

    /* The game's boot fade-in ends at CL = 0x4A, i.e. master volume 0x2F. */
    g_sound_driver_adlib.setTempo(drv, 0x4A);
    g_sound_driver_adlib.loadTune(drv, image, size);
    handle = g_sound_driver_adlib.startEvent(drv, 0);
    assert(handle != 0xFFFF);
    /* The image has two record slots; the third is out of range. */
    assert(g_sound_driver_adlib.startEvent(drv, 0xFFFF) == 0xFFFF);

    /* Two seconds of the game's tick rate: 1500 ticks is far more than one
     * pattern row (6 ticks), so cells have been decoded and played. */
    for (ticks = 0; ticks < 1500u; ticks++) {
        uint32_t peak;
        g_sound_driver_adlib.tick(drv);
        peak = _peak(drv);
        if (peak > 512u) loud++;
    }
    assert(loud > 0);

    g_sound_driver_adlib.releaseEvent(drv, handle);
    /* Let the release envelopes fall; the OPL should be near silence. */
    for (ticks = 0; ticks < 240u; ticks++) {
        uint32_t peak;
        g_sound_driver_adlib.tick(drv);
        peak = _peak(drv);
        if (peak <= 512u) quiet++;
    }
    assert(quiet > 200u);

    g_sound_driver_adlib.destroy(drv);
    free(image);
    printf("test_adlib: ok\n");
    return 0;
}
