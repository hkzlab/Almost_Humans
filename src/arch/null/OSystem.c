/* The headless reference backend: plain page buffers, idle input and a
 * manually driven tick.
 *
 * Port-only; used by CI and the headless tests, and the shape every other
 * backend matches.
 */
#include "osystem.h"
#include "fs/posix/fs_posix.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief The null backend: record the video mode.
 *
 * @param mode the mode
 */
static void _null_set_mode(VideoMode mode);

/**
 * @brief The null backend: one of the two video pages.
 *
 * @param which the page selector
 * @return the page, masked to 0 or 1
 */
static uint8_t *_null_page(int32_t which);

/**
 * @brief The null backend: run the present hook.
 *
 * @param which the page selector
 */
static void _null_present(int32_t which);

/**
 * @brief The null backend: record a palette range.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param n the count
 */
static void _null_set_palette(const uint8_t *p6, uint32_t first, uint32_t n);

/**
 * @brief The null backend: inject the scripted input.
 *
 * @param out the output buffer
 */
static void _null_poll(InputState *out);

/**
 * @brief The null backend: record the tick callback.
 *
 * @param rate the tick rate
 * @param tick the tick callback
 */
static void _null_timer_start(uint32_t rate, TickFn tick);

/**
 * @brief The null backend: one tick per call, in and out.
 *
 * @param lastSeen updated with the counter just run
 */
static void _null_tick_sync(uint32_t *lastSeen);

/** @brief The null backend: accept the driver install.
 *
 * @return 0
 */
static int32_t _null_audio_install(void);

/**
 * @brief The null backend: record a tune image's size (cmd 0x0C).
 *
 * @param image the tune image
 * @param size its byte length
 * @return 0
 */
static int32_t _null_audio_load(const uint8_t *image, uint32_t size);

/**
 * @brief The null backend: count a music_resume (cmd 5, CX = 0).
 */
static void _null_audio_resume(void);

/**
 * @brief The null backend: count a music_stop (cmd 6).
 */
static void _null_audio_stop(void);

/**
 * @brief The null backend: record a sound effect request (cmd 5).
 *
 * @param id the event id
 */
static void _null_audio_sfx(uint16_t id);

/**
 * @brief The null backend: record the master-volume command (cmd 0x0A).
 *
 * @param cl the game's byte
 */
static void _null_audio_tempo(uint8_t cl);

/**
 * @brief The null backend: count a music tick (cmd 2).
 */
static void _null_audio_tick(void);

/**
 * @brief [0x5AFE] for the null backend: AdLib.
 *
 * @return 1 (AdLib)
 */
static int32_t _null_audio_driver_id(void);

/**
 * @brief The null backend: a non-fatal diagnostic goes to stderr.
 *
 * @param msg the message
 */
static void _null_warn(const char *msg);

/**
 * @brief The null backend: print and exit.
 *
 * @param msg the message
 */
static void _null_fatal(const char *msg);

static uint8_t _pages[2][VIDEO_PAGE_BYTES];
static uint8_t _pal[256 * 3];
static TickFn _the_tick;
static InputState _the_input;    // injected input (idle by default)
static const InputState *_script_states; // scripted input for smoke tests
static uint32_t _script_n, _poll_count;
static VideoMode _mode_set; // the core's last videoSetMode

/* The frame-evidence dumps key on the poll count, which only the backend
 * knows (the smoke script is indexed by eventsPoll calls). */
static void (*_present_hook)(uint32_t poll);
/* The null backend has no clock: the foreground loop itself is the tick
 * source (one tickSync call = one tick), so the scripted tests keep their
 * one-poll-per-tick timing.  osys_null_run_ticks drives the ISR body by hand
 * for the content tests and advances the same counter. */
static uint32_t _tick_counter; // [0x5AC8]
static uint32_t _tick_limit; // 0 = forever: the tests' default
/* The audio calls are counted: the MUSIC board's gate is the only
 * observable, and test_frontend asserts it. */
static uint32_t _audio_starts, _audio_stops;
static uint32_t _audio_installs, _audio_loads, _audio_sfx_count, _audio_ticks;
static uint32_t _audio_tune_size;
#define _AUDIO_LOADS_MAX 16
static uint32_t _audio_load_size[_AUDIO_LOADS_MAX];
static uint16_t _audio_last_sfx;
static uint8_t _audio_tempo;

const OSystem g_osys_null = {
    _null_set_mode,     _null_page,          _null_present,
    _null_set_palette,  _null_poll,          _null_timer_start,
    _null_tick_sync,    _null_audio_install, _null_audio_load,
    _null_audio_resume, _null_audio_stop,    _null_audio_sfx,
    _null_audio_tempo,  _null_audio_tick,    _null_audio_driver_id,
    _null_warn,         _null_fatal,         &g_fs_posix
};

static void _null_set_mode(VideoMode mode) { _mode_set = mode; }
static uint8_t *_null_page(int32_t which) { return _pages[which & 1]; }
static void _null_present(int32_t which)
{
    (void)which;
    if (_present_hook) _present_hook(_poll_count);
}
static void _null_set_palette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    if (first + n > 256) n = 256 - first;
    memcpy(_pal + first * 3, p6, (size_t)n * 3);
}
static void _null_poll(InputState *out)
{
    if (_script_states && _script_n) {
        /* the script holds one state per poll; the last one repeats */
        uint32_t i = _poll_count < _script_n ? _poll_count : _script_n - 1u;
        _the_input = _script_states[i];
    }
    _poll_count++;
    *out = _the_input;
    /* the headless executable's --ticks: quit through the core's own path
     * (game_main's input.quit check), not from inside the backend */
    if (_tick_limit && _tick_counter >= _tick_limit) out->quit = 1;
}
static void _null_timer_start(uint32_t rate, TickFn tick)
{
    (void)rate; // the null backend has no clock: one tickSync = one tick
    _the_tick = tick;
}
/* One tick boundary: bump the counter and run the ISR body. */
static void _null_tick_sync(uint32_t *lastSeen)
{
    /* one tickSync = one tick: the ISR's INC [0x5AC8] (1000:8AA4) then the
     * ISR body (the palette step et al) - the foreground never sees a tick
     * boundary the ISR body has not already run (the real machine's INT 8
     * fires while the foreground spins) */
    _tick_counter++;
    if (_the_tick) _the_tick();
    *lastSeen = _tick_counter;
}
static int32_t _null_audio_install(void)
{
    _audio_installs++;
    return 0;
}
static int32_t _null_audio_load(const uint8_t *image, uint32_t size)
{
    (void)image;
    if (_audio_loads < _AUDIO_LOADS_MAX) _audio_load_size[_audio_loads] = size;
    _audio_loads++;
    _audio_tune_size = size;
    return 0;
}
static void _null_audio_resume(void) { _audio_starts++; }
static void _null_audio_stop(void) { _audio_stops++; }
static void _null_audio_sfx(uint16_t id)
{
    _audio_sfx_count++;
    _audio_last_sfx = id;
}
static void _null_audio_tempo(uint8_t cl) { _audio_tempo = cl; }
static void _null_audio_tick(void) { _audio_ticks++; }
static int32_t _null_audio_driver_id(void) { return 1; }
static void _null_warn(const char *msg) { fprintf(stderr, "%s\n", msg); }
static void _null_fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

void osys_null_set_tick_limit(uint32_t n) { _tick_limit = n; }

/** @brief Drive N ticks by hand (the headless content tests).
 *
 * @param n the count
 */
void osys_null_run_ticks(uint32_t n)
{
    for (uint32_t i = 0; i < n && _the_tick; i++) {
        _tick_counter++; // the ISR's INC [0x5AC8] (1000:8AA4)
        _the_tick();
    }
}

/**
 * @brief Inject the input state the next poll returns.
 *
 * @param in the input state
 */
void osys_null_inject(const InputState *in) { _the_input = *in; }

/**
 * @brief Accessor: the music-resume (cmd 5, CX = 0) count.
 *
 * @return The audio-start count.
 */
uint32_t osys_null_audio_starts(void) { return _audio_starts; }
/**
 * @brief Accessor: the music-stop (cmd 6) count.
 *
 * @return The audio-stop count.
 */
uint32_t osys_null_audio_stops(void) { return _audio_stops; }
/**
 * @brief Accessor: the install count.
 *
 * @return The install count.
 */
uint32_t osys_null_audio_installs(void) { return _audio_installs; }
/**
 * @brief Accessor: the tune-load count.
 *
 * @return The tune-load count.
 */
uint32_t osys_null_audio_loads(void) { return _audio_loads; }
/**
 * @brief Accessor: the last loaded tune's byte size.
 *
 * @return The size.
 */
uint32_t osys_null_audio_tune_size(void) { return _audio_tune_size; }
/**
 * @brief Accessor: the byte size of the i-th tune load.
 *
 * @param i the load index
 * @return the size, 0 when out of range
 */
uint32_t osys_null_audio_load_size(uint32_t i)
{ return i < _AUDIO_LOADS_MAX ? _audio_load_size[i] : 0u; }
/**
 * @brief Accessor: the sound-effect command count.
 *
 * @return The count.
 */
uint32_t osys_null_audio_sfx_count(void) { return _audio_sfx_count; }
/**
 * @brief Accessor: the last sound-effect event id.
 *
 * @return The event id, 0xFFFF when none.
 */
uint16_t osys_null_audio_last_sfx(void)
{ return _audio_sfx_count ? _audio_last_sfx : 0xFFFFu; }
/**
 * @brief Accessor: the last master-volume byte (cmd 0x0A).
 *
 * @return The byte.
 */
uint8_t osys_null_audio_tempo(void) { return _audio_tempo; }
/**
 * @brief Accessor: the music-tick count (cmd 2).
 *
 * @return The count.
 */
uint32_t osys_null_audio_ticks(void) { return _audio_ticks; }
/**
 * @brief Accessor: the last video mode set.
 *
 * @return The last video mode set.
 */
VideoMode osys_null_video_mode(void) { return _mode_set; }

/** @brief The eventsPoll count: the smoke test's frame-dump clock.
 *
 * @return the eventsPoll call count
 */
uint32_t osys_null_poll_count(void) { return _poll_count; }
/**
 * @brief Install the present hook.
 *
 * @param fn the callback
 */
void osys_null_on_present(void (*fn)(uint32_t poll)) { _present_hook = fn; }

/**
 * @brief Install a scripted input sequence.
 *
 * @param states the input sequence
 * @param n the count
 */
void osys_null_input_script(const InputState *states, uint32_t n)
{
    _script_states = states;
    _script_n = n;
    _poll_count = 0;
}

/** @brief Accessor: the i-th null video page.
 *
 * @param which the page selector
 * @return the page, masked to 0 or 1
 */
const uint8_t *osys_null_page(int32_t which) { return _pages[which & 1]; }

/**
 * @brief Accessor: the last palette written.
 *
 * @return The last palette written.
 */
const uint8_t *osys_null_palette(void) { return _pal; }
