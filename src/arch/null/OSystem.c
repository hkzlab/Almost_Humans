/* arch/null/OSystem.c — the headless reference backend.
 *
 * Builds with no dependencies at all; used by CI and the headless tests.
 * The pages are plain buffers, the input is always idle, the timer stores
 * the callback for the test to drive manually.  It is also the shape every
 * other backend must match (port_structure.md §2). */
#include "osystem.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void null_setMode(VideoMode mode);
static uint8_t *null_page(int32_t which);
static void null_present(int32_t which);
static void null_setPalette(const uint8_t *p6, uint32_t first, uint32_t n);
static void null_poll(InputState *out);
static void null_timerStart(uint32_t rate, TickFn tick);
static void null_tickSync(uint32_t *lastSeen);
static int32_t null_audioStart(void);
static int32_t null_playTune(uint32_t id);
static void null_stop(void);
static void null_fatal(const char *msg);

static uint8_t pages[2][320 * 200];
static uint8_t pal[256 * 3];
static TickFn the_tick;
static uint32_t the_rate;
static InputState the_input;    /* injected input (idle by default) */
static const InputState *script_states;   /* scripted input for smoke tests */
static uint32_t script_n, script_i;
static VideoMode modeSet;                 /* the core's last videoSetMode */

/* a test hook: the frame-evidence dumps key on the poll count, which only
 * the backend knows (the smoke script is indexed by eventsPoll calls) */
static void (*present_hook)(uint32_t poll);
/* The null backend has no clock: the foreground loop itself is the tick
 * source (one tickSync call = one tick), so the scripted tests keep their
 * one-poll-per-tick timing.  osys_null_runTicks drives the ISR body by hand
 * for the content tests and advances the same counter. */
static uint32_t tick_counter;          /* [0x5AC8] */
/* the audio calls are counted: the MUSIC board's gate is the only
 * observable the M5 stub has, and test_frontend asserts it */
static uint32_t audio_starts, audio_stops;

const OSystem osys_null = { null_setMode,    null_page,       null_present,
                            null_setPalette, null_poll,       null_timerStart,
                            null_tickSync,   null_audioStart, null_playTune,
                            null_stop,       null_fatal };

static void null_setMode(VideoMode mode) { modeSet = mode; }
static uint8_t *null_page(int32_t which) { return pages[which & 1]; }
static void null_present(int32_t which)
{
    (void)which;
    if (present_hook) present_hook(script_i);
}
static void null_setPalette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    if (first + n > 256) n = 256 - first;
    memcpy(pal + first * 3, p6, (size_t)n * 3);
}
static void null_poll(InputState *out)
{
    if (script_states && script_i < script_n)
        the_input = script_states[script_i++];
    else if (script_states) the_input = script_states[script_n - 1];
    *out = the_input;
}
static void null_timerStart(uint32_t rate, TickFn tick)
{
    the_rate = rate;
    the_tick = tick;
}
static void null_tickSync(uint32_t *lastSeen)
{
    /* one tickSync = one tick: the ISR's INC [0x5AC8] (1000:8AA4) then the
     * ISR body (the palette step et al) — the foreground never sees a tick
     * boundary the ISR body has not already run (the real machine's INT 8
     * fires while the foreground spins) */
    tick_counter++;
    if (the_tick) the_tick();
    *lastSeen = tick_counter;
}
static int32_t null_audioStart(void)
{
    audio_starts++;
    return 0;
}
static int32_t null_playTune(uint32_t id)
{
    (void)id;
    return 0;
}
static void null_stop(void) { audio_stops++; }
static void null_fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

/* test hooks: drive N ticks by hand, inject input, read back video state
 * (the headless tests use these) */
void osys_null_runTicks(uint32_t n)
{
    for (uint32_t i = 0; i < n && the_tick; i++) {
        tick_counter++; /* the ISR's INC [0x5AC8] (1000:8AA4) */
        the_tick();
    }
    (void)the_rate;
}

void osys_null_inject(const InputState *in) { the_input = *in; }

uint32_t osys_null_audioStarts(void) { return audio_starts; }
uint32_t osys_null_audioStops(void) { return audio_stops; }
VideoMode osys_null_videoMode(void) { return modeSet; }

/* the script cursor = the eventsPoll count: the smoke test's frame dumps
 * key on it ("dump at tick N" = "dump when the N-th poll has happened") */
uint32_t osys_null_pollCount(void) { return script_i; }
void osys_null_onPresent(void (*fn)(uint32_t poll)) { present_hook = fn; }

void osys_null_inputScript(const InputState *states, uint32_t n)
{
    script_states = states;
    script_n = n;
    script_i = 0;
}

/* Scripted input for headless smoke tests: eventsPoll hands out the next
 * state per call; the last one repeats. */
const uint8_t *osys_null_page(int32_t which) { return pages[which & 1]; }

const uint8_t *osys_null_palette(void) { return pal; }
