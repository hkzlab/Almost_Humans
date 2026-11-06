/* arch/sdl3/audio.c — the SDL3 port's audio backend.
 *
 * The backend owns the sample stream; the sound-driver module
 * (audio/sound_driver.h) owns the synth.  Every OSystem audio member is one
 * INT 60h command the game sends (re_snd_drv.md §2.4), so this file is a
 * thin, thread-safe wrapper around the module — the game-side schedules (the
 * 2-of-3 music tick, the master-volume fades, the tune transitions) live in
 * the core (src/audio.c), where the original runs them.
 *
 * Threading: the tick thread (arch/sdl3/timer.c) calls the core's ISR body,
 * which calls audioTick/audioTempo; the SDL audio thread runs the sample
 * callback and calls generate.  Both touch the driver's OPL state, so every
 * call is serialised by _lock.  The critical sections are tiny (a tick steps
 * the sequencer; a tempo write is a handful of register writes).
 */
#include "osystem.h"

#include "audio/sound_driver.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdlib.h>

/* 48 kHz: Nuked-OPL3 resamples from its 49716 Hz native rate in
 * OPL3_Generate, so the stream runs at the host rate with no extra work. */
#define SDL3_AUDIO_RATE 48000
#define _CHUNK_FRAMES 1024

static SoundDriver *_driver;
static SDL_AudioStream *_stream;
static SDL_Mutex *_lock;
static uint16_t _voice = 0xFFFFu; /* the current cmd-5 handle ([0x5AF0]) */

/* The audio _probe (HUMANSRE_AUDIO_PROBE set): reports the rendered peak
 * once per second.  Verification aid for the D-01 wiring, not a feature. */
static int32_t _probe;
static uint32_t _probe_frames;
static int32_t _probe_peak;

/**
 * @brief The stream callback: render frames from the driver.
 *
 * @param userdata unused
 * @param stream the stream to feed
 * @param additional the bytes the caller wants
 * @param total the stream's total buffered bytes (unused)
 */
static void SDLCALL _audio_callback(void *userdata, SDL_AudioStream *stream,
                                    int additional, int total)
{
    int16_t chunk[_CHUNK_FRAMES * 2];
    int frames;
    (void)userdata;
    (void)total;

    if (!_driver || additional <= 0) return;
    frames = additional / ((int)sizeof(int16_t) * 2);
    while (frames > 0) {
        int n = frames > _CHUNK_FRAMES ? _CHUNK_FRAMES : frames;
        if (_lock) SDL_LockMutex(_lock);
        g_sound_driver.generate(_driver, chunk, (uint32_t)n);
        if (_lock) SDL_UnlockMutex(_lock);
        SDL_PutAudioStreamData(stream, chunk, n * (int)sizeof(int16_t) * 2);
        frames -= n;
        if (_probe) {
            int k;
            for (k = 0; k < n * 2; k++) {
                int32_t v = chunk[k];
                if (v < 0) v = -v;
                if (v > _probe_peak) _probe_peak = v;
            }
            _probe_frames += (uint32_t)n;
            if (_probe_frames >= SDL3_AUDIO_RATE) {
                SDL_Log("audio: 1 s rendered, peak %d", _probe_peak);
                _probe_frames = 0;
                _probe_peak = 0;
            }
        }
    }
}

/**
 * @brief Install the AdLib driver emulation and open the stream.
 *
 * @return 0 on success, -1 when the driver or the stream cannot be created
 */
int32_t sdl3_audio_install(void)
{
    SDL_AudioSpec spec;

    if (_driver) return 0;
    _driver = g_sound_driver.create(SDL3_AUDIO_RATE);
    if (!_driver) {
        SDL_Log("audio: cannot install the sound driver");
        return -1;
    }
    _lock = SDL_CreateMutex();
    if (!_lock) {
        SDL_Log("audio: cannot create the driver lock: %s", SDL_GetError());
        g_sound_driver.destroy(_driver);
        _driver = NULL;
        return -1;
    }
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = SDL3_AUDIO_RATE;
    _stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, _audio_callback, NULL);
    if (!_stream) {
        SDL_Log("audio: cannot open the stream: %s", SDL_GetError());
        SDL_DestroyMutex(_lock);
        _lock = NULL;
        g_sound_driver.destroy(_driver);
        _driver = NULL;
        return -1;
    }
    SDL_ResumeAudioStreamDevice(_stream);
    _probe = getenv("HUMANSRE_AUDIO_PROBE") != NULL;
    SDL_Log("audio: AdLib driver ready (%u Hz stereo)", SDL3_AUDIO_RATE);
    return 0;
}

/**
 * @brief cmd 0x0C: hand the loaded tune image to the driver.
 *
 * @param image the .ALB image
 * @param size its byte length
 * @return 0, always
 */
int32_t sdl3_audio_load_tune(const uint8_t *image, uint32_t size)
{
    if (!_driver) return -1;
    SDL_LockMutex(_lock);
    g_sound_driver.loadTune(_driver, image, size);
    SDL_UnlockMutex(_lock);
    return 0;
}

/**
 * @brief cmd 5, CX = 0: start the loaded tune's record 0 (music_resume).
 */
void sdl3_audio_resume(void)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    _voice = g_sound_driver.startEvent(_driver, 0);
    SDL_UnlockMutex(_lock);
}

/**
 * @brief cmd 6: release the current record (music_stop).
 */
void sdl3_audio_stop(void)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    if (_voice != 0xFFFFu) g_sound_driver.releaseEvent(_driver, _voice);
    _voice = 0xFFFFu;
    SDL_UnlockMutex(_lock);
}

/**
 * @brief cmd 5, CX = id: start a sound-effect record.
 *
 * The original's driver_call stores the new handle in [0x5AF0], overwriting
 * the music handle; that is reproduced here.
 *
 * @param id the event id
 */
void sdl3_audio_sfx(uint16_t id)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    _voice = g_sound_driver.startEvent(_driver, id);
    SDL_UnlockMutex(_lock);
}

/**
 * @brief cmd 0x0A: the master-volume byte.
 *
 * @param cl the game's byte
 */
void sdl3_audio_tempo(uint8_t cl)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    g_sound_driver.setTempo(_driver, cl);
    SDL_UnlockMutex(_lock);
}

/**
 * @brief cmd 2: step the sequencer one tick.
 */
void sdl3_audio_tick(void)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    g_sound_driver.tick(_driver);
    SDL_UnlockMutex(_lock);
}
