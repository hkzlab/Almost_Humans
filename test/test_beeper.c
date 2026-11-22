/* test/test_beeper.c - the Beeper sound-driver module against a real .BEP.
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
#include "audio/beeper/beeper_internal.h"
#include "audio/beeper/beeper_speaker.c"
#include "audio/beeper/beeper_seq.c"
#include "audio/beeper/sound_driver_beeper.c"

#include "audio.h"
#include "audio/sound_driver.h"
#include "byteorder.h"
#include "compat.h"
#include "fs/posix/fs_posix.h"
#include "arch_mem.h"
#include "osystem.h"
#include "res.h"
#include "test_support.h"

#include <stdint.h>
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

/**
 * @brief Run the Beeper driver checks.
 */
static void test_beeper(void)
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

    if (_test_argc < 2) TEST_FAILF("usage: %s <resources-dir>", _test_argv[0]);
    snprintf(path, sizeof(path), "%s/HUMANS.000/MUS/TITLE.BEP", _test_argv[1]);
    image = _read_file(path, &size);
    if (!image) TEST_SKIPF("%s missing, skipping", path);

    drv = g_sound_driver_beeper.create(RATE);
    TEST_ASSERT_TRUE(drv != NULL);
    TEST_ASSERT_TRUE(drv->ready == 1);

    /* A command before the tune is loaded must be refused, not crash. */
    TEST_ASSERT_TRUE(g_sound_driver_beeper.startEvent(drv, 0) == 0xFFFF);

    g_sound_driver_beeper.setTempo(drv, 0x4A); // boot fade's last byte
    TEST_ASSERT_TRUE(drv->tempo == (uint8_t)((0xA3u * 0x4Au) >> 8));
    g_sound_driver_beeper.loadTune(drv, image, size);

    /* K, P, Stream, N, SDS. */
    for (i = 0; i < 0x7Fu; i++)
        if (image[0x18u + i] > max_order) max_order = image[0x18u + i];
    patterns = max_order + 1u;
    records = image[0x11Bu];
    stream = 0x158u + records * 16u;
    table = ((patterns * 2u) & 0xFFF0u) + 0x10u;
    sds = stream + table;

    TEST_ASSERT_TRUE(drv->tune_size == size);
    TEST_ASSERT_TRUE(memcmp(drv->header, image, 0x158u) == 0);
    TEST_ASSERT_TRUE(drv->offset_count == table / 2u);
    TEST_ASSERT_TRUE(drv->song_data == sds);
    TEST_ASSERT_TRUE(drv->offset[0] == rd_le_u16(image + stream));

    /* cmd 5: the record fields and the two claimed speaker slots. */
    handle = g_sound_driver_beeper.startEvent(drv, 0);
    TEST_ASSERT_TRUE(handle == 0);
    TEST_ASSERT_TRUE(g_sound_driver_beeper.startEvent(drv, image[0x11Bu]) ==
                     0xFFFF);
    TEST_ASSERT_TRUE(drv->record[0].active == 0xFFu);
    TEST_ASSERT_TRUE(drv->record[0].tracks == image[0x158u + 0x0Du]);
    TEST_ASSERT_TRUE(drv->record[0].tracks == 2u);
    TEST_ASSERT_TRUE(drv->record[0].ticks_per_row == 6u);
    TEST_ASSERT_TRUE(drv->record[0].order == image[0x158u + 0x0Au]);
    TEST_ASSERT_TRUE(drv->record[0].loop == image[0x158u + 0x0Cu]);
    TEST_ASSERT_TRUE(drv->record[0].last_order ==
                     (uint8_t)(image[0x158u + 0x0Bu] + 1u));
    TEST_ASSERT_TRUE(drv->slot_free[0] == 0xFFu && drv->slot_free[1] == 0xFFu);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_LAST_INSTRUMENT] == 0xFFu);
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_LAST_INSTRUMENT] == 0xFFu);

    /* Six ticks = one row (ticks per row 6): the row decodes and plays. */
    for (i = 0; i < 6u; i++) g_sound_driver_beeper.tick(drv);

    pattern = drv->header[0x18u + drv->record[0].order];
    base = drv->song_data +
           ((uint32_t)drv->offset[pattern] - drv->offset[0]) * 16u;
    note = image[base + 0u];
    instrument = image[base + 1u];
    command = image[base + 2u];
    param = image[base + 3u];
    /* TITLE.BEP's first cell is note 36, instrument 3 */
    TEST_ASSERT_TRUE(note != 0);

    TEST_ASSERT_TRUE(drv->slot[0][BEEP_NOTE] == note);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_INSTRUMENT] == instrument);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_COMMAND] == command);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_PARAM] == param);
    /* Cell 1 is zero in this first pattern; the decode copies it to slot 1
     * regardless (the stride is 0x16: 4-byte cell + 0x12 gap). */
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_NOTE] == image[base + 4u]);
    TEST_ASSERT_TRUE(drv->slot_flag[0] == 0xFEu); // armed then switched on
    TEST_ASSERT_TRUE(drv->speaker_on == 1);
    TEST_ASSERT_TRUE(drv->divider == _period[note]);

    /* The gated square's pitch is the PIT's 1193182 / divisor. */
    pcm = calloc((size_t)RATE * 2u, sizeof(*pcm));
    TEST_ASSERT_TRUE(pcm != NULL);
    g_sound_driver_beeper.generate(drv, pcm, RATE);
    measured = _frequency(pcm, RATE);
    expected = (double)BEEP_PIT_HZ / (double)drv->divider;
    TEST_ASSERT_TRUE(measured > expected * 0.99 && measured < expected * 1.01);

    /* Half a row later slot 1 is switched in; it is unarmed in this pattern
     * (cell 1 is zero), so the speaker goes off.  The impulse model's
     * accumulator decays, so let it settle before checking for silence. */
    for (i = 0; i < 3u; i++) g_sound_driver_beeper.tick(drv);
    TEST_ASSERT_TRUE(drv->speaker_on == 0);
    g_sound_driver_beeper.generate(drv, pcm, RATE / 2u);
    memset(pcm, 0x55, (size_t)RATE * 2u * sizeof(*pcm));
    g_sound_driver_beeper.generate(drv, pcm, RATE);
    for (i = 0; i < RATE * 2u; i++) TEST_ASSERT_TRUE(abs(pcm[i]) <= 4);

    /* Reach order 4 (TITLE's pattern 2), whose row 0 has a non-zero second
     * cell, and prove it reaches slot 1 with its own divider. */
    for (i = 0; i < 2000u && drv->record[0].order != 4u; i++)
        g_sound_driver_beeper.tick(drv);
    TEST_ASSERT_TRUE(drv->record[0].order == 4u);
    for (i = 0; i < 6u; i++) g_sound_driver_beeper.tick(drv);
    pattern = drv->header[0x18u + 4u];
    base = drv->song_data +
           ((uint32_t)drv->offset[pattern] - drv->offset[0]) * 16u;
    TEST_ASSERT_TRUE(image[base + 4u] != 0); // the second cell carries a note
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_NOTE] == image[base]);
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_NOTE] == image[base + 4u]);
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_INSTRUMENT] == image[base + 5u]);
    TEST_ASSERT_TRUE(rd_le_u16(&drv->slot[1][BEEP_DIVIDER]) ==
                     _period[image[base + 4u]]);

    /* Live snapshot (BEEPER.DRV, 2026-11-07): the driver's state at
     * record 0, order 3, row 0x20, tick 3 on the second pass.  This pins the
     * decoder's 0x16 stride and the second voice against the real driver. */
    for (i = 0; i < 20000u; i++) {
        if (drv->record[0].finished == 0xFFu && drv->record[0].order == 3u &&
            drv->record[0].row == 0x20u && drv->record[0].tick == 3u)
            break;
        g_sound_driver_beeper.tick(drv);
    }
    TEST_ASSERT_TRUE(drv->record[0].finished == 0xFFu);
    TEST_ASSERT_TRUE(drv->record[0].order == 3u &&
                     drv->record[0].row == 0x20u && drv->record[0].tick == 3u);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_NOTE] == 0x00u);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_INSTRUMENT] == 0x00u);
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_NOTE] == 0x35u);
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_INSTRUMENT] == 0x05u);
    TEST_ASSERT_TRUE(rd_le_u16(&drv->slot[0][BEEP_DIVIDER]) == 0x1530u);
    TEST_ASSERT_TRUE(rd_le_u16(&drv->slot[1][BEEP_DIVIDER]) == 0x07EFu);
    TEST_ASSERT_TRUE(drv->slot_flag[0] == 0x00u);
    TEST_ASSERT_TRUE(drv->slot_flag[1] == 0xFEu);
    TEST_ASSERT_TRUE(drv->slot[0][BEEP_LAST_INSTRUMENT] == 0x03u);
    TEST_ASSERT_TRUE(drv->slot[1][BEEP_LAST_INSTRUMENT] == 0x05u);
    TEST_ASSERT_TRUE(drv->record[0].active == 0xFFu);
    TEST_ASSERT_TRUE(drv->record[0].tracks == 2u);
    TEST_ASSERT_TRUE(drv->record[0].ticks_per_row == 6u);
    TEST_ASSERT_TRUE(drv->record[0].cursor == 0x0298u);
    TEST_ASSERT_TRUE(drv->record[0].loop == 1u);
    TEST_ASSERT_TRUE(drv->record[0].first_order == 0u);
    TEST_ASSERT_TRUE(drv->record[0].last_order == 0x0Eu);
    TEST_ASSERT_TRUE(drv->record[0].track_slot[0] == 0u);
    TEST_ASSERT_TRUE(drv->record[0].track_slot[1] == 1u);

    /* cmd 6 frees the record and restores the two speaker slots. */
    g_sound_driver_beeper.releaseEvent(drv, handle);
    TEST_ASSERT_TRUE(drv->record[0].active == 0);
    TEST_ASSERT_TRUE(drv->speaker_on == 0);
    TEST_ASSERT_TRUE(drv->slot_free[0] == 0 && drv->slot_free[1] == 1);

    /* The core's play_tune must choose .BEP from [0x5AFE] = 0 (1000:8462).
     * That path reads the packed volumes, so it needs the volume dir. */
    if (_test_argc > 2) {
        static const OSystem osys = { .audioLoadTune = _core_load,
                                      .audioResume = _core_resume,
                                      .audioStop = _core_stop,
                                      .audioTempo = _core_tempo,
                                      .audioDriverId = _core_id,
                                      .warn = _core_warn,
                                      .fs = &g_fs_posix,
                                      .memAlloc = arch_mem_alloc,
                                      .memFree = arch_mem_free };
        static const char missing[] = "mus\\nope";
        uint32_t stops0;

        TEST_ASSERT_TRUE(compat_chdir(_test_argv[2]) == 0);
        osys_bind(&osys);
        TEST_ASSERT_TRUE(audio_init() == 0);
        res_init();
        audio_play_tune(audio_tune_name(AUDIO_TUNE_TITLE));
        TEST_ASSERT_TRUE(_core_loaded_size == size);
        audio_music_resume();
        TEST_ASSERT_TRUE(_core_resumes == 1);

        /* D-252: a missing .BEP stops the current record, and a later
         * music_resume must not restart the old driver image. */
        stops0 = _core_stops;
        audio_play_tune(missing);
        TEST_ASSERT_TRUE(_core_stops == stops0 + 1);
        TEST_ASSERT_TRUE(_core_warns == 1); // the missing tune is reported once
        audio_music_resume();
        TEST_ASSERT_TRUE(_core_resumes == 1);

        /* A successful load clears the silence and lets the music resume. */
        audio_play_tune(audio_tune_name(AUDIO_TUNE_TITLE));
        TEST_ASSERT_TRUE(_core_loaded_size == size);
        audio_music_resume();
        TEST_ASSERT_TRUE(_core_resumes == 2);

        /* D-279: the Beeper level tunes are digit-named (LEV1..LEV9). */
        {
            uint32_t warns0 = _core_warns;

            res_set_disc(1u); // the level tunes ship on HUMANS.001
            audio_play_level_tune(1u);
            TEST_ASSERT_TRUE(
                strcmp(audio_tune_name(AUDIO_TUNE_LEV), "mus\\lev1") == 0);
            TEST_ASSERT_TRUE(_core_loaded_size == 8136u); // LEV1.BEP
            audio_play_tune(audio_tune_name(AUDIO_TUNE_PARTY));
            audio_play_level_tune(6u);
            TEST_ASSERT_TRUE(
                strcmp(audio_tune_name(AUDIO_TUNE_LEV), "mus\\lev6") == 0);
            TEST_ASSERT_TRUE(_core_loaded_size == 5528u); // LEV6.BEP
            audio_play_tune(audio_tune_name(AUDIO_TUNE_HUMFX));
            audio_play_level_tune(10u);
            TEST_ASSERT_TRUE(
                strcmp(audio_tune_name(AUDIO_TUNE_LEV), "mus\\lev1") == 0);
            TEST_ASSERT_TRUE(_core_loaded_size == 8136u); // LEV1.BEP
            /* The cutscene keeps its own static 0x62E3 name. */
            TEST_ASSERT_TRUE(strcmp(audio_tune_name(AUDIO_TUNE_LEV_SCENE),
                                    "mus\\levA") == 0);
            TEST_ASSERT_TRUE(_core_warns ==
                             warns0); // every mapped file shipped
        }
    }

    free(pcm);
    g_sound_driver_beeper.destroy(drv);
    free(image);
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_beeper);
    return UNITY_END();
}
