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

static uint8_t  pages[2][320 * 200];
static uint8_t  pal[256 * 3];
static TickFn   the_tick;
static unsigned the_rate;
static InputState the_input;    /* injected input (idle by default) */
static const InputState *script_states;   /* scripted input for smoke tests */
static unsigned script_n, script_i;

static void null_setMode(void) {}
static uint8_t *null_page(int which) { return pages[which & 1]; }
static void null_present(int which) { (void)which; }
static void null_setPalette(const uint8_t *p6, unsigned first, unsigned n)
{
    if (first + n > 256) n = 256 - first;
    memcpy(pal + first * 3, p6, (size_t)n * 3);
}
static void null_poll(InputState *out)
{
    if (script_states && script_i < script_n)
        the_input = script_states[script_i++];
    else if (script_states)
        the_input = script_states[script_n - 1];
    *out = the_input;
}
static void null_timerStart(unsigned rate, TickFn tick) { the_rate = rate; the_tick = tick; }
/* The null backend has no clock: the foreground loop itself is the tick
 * source (one tickSync call = one tick), so the scripted tests keep their
 * one-poll-per-tick timing.  osys_null_runTicks drives the ISR body by hand
 * for the content tests and advances the same counter. */
static unsigned tick_counter;          /* [0x5AC8] */
static void null_tickSync(unsigned *lastSeen)
{
    /* one tickSync = one tick: the ISR's INC [0x5AC8] (1000:8AA4) then the
     * ISR body (the palette step et al) — the foreground never sees a tick
     * boundary the ISR body has not already run (the real machine's INT 8
     * fires while the foreground spins) */
    tick_counter++;
    if (the_tick) the_tick();
    *lastSeen = tick_counter;
}
/* the audio calls are counted: the MUSIC board's gate is the only
 * observable the M5 stub has, and test_frontend asserts it */
static unsigned audio_starts, audio_stops;
static int  null_audioStart(void) { audio_starts++; return 0; }
static int  null_playTune(unsigned id) { (void)id; return 0; }
static void null_stop(void) { audio_stops++; }
static void null_fatal(const char *msg) { fprintf(stderr, "fatal: %s\n", msg); exit(1); }

const OSystem osys_null = {
    null_setMode, null_page, null_present, null_setPalette,
    null_poll, null_timerStart, null_tickSync,
    null_audioStart, null_playTune, null_stop,
    null_fatal
};

/* test hooks: drive N ticks by hand, inject input, read back video state
 * (the headless tests use these) */
void osys_null_runTicks(unsigned n)
{
    for (unsigned i = 0; i < n && the_tick; i++) {
        tick_counter++;                /* the ISR's INC [0x5AC8] (1000:8AA4) */
        the_tick();
    }
    (void)the_rate;
}

void osys_null_inject(const InputState *in) { the_input = *in; }

unsigned osys_null_audioStarts(void) { return audio_starts; }
unsigned osys_null_audioStops(void)  { return audio_stops; }

void osys_null_inputScript(const InputState *states, unsigned n)
{
    script_states = states;
    script_n = n;
    script_i = 0;
}

/* Scripted input for headless smoke tests: eventsPoll hands out the next
 * state per call; the last one repeats. */
const uint8_t *osys_null_page(int which) { return pages[which & 1]; }

const uint8_t *osys_null_palette(void) { return pal; }
