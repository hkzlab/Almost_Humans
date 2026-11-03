/* arch/sdl3/timer.c — the fixed-tick source: the PIT/INT 8 heartbeat's
 * equivalent.  A dedicated thread calls the tick callback at ticksPerSec;
 * the game's foreground loop runs on the main thread (the same split as
 * the original's ISR/foreground pair).
 *
 * Pacing (re_map.md "Timer / INT 8"): the original's ISR increments the
 * tick counter [0x5AC8] once per display period and re-arms the PIT
 * one-shot, re-syncing to vsync — so one tick = one display period
 * (GAME_TICK_HZ), never twice per period and never host-refresh-paced.
 * tickSync is that counter's foreground gate: the loop blocks on it, so
 * SDL_SetRenderVSync is a presentation detail, not a clock.
 *
 * Why a thread and not SDL_AddTimerNS: measured live (HUMANSRE_RATE_PROBE,
 * Windows/ARM64), SDL's timer thread delivers a 14.29 ms schedule at
 * 63.5 Hz — its scheduler granularity eats a tick every ~6. OS timers
 * cannot be trusted for the game's heartbeat; this thread sleeps until
 * close and spins the last stretch on the QPC-backed clock instead. */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

static TickFn theTick;
static uint32_t theRate;
static Uint64 tickIntervalNs;
static SDL_AtomicInt tickCount;        /* [0x5AC8] */
static SDL_AtomicInt running;

/* The rate probe (HUMANSRE_RATE_PROBE set): reports the measured tick rate
 * every 5 s.  Verification aid for the pacing work, not a game feature. */
static int32_t probe;
static uint32_t probeTicks;
static Uint64 probeWindowStartNs;

static void fire_tick(void)
{
    SDL_AddAtomicInt(&tickCount, 1);   /* the ISR's INC [0x5AC8] (1000:8AA4) */
    theTick();
    if (probe) {
        probeTicks++;
        Uint64 now = SDL_GetTicksNS();
        if (now - probeWindowStartNs >= 5000000000ull) {
            double s = (double)(now - probeWindowStartNs) / 1e9;
            SDL_Log("tick rate: %.2f Hz (target %u)", (double)probeTicks / s, theRate);
            probeTicks = 0;
            probeWindowStartNs = now;
        }
    }
}

static int32_t SDLCALL tickThreadFn(void *arg)
{
    (void)arg;
    Uint64 next = SDL_GetTicksNS() + tickIntervalNs;
    while (SDL_GetAtomicInt(&running)) {
        /* the hybrid wait: sleep while the boundary is far, spin the
         * last ~2 ms where the OS granularity would show */
        for (;;) {
            Sint64 remain = (Sint64)next - (Sint64)SDL_GetTicksNS();
            if (remain <= 0) break;
            if (remain > 2000000) SDL_Delay((Uint32)((remain - 1000000) / 1000000));
        }
        fire_tick();
        next += tickIntervalNs;
        /* a stall (debugger, heavy frame) must not burst ticks to catch up:
         * the original's delta-spin semantics drop missed boundaries */
        if ((Sint64)(SDL_GetTicksNS() - next) > 0) next = SDL_GetTicksNS() + tickIntervalNs;
    }
    return 0;
}

void sdl3_timerStart(uint32_t ticksPerSec, TickFn tick)
{
    theTick = tick;
    theRate = ticksPerSec;
    tickIntervalNs = 1000000000ull / ticksPerSec;  /* 14285714.29 at 70: the
                                                    * constant errs 0.3 ns/tick,
                                                    * no accumulation */
    SDL_SetAtomicInt(&tickCount, 0);
    probe = getenv("HUMANSRE_RATE_PROBE") != NULL;
    probeTicks = 0;
    probeWindowStartNs = SDL_GetTicksNS();
    SDL_SetAtomicInt(&running, 1);
    /* the thread lives until process exit (the original's ISR is likewise
     * installed for the session; there is no timerStop in the interface) */
    SDL_DetachThread(SDL_CreateThread(tickThreadFn, "humansre-tick", NULL));
}

void sdl3_tickSync(uint32_t *lastSeen)
{
    for (;;) {
        int32_t now = SDL_GetAtomicInt(&tickCount);
        if ((uint32_t)now != *lastSeen) {
            *lastSeen = (uint32_t)now;
            return;
        }
        SDL_Delay(1);   /* the wait_vsync-and-abort spin, sleeping */
    }
}

