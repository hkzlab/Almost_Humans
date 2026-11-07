/* arch/null/OSystem.c — the headless reference backend.
 *
 * Builds with no dependencies at all; used by CI and the headless tests.
 * The _pages are plain buffers, the input is always idle, the timer stores
 * the callback for the test to drive manually.  It is also the shape every
 * other backend must match (port_structure.md §2). */
#include "osystem.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void _null_set_mode(VideoMode mode);
static uint8_t *_null_page(int32_t which);
static void _null_present(int32_t which);
static void _null_set_palette(const uint8_t *p6, uint32_t first, uint32_t n);
static void _null_poll(InputState *out);
static void _null_timer_start(uint32_t rate, TickFn tick);
static void _null_tick_sync(uint32_t *lastSeen);
static int32_t _null_audio_install(void);
static int32_t _null_audio_load(const uint8_t *image, uint32_t size);
static void _null_audio_resume(void);
static void _null_audio_stop(void);
static void _null_audio_sfx(uint16_t id);
static void _null_audio_tempo(uint8_t cl);
static void _null_audio_tick(void);
static int32_t _null_audio_driver_id(void);
static void _null_fatal(const char *msg);

static uint8_t _pages[2][320 * 200];
static uint8_t _pal[256 * 3];
static TickFn _the_tick;
static uint32_t _the_rate;
static InputState _the_input;    /* injected input (idle by default) */
static const InputState *_script_states; /* scripted input for smoke tests */
static uint32_t _script_n, _script_i;
static VideoMode _mode_set; /* the core's last videoSetMode */

/* a test hook: the frame-evidence dumps key on the poll count, which only
 * the backend knows (the smoke script is indexed by eventsPoll calls) */
static void (*_present_hook)(uint32_t poll);
/* The null backend has no clock: the foreground loop itself is the tick
 * source (one tickSync call = one tick), so the scripted tests keep their
 * one-poll-per-tick timing.  osys_null_run_ticks drives the ISR body by hand
 * for the content tests and advances the same counter. */
static uint32_t _tick_counter; /* [0x5AC8] */
/* the audio calls are counted: the MUSIC board's gate is the only
 * observable the M5 stub has, and test_frontend asserts it */
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
    _null_fatal
};

/**
 * @brief The null backend: record the video mode.
 *
 * @param mode the mode
 */
static void _null_set_mode(VideoMode mode) { _mode_set = mode; }
/**
 * @brief Accessor: one of the null video pages.
 *
 * @param which the page selector
 * @return One of the null video pages.
 */
static uint8_t *_null_page(int32_t which) { return _pages[which & 1]; }
/**
 * @brief The null backend: run the present hook.
 *
 * @param which the page selector
 */
static void _null_present(int32_t which)
{
    (void)which;
    if (_present_hook) _present_hook(_script_i);
}
/**
 * @brief The null backend: record a palette range.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param n the count
 */
static void _null_set_palette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    if (first + n > 256) n = 256 - first;
    memcpy(_pal + first * 3, p6, (size_t)n * 3);
}
/**
 * @brief The null backend: inject the scripted input.
 *
 * @param out the output buffer
 */
static void _null_poll(InputState *out)
{
    if (_script_states && _script_i < _script_n)
        _the_input = _script_states[_script_i++];
    else if (_script_states) _the_input = _script_states[_script_n - 1];
    *out = _the_input;
}
/**
 * @brief The null backend: record the tick callback.
 *
 * @param rate the tick rate
 * @param tick the tick count
 */
static void _null_timer_start(uint32_t rate, TickFn tick)
{
    _the_rate = rate;
    _the_tick = tick;
}
/**
 * @brief The null backend: one tick per call.
 *
 * @param lastSeen the last seen tick
 */
static void _null_tick_sync(uint32_t *lastSeen)
{
    /* one tickSync = one tick: the ISR's INC [0x5AC8] (1000:8AA4) then the
     * ISR body (the palette step et al) — the foreground never sees a tick
     * boundary the ISR body has not already run (the real machine's INT 8
     * fires while the foreground spins) */
    _tick_counter++;
    if (_the_tick) _the_tick();
    *lastSeen = _tick_counter;
}
/** @brief The null backend: accept the driver install.
 *
 * @return 0, always (accepted)
 */
static int32_t _null_audio_install(void)
{
    _audio_installs++;
    return 0;
}
/**
 * @brief The null backend: record a tune image's size (cmd 0x0C).
 *
 * @param image the tune image
 * @param size its byte length
 * @return 0, always (accepted)
 */
static int32_t _null_audio_load(const uint8_t *image, uint32_t size)
{
    (void)image;
    if (_audio_loads < _AUDIO_LOADS_MAX) _audio_load_size[_audio_loads] = size;
    _audio_loads++;
    _audio_tune_size = size;
    return 0;
}
/**
 * @brief The null backend: count a music_resume (cmd 5, CX = 0).
 */
static void _null_audio_resume(void) { _audio_starts++; }
/**
 * @brief The null backend: count a music_stop (cmd 6).
 */
static void _null_audio_stop(void) { _audio_stops++; }
/**
 * @brief The null backend: record a sound effect request (cmd 5).
 *
 * @param id the event id
 */
static void _null_audio_sfx(uint16_t id)
{
    _audio_sfx_count++;
    _audio_last_sfx = id;
}
/**
 * @brief The null backend: record the master-volume command (cmd 0x0A).
 *
 * @param cl the game's byte
 */
static void _null_audio_tempo(uint8_t cl) { _audio_tempo = cl; }
/**
 * @brief The null backend: count a music tick (cmd 2).
 */
static void _null_audio_tick(void) { _audio_ticks++; }
/**
 * @brief The null backend's [0x5AFE]: AdLib, the port's default driver.
 *
 * @return 1 (AdLib)
 */
static int32_t _null_audio_driver_id(void) { return 1; }
/**
 * @brief The null backend: print and exit.
 *
 * @param msg the message
 */
static void _null_fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

/** @brief test hooks: drive N ticks by hand, inject input, read back video state
 * (the headless tests use these)
 *
 * @param n the count
 */
void osys_null_run_ticks(uint32_t n)
{
    for (uint32_t i = 0; i < n && _the_tick; i++) {
        _tick_counter++; /* the ISR's INC [0x5AC8] (1000:8AA4) */
        _the_tick();
    }
    (void)_the_rate;
}

/**
 * @brief Inject the input state the next poll returns.
 *
 * @param in the input state
 */
void osys_null_inject(const InputState *in) { _the_input = *in; }

/**
 * @brief Accessor: the audio-start count.
 *
 * @return The audio-start count.
 */
uint32_t osys_null_audio_starts(void) { return _audio_starts; }
/**
 * @brief Accessor: the audio-stop count.
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
 * @brief Accessor: the tune-load count and the last image's size.
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

/** @brief the script cursor = the eventsPoll count: the smoke test's frame dumps
 * key on it ("dump at tick N" = "dump when the N-th poll has happened")
 *
 * @return the script cursor
 */
uint32_t osys_null_poll_count(void) { return _script_i; }
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
    _script_i = 0;
}

/** @brief Scripted input for headless smoke tests: eventsPoll hands out the next
 * state per call; the last one repeats.
 *
 * @param which the page selector
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *osys_null_page(int32_t which) { return _pages[which & 1]; }

/**
 * @brief Accessor: the last palette written.
 *
 * @return The last palette written.
 */
const uint8_t *osys_null_palette(void) { return _pal; }
