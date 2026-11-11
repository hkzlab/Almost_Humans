/* The silent driver: logs every INT 60h command, no synth underneath.
 *
 * Port-only; built as `audio_null`, linked into no executable.
 */
#include "audio/null/sound_driver_null.h"
#include "arch_mem.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct SoundDriver {
    uint32_t sampleRate;
};

/* cmd 0x0E: the command block, all zero like the real drivers' idle block. */
static const uint8_t _command_block[8];

/**
 * @brief The install entry.
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when out of memory
 */
static SoundDriver *_null_create(uint32_t sampleRate)
{
    SoundDriver *drv = arch_mem_alloc(sizeof(*drv));
    if (!drv) return NULL;
    drv->sampleRate = sampleRate;
    fprintf(stderr, "audio(null): install rate=%u\n", sampleRate);
    return drv;
}

/**
 * @brief cmd 0: uninstall.
 *
 * @param drv the driver; NULL is accepted
 */
static void _null_destroy(SoundDriver *drv)
{
    if (!drv) return;
    fprintf(stderr, "audio(null): cmd 0x00 uninstall\n");
    arch_mem_free(drv);
}

/**
 * @brief cmd 0x0A: set the tempo.
 *
 * @param drv the driver
 * @param tempo CL, the game's tempo byte
 */
static void _null_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    (void)drv;
    fprintf(stderr, "audio(null): cmd 0x0A tempo=0x%02X\n", tempo);
}

/**
 * @brief cmd 0x0E: identify.
 *
 * @param drv the driver
 * @return the command block
 */
static const uint8_t *_null_command_block(SoundDriver *drv)
{
    (void)drv;
    fprintf(stderr, "audio(null): cmd 0x0E identify\n");
    return _command_block;
}

/**
 * @brief cmd 0x0C: load and start a tune image.
 *
 * @param drv the driver
 * @param image the tune image
 * @param size its byte length
 */
static void _null_load_tune(SoundDriver *drv, const uint8_t *image,
                            uint32_t size)
{
    (void)drv;
    (void)image;
    fprintf(stderr, "audio(null): cmd 0x0C load tune (%u bytes)\n", size);
}

/**
 * @brief cmd 5: start an event/voice.
 *
 * @param drv the driver
 * @param event CX, the event id
 * @return the handle the caller stores as the voice (0xFFFF = refused)
 */
static uint16_t _null_start_event(SoundDriver *drv, uint16_t event)
{
    (void)drv;
    fprintf(stderr, "audio(null): cmd 0x05 event=%u\n", event);
    return 0xFFFF;
}

/**
 * @brief cmd 6: release a voice.
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _null_release_event(SoundDriver *drv, uint16_t voice)
{
    (void)drv;
    fprintf(stderr, "audio(null): cmd 0x06 voice=%u\n", voice);
}

/**
 * @brief cmd 2: step the sequencer one tick.
 *
 * @param drv the driver
 */
static void _null_tick(SoundDriver *drv)
{
    (void)drv;
    fprintf(stderr, "audio(null): cmd 0x02 tick\n");
}

/**
 * @brief Render silence.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _null_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    (void)drv;
    memset(dst, 0, (size_t)frames * 2 * sizeof(*dst));
}

const SoundDriverModule g_sound_driver_null = {
    _null_create,        _null_destroy,   _null_set_tempo,
    _null_command_block, _null_load_tune, _null_start_event,
    _null_release_event, _null_tick,      _null_generate
};
