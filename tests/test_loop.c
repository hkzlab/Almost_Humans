/* test_loop.c — the flow/tick state machine under the null backend.
 *
 * Assert-based (no framework): exits 0 silently on success, prints the
 * failing fact to stderr and exits 1 otherwise.
 *
 * With the real attract loop in place (game_loop 1000:131E), FLOW_ATTRACT
 * no longer auto-advances: the four-effect cycle runs until the escape
 * (attract_exit 1000:13E9 — the action key or any held key), whose final
 * fade completes inside attract_tick.  Without the game data the effects
 * skip (the headless pattern), so the cycle spins and the phase probe can
 * still assert the 1 -> 3 -> 2 -> 4 order; palette_tick() is driven by
 * hand, once per flow step, like game_main's game_tick. */
#include "game.h"
#include <stdio.h>
#include <string.h>

extern const OSystem osys_null;

static InputState make_input(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;              /* joyBits 0 = fire released, lastKey 0 = none */
}

static int expect_state(GameFlow got, GameFlow want, unsigned tick, const char *what)
{
    if (got == want) return 0;
    fprintf(stderr, "FAIL at tick %u (%s): state %d, want %d\n",
            tick, what, (int)got, (int)want);
    return 1;
}

/* the effect a phase belongs to: 1, 3, 2, 4 (the enum groups) */
static unsigned effect_of(unsigned phase)
{
    if (phase >= ATTRACT_E1_LOAD && phase <= ATTRACT_E1_OUT) return 1u;
    if (phase >= ATTRACT_E3_LOAD && phase <= ATTRACT_E3_OUT) return 3u;
    if (phase >= ATTRACT_E2_LOAD && phase <= ATTRACT_E2_OUT) return 2u;
    if (phase >= ATTRACT_E4_LOAD && phase <= ATTRACT_E4_OUT) return 4u;
    return 0u;
}

int main(void)
{
    int bad = 0;
    unsigned tick = 0;

    /* the module set the attract needs (game_main's resources_init order,
     * trimmed to the flow-relevant ones) */
    {
        const OSystem *os = &osys_null;
        if (res_init(os) || video_init(os) || palette_init(os) ||
            font_init(os) || attract_init(os)) {
            fprintf(stderr, "FAIL: module init\n");
            return 1;
        }
    }

    /* 1. ATTRACT holds on idle — the cycle never auto-advances (the old
     *    5 s dwell is gone; only the escape exits it). */
    {
        InputState in = make_input();
        GameFlow s = FLOW_ATTRACT;
        for (tick = 1; tick <= 2000u; tick++) {
            s = gameflow_step(s, &in);
            palette_tick();               /* the ISR body's fade step */
            if (s != FLOW_ATTRACT) {
                fprintf(stderr, "FAIL at tick %u: idle attract left the "
                        "state (%d)\n", tick, (int)s);
                bad = 1;
                break;
            }
        }
    }

    /* 2. the effect order 1 -> 3 -> 2 -> 4: without data each LOAD fails
     *    and the cycle spins one effect per tick, so the phase sequence
     *    observed before each step shows the cycle. */
    {
        InputState in = make_input();
        GameFlow s = FLOW_ATTRACT;
        unsigned seq[12];
        unsigned n = 0, runs = 0;
        unsigned last = 0;
        for (tick = 0; tick < 16u && runs < 8u; tick++) {
            unsigned ph = attract_phaseForProbe();
            unsigned e = effect_of(ph);
            if (e && e != last) {
                if (n < 12u) seq[n] = e;
                n++;
                runs++;
                last = e;
            }
            s = gameflow_step(s, &in);
            palette_tick();
        }
        /* the cycle edges: 1 -> 3 -> 2 -> 4 -> 1, rotation-independent
         * (test 1's idle ticks already left the machine mid-cycle, so the
         * sequence's start is arbitrary — its structure is not) */
        static const unsigned after[5] = { 0u, 3u, 4u, 2u, 1u };
        if (runs < 8u) {
            fprintf(stderr, "FAIL: only %u effect runs observed\n", runs);
            bad = 1;
        }
        for (unsigned i = 0; i + 1 < 8u; i++) {
            if (seq[i] < 1u || seq[i] > 4u || after[seq[i]] != seq[i + 1]) {
                fprintf(stderr, "FAIL: effect order[%u] = %u -> %u, want "
                        "the 1->3->2->4 cycle\n", i, seq[i], seq[i + 1]);
                bad = 1;
                break;
            }
        }
    }

    /* 3. the escape: the action bit latches the 13B2 fade, whose ~35
     *    ticks (div 0x23) end the attract -> MENU. */
    {
        InputState in = make_input();
        in.joyBits = 0x80u;               /* the configured action key */
        GameFlow s = FLOW_ATTRACT;
        s = gameflow_step(s, &in);
        palette_tick();
        bad |= expect_state(s, FLOW_ATTRACT, 0, "escape: fade still running");
        InputState idle = make_input();
        unsigned guard = 0;
        while (s == FLOW_ATTRACT && guard++ < 200u) {
            s = gameflow_step(s, &idle);
            palette_tick();
        }
        if (s != FLOW_MENU) {
            fprintf(stderr, "FAIL: the escape never reached MENU "
                    "(state %d after %u ticks)\n", (int)s, guard);
            bad = 1;
        }
    }

    /* 4. MENU holds until confirm; ESC returns to ATTRACT. */
    {
        InputState idle = make_input();
        GameFlow s = FLOW_MENU;
        s = gameflow_step(s, &idle);
        bad |= expect_state(s, FLOW_MENU, 0, "menu holds");
        InputState esc = make_input(); esc.lastKey = 0x01u;
        s = gameflow_step(s, &esc);
        bad |= expect_state(s, FLOW_ATTRACT, 0, "menu ESC -> attract");
    }

    /* 5. the re-entry: the attract re-arms (game_loop's prologue runs
     *    again — LAB_000E) and plays until the next escape. */
    {
        InputState idle = make_input();
        GameFlow s = FLOW_ATTRACT;
        s = gameflow_step(s, &idle);      /* the restart + the first effect */
        palette_tick();
        bad |= expect_state(s, FLOW_ATTRACT, 0, "re-entry plays");
        unsigned ph = attract_phaseForProbe();
        if (ph == ATTRACT_DONE || ph == ATTRACT_IDLE) {
            fprintf(stderr, "FAIL: the re-entry did not re-arm (phase %u)\n", ph);
            bad = 1;
        }
        InputState fire = make_input(); fire.joyBits = 0x80u;
        s = gameflow_step(s, &fire);      /* escape again */
        palette_tick();
        unsigned guard = 0;
        while (s == FLOW_ATTRACT && guard++ < 200u) {
            InputState idle2 = make_input();
            s = gameflow_step(s, &idle2);
            palette_tick();
        }
        bad |= expect_state(s, FLOW_MENU, 0, "second escape -> menu");
    }

    /* 6. MENU + fire (+0x5ACE bit 7) enters LEVEL. */
    {
        InputState fire = make_input(); fire.joyBits = 0x80u;
        GameFlow s = FLOW_MENU;
        s = gameflow_step(s, &fire);
        bad |= expect_state(s, FLOW_LEVEL, 0, "menu fire -> level");
        s = gameflow_step(s, &fire);        /* level holds */
        bad |= expect_state(s, FLOW_LEVEL, 0, "level holds");
    }

    /* 7. the full-game_main smoke test: tests/test_smoke.c drives the
     * flow through the null backend's scripted-input hook. */

    if (!bad) printf("flow tests: OK (attract cycle + escape + menu)\n");
    return bad;
}
