/* arch/audio/adlib/sound_driver_adlib.c — ADLIB.DRV's replacement.
 *
 * Stub: the command surface is in place (audio/sound_driver.h), the sequencer
 * and the OPL3 chip are not.  The register stream to reproduce is the one
 * adlib_write_opl_reg wrote (1E95:0E23, ports 0x388/0x389, OPL2 registers
 * 0x00..0xF5); the chip will be Nuked-OPL3-fast, private to this file.
 *
 * shortcut: no sequencer and no chip yet, upgrade when the tune format and
 * the driver's voice table are ported (next_step.md, D-01).
 */
#include "audio/sound_driver.h"

#include <stdlib.h>
#include <string.h>

struct SoundDriver {
    uint32_t sampleRate;
};

/* cmd 0x0E: the driver's command block — all zero in every observed session. */
static const uint8_t _command_block[8];

/**
 * @brief Install the AdLib driver emulation.
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when out of memory
 */
static SoundDriver *_adlib_create(uint32_t sampleRate)
{
    SoundDriver *drv = malloc(sizeof(*drv));
    if (!drv) return NULL;
    drv->sampleRate = sampleRate;
    return drv;
}

/**
 * @brief cmd 0: uninstall.
 *
 * @param drv the driver; NULL is accepted
 */
static void _adlib_destroy(SoundDriver *drv) { free(drv); }

/**
 * @brief cmd 0x0A: set the tempo.
 *
 * @param drv the driver
 * @param tempo CL, the game's tempo byte
 */
static void _adlib_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    (void)drv;
    (void)tempo;
}

/**
 * @brief cmd 0x0E: identify, the 8-byte command block.
 *
 * @param drv the driver
 * @return the block (ES:DI in the original)
 */
static const uint8_t *_adlib_command_block(SoundDriver *drv)
{
    (void)drv;
    return _command_block;
}

/**
 * @brief cmd 0x0C: load and start a tune image.
 *
 * @param drv the driver
 * @param image the .ALB tune image
 * @param size its byte length (the caller's bound)
 */
static void _adlib_load_tune(SoundDriver *drv, const uint8_t *image,
                             uint32_t size)
{
    (void)drv;
    (void)image;
    (void)size;
}

/**
 * @brief cmd 5: start an event/voice.
 *
 * @param drv the driver
 * @param event CX, the event id
 * @return the voice handle (DI in the original); 0xFFFF = refused
 */
static uint16_t _adlib_start_event(SoundDriver *drv, uint16_t event)
{
    (void)drv;
    (void)event;
    return 0xFFFF;
}

/**
 * @brief cmd 6: release a voice.
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _adlib_release_event(SoundDriver *drv, uint16_t voice)
{
    (void)drv;
    (void)voice;
}

/**
 * @brief cmd 2: step the sequencer one tick.
 *
 * @param drv the driver
 */
static void _adlib_tick(SoundDriver *drv) { (void)drv; }

/**
 * @brief Render interleaved stereo samples.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _adlib_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    (void)drv;
    memset(dst, 0, (size_t)frames * 2 * sizeof(*dst));
}

const SoundDriverModule g_sound_driver = {
    _adlib_create,        _adlib_destroy,   _adlib_set_tempo,
    _adlib_command_block, _adlib_load_tune, _adlib_start_event,
    _adlib_release_event, _adlib_tick,      _adlib_generate
};
