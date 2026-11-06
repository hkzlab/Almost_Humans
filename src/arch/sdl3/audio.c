/* arch/sdl3/audio.c — the SDL3 port's audio backend.
 *
 * The audio module is the port's choice; SDL3 links the AdLib driver
 * emulation (audio/sound_driver.h).  This file owns the SDL audio stream
 * around it.  The stream is not open yet — the tune loader and the sequencer
 * tick are the audio objective (D-01) — so the driver is installed and
 * uninstalled around the core's start/stop and nothing renders.
 */
#include "osystem.h"

#include "audio/sound_driver.h"

#include <SDL3/SDL.h>

#define SDL3_AUDIO_RATE 48000

static SoundDriver *_driver;

/**
 * @brief Install the AdLib driver emulation.
 *
 * @return 0 on success, -1 when the driver cannot be created
 */
int32_t sdl3_audio_start(void)
{
    if (_driver) return 0;
    _driver = g_sound_driver.create(SDL3_AUDIO_RATE);
    if (!_driver) {
        SDL_Log("audio: cannot install the sound driver");
        return -1;
    }
    SDL_Log("audio: AdLib driver emulation ready (%u Hz, no stream yet)",
            SDL3_AUDIO_RATE);
    return 0;
}

/**
 * @brief Start one of the game's tunes.
 *
 * @param id the identifier
 * @return 0 on success
 */
int32_t sdl3_audio_play_tune(uint32_t id)
{
    (void)id; /* the loaded .ALB image reaches cmd 0x0C with the tune loader */
    return 0;
}

/**
 * @brief Uninstall the sound driver.
 */
void sdl3_audio_stop(void)
{
    if (_driver) {
        g_sound_driver.destroy(_driver);
        _driver = NULL;
    }
}
