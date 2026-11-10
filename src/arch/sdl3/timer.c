/* arch/sdl3/timer.c - the fixed-tick source: the PIT/INT 8 heartbeat's
 * equivalent.  A dedicated thread calls the tick callback at ticksPerSec;
 * the game's foreground loop runs on the main thread (the same split as
 * the original's ISR/foreground pair).
 *
 * Pacing: the original's ISR increments the
 * tick counter [0x5AC8] once per display period and re-arms the PIT
 * one-shot, re-syncing to vsync - so one tick = one display period
 * (GAME_TICK_HZ), never twice per period and never host-refresh-paced.
 * tickSync is that counter's foreground gate: the loop blocks on it, so
 * SDL_SetRenderVSync is a presentation detail, not a clock.
 *
 * Why a thread and not SDL_AddTimerNS: measured live (HUMANSRE_RATE_PROBE,
 * Windows/ARM64), SDL's timer thread delivers a 14.29 ms schedule at
 * 63.5 Hz - its scheduler granularity eats a tick every ~6. OS timers
 * cannot be trusted for the game's heartbeat; this thread waits on
 * SDL_DelayPrecise, which sleeps up to the boundary instead of burning
 * the last stretch in a spin. */
#include "osystem.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

static TickFn _the_tick;
static uint32_t _the_rate;
static Uint64 _tick_interval_ns;
static SDL_AtomicInt _tick_count;        // [0x5AC8]
static SDL_AtomicInt _running;

/* The rate _probe (HUMANSRE_RATE_PROBE set): reports the measured tick rate
 * every 5 s.  Verification aid for the pacing work, not a game feature. */
static int32_t _probe;
static uint32_t _probe_ticks;
static Uint64 _probe_window_start_ns;

/**
 * @brief Run the fixed-tick callback and advance the tick counter.
 */
static void _fire_tick(void)
{
    SDL_AddAtomicInt(&_tick_count, 1); // the ISR's INC [0x5AC8] (1000:8AA4)
    _the_tick();
    if (_probe) {
        _probe_ticks++;
        Uint64 now = SDL_GetTicksNS();
        if (now - _probe_window_start_ns >= 5000000000ull) {
            double s = (double)(now - _probe_window_start_ns) / 1e9;
            SDL_Log("tick rate: %.2f Hz (target %u)", (double)_probe_ticks / s,
                    _the_rate);
            _probe_ticks = 0;
            _probe_window_start_ns = now;
        }
    }
}

/**
 * @brief The backend's tick thread: fire the callback at the fixed rate.
 *
 * @param arg the argument
 * @return 0 when the thread exits
 */
static int32_t SDLCALL _tick_thread_fn(void *arg)
{
    (void)arg;
    Uint64 next = SDL_GetTicksNS() + _tick_interval_ns;
    while (SDL_GetAtomicInt(&_running)) {
        /* SDL_DelayPrecise sleeps to the boundary and spins only the
         * scheduler's overshoot, where the old hybrid wait hand-spun the
         * last ~1.3 ms of every 14.3 ms tick (about 9% of a core). */
        Sint64 remain = (Sint64)next - (Sint64)SDL_GetTicksNS();
        if (remain > 0) SDL_DelayPrecise((Uint64)remain);
        _fire_tick();
        next += _tick_interval_ns;
        /* a stall (debugger, heavy frame) must not burst ticks to catch up:
         * the original's delta-spin semantics drop missed boundaries */
        if ((Sint64)(SDL_GetTicksNS() - next) > 0)
            next = SDL_GetTicksNS() + _tick_interval_ns;
    }
    return 0;
}

void sdl3_timer_start(uint32_t ticksPerSec, TickFn tick)
{
    _the_tick = tick;
    _the_rate = ticksPerSec;
    _tick_interval_ns = 1000000000ull / ticksPerSec; /* 14285714.29 at 70: the
                                                    * constant errs 0.3 ns/tick,
                                                    * no accumulation */
    SDL_SetAtomicInt(&_tick_count, 0);
    _probe = getenv("HUMANSRE_RATE_PROBE") != NULL;
    _probe_ticks = 0;
    _probe_window_start_ns = SDL_GetTicksNS();
    SDL_SetAtomicInt(&_running, 1);
    /* the thread lives until process exit (the original's ISR is likewise
     * installed for the session; there is no timerStop in the interface) */
    SDL_DetachThread(SDL_CreateThread(_tick_thread_fn, "humansre-tick", NULL));
}

void sdl3_tick_sync(uint32_t *lastSeen)
{
    while (1) {
        int32_t now = SDL_GetAtomicInt(&_tick_count);
        if ((uint32_t)now != *lastSeen) {
            *lastSeen = (uint32_t)now;
            return;
        }
        SDL_Delay(1); // the wait_vsync-and-abort spin, sleeping
    }
}
