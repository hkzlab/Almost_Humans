/* arch/sdl3/audio.c — the SDL3 port's audio backend.
 *
 * The backend owns the sample stream; the sound-driver module
 * (audio/sound_driver.h) owns the synth.  Every OSystem audio member is one
 * INT 60h command the game sends (re_snd_drv.md §2.4), so this file is a
 * thin, thread-safe wrapper around the module — the game-side schedules (the
 * 2-of-3 music tick, the master-volume fades, the tune transitions) live in
 * the core (src/audio.c), where the original runs them.
 *
 * Threading: the SDL audio thread runs the sample callback, and it is the
 * audio clock — the core's audio tick (audio_render_tick) is stepped from
 * inside the render at the exact sample where the next game tick is due, so
 * a note's key-on/key-off lands on the sample the original's ISR wrote it at,
 * not on a render-chunk boundary.  The game-tick thread (arch/sdl3/timer.c)
 * still runs the ISR body, but its audio_tick call is a no-op while this
 * clock is active (audio_set_render_clock).  Both threads touch the driver's
 * synth state, so every call is serialised by _lock; the critical sections
 * are tiny.
 *
 * The driver module is chosen before the install (sdl3_audio_select), the
 * launcher's [0x5AFE] selection: AdLib by default, the PC speaker with
 * `--audio beeper`.
 */
#include "osystem.h"

#include "audio.h"
#include "audio/sound_driver.h"
#include "gameflow.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 48 kHz: Nuked-OPL3 resamples from its 49716 Hz native rate in
 * OPL3_Generate, so the stream runs at the host rate with no extra work.
 * The beeper renders its PIT square at the same host rate. */
#define SDL3_AUDIO_RATE 48000
#define _CHUNK_FRAMES   1024

static const SoundDriverModule *_module = &g_sound_driver_adlib;
static const char *_module_name = "AdLib";
static int32_t _driver_id = 1; /* [0x5AFE]: 0 Beeper, 1 AdLib */
static SoundDriver *_driver;
static SDL_AudioStream *_stream;
static SDL_Mutex *_lock;
static uint16_t _voice = 0xFFFFu; /* the current cmd-5 handle ([0x5AF0]) */
/* The game-tick grid in samples: SDL3_AUDIO_RATE / GAME_TICK_HZ.  _tick_acc
 * counts in frames * GAME_TICK_HZ, so a tick is due when it reaches
 * SDL3_AUDIO_RATE — exact integer arithmetic, no drift. */
static uint32_t _tick_acc;

/* The audio _probe (HUMANSRE_AUDIO_PROBE set): reports the rendered peak
 * once per second.  Verification aid for the D-01 wiring, not a feature. */
static int32_t _probe;
static uint32_t _probe_frames;
static int32_t _probe_peak;

/**
 * @brief The stream callback: render frames and step the audio clock.
 *
 * The render is split at every game-tick boundary, and the core's audio tick
 * runs between the two halves, so the sequencer's register writes take
 * effect at the exact sample the tick is due at.  Without the split a write
 * waits for the next callback chunk (~21 ms at the default buffer), which is
 * about one music tick: notes would come out a tick short or long.
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
        /* frames until the next game tick, and the chunk cap */
        int until = (int)((SDL3_AUDIO_RATE - _tick_acc + GAME_TICK_HZ - 1) /
                          GAME_TICK_HZ);
        int n = frames < until ? frames : until;
        if (n > _CHUNK_FRAMES) n = _CHUNK_FRAMES;
        if (_lock) SDL_LockMutex(_lock);
        _module->generate(_driver, chunk, (uint32_t)n);
        if (_lock) SDL_UnlockMutex(_lock);
        SDL_PutAudioStreamData(stream, chunk, n * (int)sizeof(int16_t) * 2);
        frames -= n;
        _tick_acc += (uint32_t)n * GAME_TICK_HZ;
        if (_tick_acc >= SDL3_AUDIO_RATE) {
            _tick_acc -= SDL3_AUDIO_RATE;
            audio_render_tick();
        }
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
    _driver = _module->create(SDL3_AUDIO_RATE);
    if (!_driver) {
        SDL_Log("audio: cannot install the sound driver");
        return -1;
    }
    _lock = SDL_CreateMutex();
    if (!_lock) {
        SDL_Log("audio: cannot create the driver lock: %s", SDL_GetError());
        _module->destroy(_driver);
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
        _module->destroy(_driver);
        _driver = NULL;
        return -1;
    }
    /* No stream = no sample clock: leave the render clock off so the game
     * tick keeps driving the audio state (silently) instead of hanging the
     * flow on a fade that never steps. */
    if (!SDL_ResumeAudioStreamDevice(_stream)) {
        SDL_Log("audio: cannot start the stream: %s", SDL_GetError());
        SDL_DestroyAudioStream(_stream);
        _stream = NULL;
        SDL_DestroyMutex(_lock);
        _lock = NULL;
        _module->destroy(_driver);
        _driver = NULL;
        return -1;
    }
    _tick_acc = 0;
    /* the render path owns the audio clock from here on (audio.h) */
    audio_set_render_clock(1);
    _probe = getenv("HUMANSRE_AUDIO_PROBE") != NULL;
    SDL_Log("audio: %s driver ready (%u Hz stereo)", _module_name,
            SDL3_AUDIO_RATE);
    return 0;
}

/**
 * @brief Select the resident driver before the install ([0x5AFE]).
 *
 * @param name "adlib" (the default) or "beeper"
 * @return 0 on success, -1 on an unknown name
 */
int32_t sdl3_audio_select(const char *name)
{
    if (!name || strcmp(name, "adlib") == 0) {
        _module = &g_sound_driver_adlib;
        _module_name = "AdLib";
        _driver_id = 1;
        return 0;
    }
    if (strcmp(name, "beeper") == 0) {
        _module = &g_sound_driver_beeper;
        _module_name = "Beeper";
        _driver_id = 0;
        return 0;
    }
    return -1;
}

/**
 * @brief The backend's [0x5AFE] sound_driver_id (osystem.h).
 *
 * @return 0 for the Beeper, 1 for AdLib; -1 before a selection is made
 */
int32_t sdl3_audio_driver_id(void) { return _driver_id; }

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
    _module->loadTune(_driver, image, size);
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
    _voice = _module->startEvent(_driver, 0);
    SDL_UnlockMutex(_lock);
}

/**
 * @brief cmd 6: release the current record (music_stop).
 */
void sdl3_audio_stop(void)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    if (_voice != 0xFFFFu) _module->releaseEvent(_driver, _voice);
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
    _voice = _module->startEvent(_driver, id);
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
    _module->setTempo(_driver, cl);
    SDL_UnlockMutex(_lock);
}

/**
 * @brief cmd 2: step the sequencer one tick.
 */
void sdl3_audio_tick(void)
{
    if (!_driver) return;
    SDL_LockMutex(_lock);
    _module->tick(_driver);
    SDL_UnlockMutex(_lock);
}
