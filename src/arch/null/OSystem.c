/* arch/null/OSystem.c — the headless reference backend.
 *
 * Builds with no dependencies at all; used by CI and the headless tests.
 * The pages are plain buffers, the input is always idle, the timer stores
 * the callback for the test to drive manually.  It is also the shape every
 * other backend must match (port_structure.md §2). */
#include "osystem.h"
#include <stdlib.h>
#include <string.h>

static uint8_t  pages[2][320 * 200];
static uint8_t  pal[256 * 3];
static TickFn   the_tick;
static unsigned the_rate;

static void null_setMode(void) {}
static uint8_t *null_page(int which) { return pages[which & 1]; }
static void null_present(int which) { (void)which; }
static void null_setPalette(const uint8_t *p6, unsigned first, unsigned n)
{
    if (first + n > 256) n = 256 - first;
    memcpy(pal + first * 3, p6, (size_t)n * 3);
}
static void null_poll(InputState *out) { out->joyBits = 0; out->lastKey = 0; out->quit = 0; }
static void null_timerStart(unsigned rate, TickFn tick) { the_rate = rate; the_tick = tick; }
static int  null_audioStart(void) { return 0; }
static int  null_playTune(unsigned id) { (void)id; return 0; }
static void null_stop(void) {}
static FILE *null_openGame(const char *name) { return fopen(name, "rb"); }
static FILE *null_openSave(unsigned slot, const char *mode)
{ char n[64]; snprintf(n, sizeof n, "save%u.sav", slot); return fopen(n, mode); }
static void null_fatal(const char *msg) { fprintf(stderr, "fatal: %s\n", msg); exit(1); }

const OSystem osys_null = {
    null_setMode, null_page, null_present, null_setPalette,
    null_poll, null_timerStart,
    null_audioStart, null_playTune, null_stop,
    null_openGame, null_openSave,
    null_fatal
};

/* test hook: drive N ticks by hand (the headless tests use this) */
void osys_null_runTicks(unsigned n)
{
    for (unsigned i = 0; i < n && the_tick; i++) the_tick();
    (void)the_rate;
}
