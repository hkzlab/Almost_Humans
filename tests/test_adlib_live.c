/* tests/test_adlib_live.c — the module's state against the live ADLIB.DRV.
 *
 * White-box: includes the driver module so it can read the private shadow and
 * voice records (the ABI exposes only the sound).  The expected values are a
 * snapshot of the *original* driver in DOSBox while it played TITLE.ALB:
 * record 0 at order 6, row 1, tick 4 — 2314 driver ticks after the tune
 * started, master volume at the post-fade 0x2F.
 *
 * This pins the exact state the original reached: the voice cells (row
 * decode), the volume cache (the cmd 0x0A decay on voices 2..4), the F-number
 * registers and the instrument registers.  A regression in any of them shows
 * here, not as "a note sounds wrong".
 *
 * Usage: test_adlib_live <path to TITLE.ALB>
 */
#include "audio/adlib/sound_driver_adlib.c"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Energy at one frequency, as a fraction of the block's total energy.
 *
 * A DFT bin at @p freq over @p n samples: 10 Hz wide at n = 4800, which is
 * enough to tell the driver's selected pitch from a core clocked off its
 * native 49716 Hz (a 3.5 % error moves the peak 1.7 bins away).
 */
static double _tone_share(const int16_t *x, int n, double freq)
{
    const double pi = 3.14159265358979323846;
    double w = 2.0 * pi * freq / 48000.0, cw = cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, total = 0.0, mag;
    int i;

    for (i = 0; i < n; i++) {
        s0 = 2.0 * cw * s1 - s2 + (double)x[2 * i];
        s2 = s1;
        s1 = s0;
        total += (double)x[2 * i] * (double)x[2 * i];
    }
    mag = s1 * s1 + s2 * s2 - 2.0 * cw * s1 * s2;
    return total > 0.0 ? mag / ((double)n * total) : 0.0;
}

/** @brief The frequency the shadow selects for a melodic voice. */
static double _voice_hz(const SoundDriver *drv, unsigned voice)
{
    unsigned fnum = (unsigned)drv->shadow[0xA0u + voice] |
                    (((unsigned)drv->shadow[0xB0u + voice] & 3u) << 8);
    unsigned block = ((unsigned)drv->shadow[0xB0u + voice] >> 2) & 7u;

    return (double)fnum * 49716.0 / (double)(1u << (20u - block));
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint8_t *img;
    SoundDriver *drv;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <TITLE.ALB>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        printf("skip: %s missing\n", argv[1]);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc((size_t)n);
    assert(img && fread(img, 1, (size_t)n, f) == (size_t)n);
    fclose(f);

    drv = _adlib_create(48000);
    assert(drv != NULL);
    _adlib_set_tempo(drv, 0x4A); /* the boot fade's last step -> 0x2F */
    _adlib_load_tune(drv, img, (uint32_t)n);
    assert(_adlib_start_event(drv, 0) == 0);

    /* the render path, not just the registers: after order 0 row 33 the chip
     * must sound each voice at the frequency the shadow selects.  A core
     * clocked off its native 49716 Hz played everything 3.5 % flat and these
     * shares collapsed (measured here: 0.14 at voice 0, 0.08 at voice 1). */
    {
        static int16_t pcm[9600];
        double v0, v1;
        int k;

        for (i = 0; i < 198; i++) _adlib_tick(drv);
        for (k = 0; k < 4800; k++) g_sound_driver.generate(drv, pcm + 2 * k, 1);
        assert(drv->shadow[0xB0] & 0x20u); /* voice 0 keyed */
        v0 = _tone_share(pcm, 4800, _voice_hz(drv, 0));
        v1 = _tone_share(pcm, 4800, _voice_hz(drv, 1));
        assert(v0 > 0.03);
        assert(v1 > 0.02);
    }

    for (; i < 1506; i++) _adlib_tick(drv);

    /* the rhythm voices: the row mask's bits 9..5 (1E95:06ED), live-checked at
     * order 3 row 59.  The former bits-3..0 decode left voice 6 at zero here
     * and dropped every percussion cell in the tune. */
    assert(drv->slot[6].note == 57u && drv->slot[6].instrument == 17u);
    assert(drv->shadow[0xBD] == 0x30u); /* rhythm mode + BD keyed */
    assert(drv->shadow[0xA6] == 0x43u && drv->shadow[0xB6] == 0x0Eu);

    for (; i < 2314; i++) _adlib_tick(drv);

    /* the record: order 6, row 1, tick 4 (the live snapshot) */
    assert(drv->record[0].active == 0xFFu);
    assert(drv->record[0].order == 6u);
    assert(drv->record[0].row == 1u);
    assert(drv->record[0].tick == 4u);
    assert(drv->tempo == 0x2Fu);

    /* the voice cells and their note-on state */
    assert(drv->slot[0].note == 31u && drv->slot[0].instrument == 3u);
    assert(drv->slot[0].last_instrument == 3u);
    assert(drv->slot[0].note_entry == 0x1203u);
    assert(drv->slot[0].volume == 0x2Fu);
    assert(drv->slot[1].note == 91u && drv->slot[1].instrument == 7u);
    assert(drv->slot[1].note_entry == 0x6203u);
    assert(drv->slot[2].note == 79u && drv->slot[2].command == 0x0Au);
    assert(drv->slot[2].note_entry == 0x5203u && drv->slot[2].octave == 5u);
    assert(drv->slot[3].note == 83u && drv->slot[3].note_entry == 0x528Au);
    assert(drv->slot[4].note == 86u && drv->slot[4].note_entry == 0x6181u);
    assert(drv->slot[5].volume == 0x10u && drv->slot[5].last_instrument == 12u);

    /* the cmd 0x0A decay: voices 2..4 reached 0x0F in the live driver */
    assert(drv->voice_volume[0] == 0x2Fu);
    assert(drv->voice_volume[1] == 0x2Fu);
    assert(drv->voice_volume[2] == 0x0Fu);
    assert(drv->voice_volume[3] == 0x0Fu);
    assert(drv->voice_volume[4] == 0x0Fu);
    assert(drv->voice_volume[5] == 0x10u);

    /* the F-number/block registers (0xA0/0xB0) */
    {
        static const uint8_t a[6] = { 0x03, 0x03, 0x03, 0x8A, 0x81, 0x43 };
        static const uint8_t b[6] = { 0x26, 0x3A, 0x36, 0x36, 0x39, 0x2E };
        for (i = 0; i < 6; i++) {
            assert(drv->shadow[0xA0 + i] == a[i]);
            assert(drv->shadow[0xB0 + i] == b[i]);
        }
    }

    /* the instrument registers (voice 0's two operators) and the chip mode */
    assert(drv->shadow[0x20] == 0x62u && drv->shadow[0x23] == 0xC2u);
    assert(drv->shadow[0x40] == 0x8Du && drv->shadow[0x43] == 0x10u);
    assert(drv->shadow[0x60] == 0x65u && drv->shadow[0x63] == 0xF5u);
    assert(drv->shadow[0x80] == 0xF2u && drv->shadow[0x83] == 0xFFu);
    assert(drv->shadow[0xC0] == 0x0Au);
    assert(drv->shadow[0xBD] == 0x30u); /* rhythm mode + the bass drum keyed
                                         * (live at this tick; the old decode
                                         * left it at 0x20, never keying it) */

    /* Every tick-path command in the driver's table (CS:0x0F4C).  Commands 1
     * and 2 slide the F-number by 2*parameter and wrap an octave; the wrap is
     * 1E95:08E9's SUB/NEG/ADD, i.e. 0x28A + (0x142 - fnum) going down and
     * (fnum - 0x2AE) + 0x157 going up.  Command 3 walks toward the cached
     * entry, 0x0A ramps the volume, and every other index is a RET. */
    {
        AdlibSlot *v = &drv->slot[0];

        memset(v, 0, sizeof(*v));
        v->fnum = 0x140u;
        v->octave = 4u;
        v->command = 2u;
        v->param = 1u;
        _tick_command(drv, 0);
        assert(v->fnum == 0x28Eu); /* 0x28A + (0x142 - 0x13E), octave 4 -> 3 */
        assert(v->octave == 3u);

        v->fnum = 0x2ACu;
        v->octave = 3u;
        v->command = 1u;
        v->param = 1u;
        _tick_command(drv, 0);
        assert(v->fnum == 0x157u); /* (0x2AE - 0x2AE) + 0x157 */
        assert(v->octave == 4u);

        v->fnum = 0x140u;
        v->octave = 4u;
        v->command = 3u;
        v->param = 0u; /* the portamento's speed is latched by note cmd 3 */
        v->slide_step = 1u;
        v->slide_dir = 0u;
        v->slide_target = 0x1000u; /* below the entry: the slide continues */
        _tick_command(drv, 0);
        assert(v->fnum == 0x28Eu && v->octave == 3u);

        v->volume = 0x30u;
        v->command = 0x0Au;
        v->param = 0x18u;
        _tick_command(drv, 0);
        assert(v->volume == 0x31u); /* high nibble adds */
        v->param = 0x02u;
        _tick_command(drv, 0);
        assert(v->volume == 0x2Fu); /* low nibble subtracts */

        v->fnum = 0x1ABu; /* every RET entry leaves the slot alone */
        v->octave = 5u;
        v->volume = 0x20u;
        v->command = 4u;
        v->param = 0x77u;
        _tick_command(drv, 0);
        assert(v->fnum == 0x1ABu && v->octave == 5u && v->volume == 0x20u);
    }

    _adlib_destroy(drv);
    free(img);
    printf("test_adlib_live: ok (matches the live driver at 2314 ticks)\n");
    return 0;
}
