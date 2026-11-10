/* tests/test_beeper.c - the Beeper sound-driver module against a real .BEP.
 *
 * White-box (the module is included) so the checks can pin what the INT 60h
 * ABI alone cannot see: the loader geometry, the row decoder's slot cells,
 * the record fields cmd 5 sets, and the PIT divider the note path arms.  The
 * rendered impulse-model output is then measured for pitch.
 *
 * The assertions this test exists for:
 *   - the .BEP header/offset-table geometry;
 *   - the row decoder's stride: the 4-byte cell plus the 0x12 gap is 0x16,
 *     so track 1 reaches slot 1 and both voices sound;
 *   - the renderer's frequency is the PIT's 1193182 / divisor;
 *   - a live state snapshot at order 3 / row 0x20 / tick 3;
 *   - a missing tune stops the record and blocks a later resume;
 *   - releasing the record frees its slots and silences the speaker.
 *
 * Usage: test_beeper <extracted-resources-dir>  (the dir holding
 * HUMANS.000/MUS/TITLE.BEP)
 */
#include "audio/beeper/sound_driver_beeper.c"

#include "audio.h"
#include "compat.h"
#include "osystem.h"
#include "res.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000u

static uint32_t _core_loaded_size;
static uint32_t _core_resumes;
static uint32_t _core_stops;
static uint32_t _core_warns;
static int32_t _core_driver_id = 0; // Beeper

/**
 * @brief The core test's cmd 0x0C: record the loaded image's size.
 *
 * @param image the tune image
 * @param size its byte length
 * @return 0, always
 */
static int32_t _core_load(const uint8_t *image, uint32_t size)
{
    (void)image;
    _core_loaded_size = size;
    return 0;
}

/**
 * @brief The core test's cmd 5, CX = 0 (music_resume).
 */
static void _core_resume(void) { _core_resumes++; }

/**
 * @brief The core test's cmd 6 (music_stop).
 */
static void _core_stop(void) { _core_stops++; }

/**
 * @brief The core test's cmd 0x0A: accepted and ignored.
 *
 * @param cl the master-volume byte
 */
static void _core_tempo(uint8_t cl) { (void)cl; }

/**
 * @brief The core test's [0x5AFE].
 *
 * @return the driver id (0 = Beeper)
 */
static int32_t _core_id(void) { return _core_driver_id; }

/**
 * @brief The core test's non-fatal diagnostic: count it.
 *
 * @param msg the message (ignored)
 */
static void _core_warn(const char *msg)
{
    (void)msg;
    _core_warns++;
}

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
 * @brief Measure the fundamental from the settled square's sign changes.
 *
 * The impulse model's sinc edges ring for a few hundred samples, so the
 * first 50 ms are skipped.
 *
 * @param pcm interleaved stereo samples
 * @param frames the frame count
 * @return the measured frequency in Hz
 */
static double _frequency(const int16_t *pcm, uint32_t frames)
{
    const uint32_t skip = 2400u;
    uint32_t i;
    uint32_t crossings = 0;

    for (i = skip + 1u; i < frames; i++)
        if ((pcm[2u * (i - 1u)] < 0) != (pcm[2u * i] < 0)) crossings++;
    return (double)crossings * (double)RATE /
           (2.0 * (double)(frames - skip - 1u));
}

int main(int argc, char **argv)
{
    char path[1024];
    uint32_t size = 0;
    uint8_t *image;
    SoundDriver *drv;
    uint16_t handle;
    unsigned i;
    unsigned records;
    unsigned patterns;
    unsigned max_order = 0;
    uint32_t stream;
    uint32_t table;
    uint32_t sds;
    unsigned pattern;
    uint32_t base;
    uint8_t note;
    uint8_t instrument;
    uint8_t command;
    uint8_t param;
    int16_t *pcm;
    double measured;
    double expected;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <resources-dir>\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/HUMANS.000/MUS/TITLE.BEP", argv[1]);
    image = _read_file(path, &size);
    if (!image) {
        fprintf(stderr, "test_beeper: %s missing, skipping\n", path);
        return 0;
    }

    drv = g_sound_driver_beeper.create(RATE);
    assert(drv != NULL);
    assert(drv->ready == 1);

    /* A command before the tune is loaded must be refused, not crash. */
    assert(g_sound_driver_beeper.startEvent(drv, 0) == 0xFFFF);

    g_sound_driver_beeper.setTempo(drv, 0x4A); // boot fade's last byte
    assert(drv->tempo == (uint8_t)((0xA3u * 0x4Au) >> 8));
    g_sound_driver_beeper.loadTune(drv, image, size);

    /* K, P, Stream, N, SDS. */
    for (i = 0; i < 0x7Fu; i++)
        if (image[0x18u + i] > max_order) max_order = image[0x18u + i];
    patterns = max_order + 1u;
    records = image[0x11Bu];
    stream = 0x158u + records * 16u;
    table = ((patterns * 2u) & 0xFFF0u) + 0x10u;
    sds = stream + table;

    assert(drv->tune_size == size);
    assert(memcmp(drv->header, image, 0x158u) == 0);
    assert(drv->offset_count == table / 2u);
    assert(drv->song_data == sds);
    assert(drv->offset[0] == rd_le_u16(image + stream));

    /* cmd 5: the record fields and the two claimed speaker slots. */
    handle = g_sound_driver_beeper.startEvent(drv, 0);
    assert(handle == 0);
    assert(g_sound_driver_beeper.startEvent(drv, image[0x11Bu]) == 0xFFFF);
    assert(drv->record[0].active == 0xFFu);
    assert(drv->record[0].tracks == image[0x158u + 0x0Du]);
    assert(drv->record[0].tracks == 2u);
    assert(drv->record[0].ticks_per_row == 6u);
    assert(drv->record[0].order == image[0x158u + 0x0Au]);
    assert(drv->record[0].loop == image[0x158u + 0x0Cu]);
    assert(drv->record[0].last_order == (uint8_t)(image[0x158u + 0x0Bu] + 1u));
    assert(drv->slot_free[0] == 0xFFu && drv->slot_free[1] == 0xFFu);
    assert(drv->slot[0][BEEP_LAST_INSTRUMENT] == 0xFFu);
    assert(drv->slot[1][BEEP_LAST_INSTRUMENT] == 0xFFu);

    /* Six ticks = one row (ticks per row 6): the row decodes and plays. */
    for (i = 0; i < 6u; i++) g_sound_driver_beeper.tick(drv);

    pattern = drv->header[0x18u + drv->record[0].order];
    base = drv->song_data +
           ((uint32_t)drv->offset[pattern] - drv->offset[0]) * 16u;
    note = image[base + 0u];
    instrument = image[base + 1u];
    command = image[base + 2u];
    param = image[base + 3u];
    assert(note != 0); // TITLE.BEP's first cell is note 36, instrument 3

    assert(drv->slot[0][BEEP_NOTE] == note);
    assert(drv->slot[0][BEEP_INSTRUMENT] == instrument);
    assert(drv->slot[0][BEEP_COMMAND] == command);
    assert(drv->slot[0][BEEP_PARAM] == param);
    /* Cell 1 is zero in this first pattern; the decode copies it to slot 1
     * regardless (the stride is 0x16: 4-byte cell + 0x12 gap). */
    assert(drv->slot[1][BEEP_NOTE] == image[base + 4u]);
    assert(drv->slot_flag[0] == 0xFEu); // armed then switched on
    assert(drv->speaker_on == 1);
    assert(drv->divider == _period[note]);

    /* The gated square's pitch is the PIT's 1193182 / divisor. */
    pcm = calloc((size_t)RATE * 2u, sizeof(*pcm));
    assert(pcm != NULL);
    g_sound_driver_beeper.generate(drv, pcm, RATE);
    measured = _frequency(pcm, RATE);
    expected = (double)BEEP_PIT_HZ / (double)drv->divider;
    assert(measured > expected * 0.99 && measured < expected * 1.01);

    /* Half a row later slot 1 is switched in; it is unarmed in this pattern
     * (cell 1 is zero), so the speaker goes off.  The impulse model's
     * accumulator decays, so let it settle before checking for silence. */
    for (i = 0; i < 3u; i++) g_sound_driver_beeper.tick(drv);
    assert(drv->speaker_on == 0);
    g_sound_driver_beeper.generate(drv, pcm, RATE / 2u);
    memset(pcm, 0x55, (size_t)RATE * 2u * sizeof(*pcm));
    g_sound_driver_beeper.generate(drv, pcm, RATE);
    for (i = 0; i < RATE * 2u; i++) assert(abs(pcm[i]) <= 4);

    /* Reach order 4 (TITLE's pattern 2), whose row 0 has a non-zero second
     * cell, and prove it reaches slot 1 with its own divider. */
    for (i = 0; i < 2000u && drv->record[0].order != 4u; i++)
        g_sound_driver_beeper.tick(drv);
    assert(drv->record[0].order == 4u);
    for (i = 0; i < 6u; i++) g_sound_driver_beeper.tick(drv);
    pattern = drv->header[0x18u + 4u];
    base = drv->song_data +
           ((uint32_t)drv->offset[pattern] - drv->offset[0]) * 16u;
    assert(image[base + 4u] != 0); // the second cell carries a note
    assert(drv->slot[0][BEEP_NOTE] == image[base]);
    assert(drv->slot[1][BEEP_NOTE] == image[base + 4u]);
    assert(drv->slot[1][BEEP_INSTRUMENT] == image[base + 5u]);
    assert(rd_le_u16(&drv->slot[1][BEEP_DIVIDER]) == _period[image[base + 4u]]);

    /* Live snapshot (BEEPER.DRV, 2026-11-07): the driver's state at
     * record 0, order 3, row 0x20, tick 3 on the second pass.  This pins the
     * decoder's 0x16 stride and the second voice against the real driver. */
    for (i = 0; i < 20000u; i++) {
        if (drv->record[0].finished == 0xFFu && drv->record[0].order == 3u &&
            drv->record[0].row == 0x20u && drv->record[0].tick == 3u)
            break;
        g_sound_driver_beeper.tick(drv);
    }
    assert(drv->record[0].finished == 0xFFu);
    assert(drv->record[0].order == 3u && drv->record[0].row == 0x20u &&
           drv->record[0].tick == 3u);
    assert(drv->slot[0][BEEP_NOTE] == 0x00u);
    assert(drv->slot[0][BEEP_INSTRUMENT] == 0x00u);
    assert(drv->slot[1][BEEP_NOTE] == 0x35u);
    assert(drv->slot[1][BEEP_INSTRUMENT] == 0x05u);
    assert(rd_le_u16(&drv->slot[0][BEEP_DIVIDER]) == 0x1530u);
    assert(rd_le_u16(&drv->slot[1][BEEP_DIVIDER]) == 0x07EFu);
    assert(drv->slot_flag[0] == 0x00u);
    assert(drv->slot_flag[1] == 0xFEu);
    assert(drv->slot[0][BEEP_LAST_INSTRUMENT] == 0x03u);
    assert(drv->slot[1][BEEP_LAST_INSTRUMENT] == 0x05u);
    assert(drv->record[0].active == 0xFFu);
    assert(drv->record[0].tracks == 2u);
    assert(drv->record[0].ticks_per_row == 6u);
    assert(drv->record[0].cursor == 0x0298u);
    assert(drv->record[0].loop == 1u);
    assert(drv->record[0].first_order == 0u);
    assert(drv->record[0].last_order == 0x0Eu);
    assert(drv->record[0].track_slot[0] == 0u);
    assert(drv->record[0].track_slot[1] == 1u);

    /* cmd 6 frees the record and restores the two speaker slots. */
    g_sound_driver_beeper.releaseEvent(drv, handle);
    assert(drv->record[0].active == 0);
    assert(drv->speaker_on == 0);
    assert(drv->slot_free[0] == 0 && drv->slot_free[1] == 1);

    /* The core's play_tune must choose .BEP from [0x5AFE] = 0 (1000:8462).
     * That path reads the packed volumes, so it needs the volume dir. */
    if (argc > 2) {
        static const OSystem osys = { .audioLoadTune = _core_load,
                                      .audioResume = _core_resume,
                                      .audioStop = _core_stop,
                                      .audioTempo = _core_tempo,
                                      .audioDriverId = _core_id,
                                      .warn = _core_warn,
                                      .fs = &g_fs_posix };
        static const char missing[] = "mus\\nope";
        uint32_t stops0;

        assert(compat_chdir(argv[2]) == 0);
        assert(audio_init(&osys) == 0);
        res_init(&osys);
        audio_play_tune(audio_tune_title);
        assert(_core_loaded_size == size);
        audio_music_resume();
        assert(_core_resumes == 1);

        /* D-252: a missing .BEP stops the current record, and a later
         * music_resume must not restart the old driver image. */
        stops0 = _core_stops;
        audio_play_tune(missing);
        assert(_core_stops == stops0 + 1);
        assert(_core_warns == 1); // the missing tune is reported once
        audio_music_resume();
        assert(_core_resumes == 1);

        /* A successful load clears the silence and lets the music resume. */
        audio_play_tune(audio_tune_title);
        assert(_core_loaded_size == size);
        audio_music_resume();
        assert(_core_resumes == 2);
    }

    free(pcm);
    g_sound_driver_beeper.destroy(drv);
    free(image);
    printf("test_beeper: ok\n");
    return 0;
}
