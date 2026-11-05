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
static int32_t _null_audio_start(void);
static int32_t _null_play_tune(uint32_t id);
static void _null_stop(void);
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

const OSystem g_osys_null = {
    _null_set_mode,  _null_page,        _null_present,   _null_set_palette,
    _null_poll,      _null_timer_start, _null_tick_sync, _null_audio_start,
    _null_play_tune, _null_stop,        _null_fatal
};

/** @brief The null backend: record the video mode. */
static void _null_set_mode(VideoMode mode) { _mode_set = mode; }
/** @brief Accessor: one of the null video pages. */
static uint8_t *_null_page(int32_t which) { return _pages[which & 1]; }
/** @brief The null backend: run the present hook. */
static void _null_present(int32_t which)
{
    (void)which;
    if (_present_hook) _present_hook(_script_i);
}
/** @brief The null backend: record a palette range. */
static void _null_set_palette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    if (first + n > 256) n = 256 - first;
    memcpy(_pal + first * 3, p6, (size_t)n * 3);
}
/** @brief The null backend: inject the scripted input. */
static void _null_poll(InputState *out)
{
    if (_script_states && _script_i < _script_n)
        _the_input = _script_states[_script_i++];
    else if (_script_states) _the_input = _script_states[_script_n - 1];
    *out = _the_input;
}
/** @brief The null backend: record the tick callback. */
static void _null_timer_start(uint32_t rate, TickFn tick)
{
    _the_rate = rate;
    _the_tick = tick;
}
/** @brief The null backend: one tick per call. */
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
/** @brief The null backend: accept an audio start. */
static int32_t _null_audio_start(void)
{
    _audio_starts++;
    return 0;
}
/** @brief The null backend: accept a tune. */
static int32_t _null_play_tune(uint32_t id)
{
    (void)id;
    return 0;
}
/** @brief The null backend: count an audio stop. */
static void _null_stop(void) { _audio_stops++; }
/** @brief The null backend: print and exit. */
static void _null_fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

/** test hooks: drive N ticks by hand, inject input, read back video state
 * (the headless tests use these) */
void osys_null_run_ticks(uint32_t n)
{
    for (uint32_t i = 0; i < n && _the_tick; i++) {
        _tick_counter++; /* the ISR's INC [0x5AC8] (1000:8AA4) */
        _the_tick();
    }
    (void)_the_rate;
}

/** @brief Inject the input state the next poll returns. */
void osys_null_inject(const InputState *in) { _the_input = *in; }

/** @brief Accessor: the audio-start count. */
uint32_t osys_null_audio_starts(void) { return _audio_starts; }
/** @brief Accessor: the audio-stop count. */
uint32_t osys_null_audio_stops(void) { return _audio_stops; }
/** @brief Accessor: the last video mode set. */
VideoMode osys_null_video_mode(void) { return _mode_set; }

/** the script cursor = the eventsPoll count: the smoke test's frame dumps
 * key on it ("dump at tick N" = "dump when the N-th poll has happened") */
uint32_t osys_null_poll_count(void) { return _script_i; }
/** @brief Install the present hook. */
void osys_null_on_present(void (*fn)(uint32_t poll)) { _present_hook = fn; }

/** @brief Install a scripted input sequence. */
void osys_null_input_script(const InputState *states, uint32_t n)
{
    _script_states = states;
    _script_n = n;
    _script_i = 0;
}

/** Scripted input for headless smoke tests: eventsPoll hands out the next
 * state per call; the last one repeats. */
const uint8_t *osys_null_page(int32_t which) { return _pages[which & 1]; }

/** @brief Accessor: the last palette written. */
const uint8_t *osys_null_palette(void) { return _pal; }
