/* test_loop.c - the flow/tick state machine under the null backend.
 *
 * Assert-based (no framework): exits 0 silently on success, prints the
 * failing fact to stderr and exits 1 otherwise.
 *
 * With the real attract loop in place (game_loop 1000:131E), FLOW_ATTRACT
 * no longer auto-advances: the four-effect cycle runs until the escape
 * (attract_exit 1000:13E9 - the action key or any held key), whose final
 * fade completes inside attract_tick.  Without the game data the effects
 * skip (the headless pattern), so the cycle spins and the phase probe can
 * still assert the 1 -> 3 -> 2 -> 4 order; palette_tick() is driven by
 * hand, once per flow step, like game_main's game_tick.  The test therefore
 * chdirs into an empty scratch directory first - a build tree that carries
 * HUMANS.00N links must not turn this into the real, multi-hundred-tick
 * playback (test_attract and test_smoke drive that path). */
#include "attract.h"
#include "font.h"
#include "gameflow.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "video.h"
#include "compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const OSystem g_osys_null;

/**
 * @brief A zeroed input state (no key, fire released).
 *
 * @return the zeroed input state
 */
static InputState _make_input(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;              // joyBits 0 = fire released, lastKey 0 = none
}

/**
 * @brief Compare a flow state with the wanted one and report a miss.
 *
 * @param got the observed value
 * @param want the wanted value
 * @param tick the tick count
 * @param what the check description
 * @return 0 on a match, 1 on a mismatch
 */
static int32_t _expect_state(GameFlow got, GameFlow want, uint32_t tick,
                             const char *what)
{
    if (got == want) return 0;
    fprintf(stderr, "FAIL at tick %u (%s): state %d, want %d\n", tick, what,
            (int32_t)got, (int32_t)want);
    return 1;
}

/**
 * @brief the effect a phase belongs to: 1, 3, 2, 4 (the enum groups)
 *
 * @param phase the phase
 * @return the effect number (1, 2, 3 or 4), 0 when none
 */
static uint32_t _effect_of(uint32_t phase)
{
    if (phase >= ATTRACT_E1_LOAD && phase <= ATTRACT_E1_OUT) return 1u;
    if (phase >= ATTRACT_E3_LOAD && phase <= ATTRACT_E3_OUT) return 3u;
    if (phase >= ATTRACT_E2_LOAD && phase <= ATTRACT_E2_OUT) return 2u;
    if (phase >= ATTRACT_E4_LOAD && phase <= ATTRACT_E4_OUT) return 4u;
    return 0u;
}

/**
 * @brief Run the attract loop tests.
 *
 * @return 0 on success, 1 on failure
 */
int main(void)
{
    int32_t bad = 0;
    uint32_t tick = 0;

    /* The flow machine is tested WITHOUT game data: the attract's
     * load-failure path is what lets the cycle be probed one effect per
     * tick (test_attract/test_smoke drive the real playback).  A build tree
     * with HUMANS.00N links would otherwise satisfy the loads, so take the
     * test into an empty scratch cwd. */
    static const char nodata[] = "humans_flow_nodata";
    (void)compat_mkdir(nodata);            // already present is fine
    if (compat_chdir(nodata)) {
        perror(nodata);
        return 1;
    }
    /* entry 1000:002B probes volume 0 before the room (draw_status_indicator
     * 1000:1FEF).  A missing volume would (correctly) park the flow on the
     * "PUT THE DISK INTO DRIVE 0" page forever, so the scratch tree needs an
     * (empty) HUMANS.000: the probe passes and the room's own loads still
     * fail, which is the no-data path this test drives. */
    {
        FILE *v = fopen("HUMANS.000", "wb");
        if (v) fclose(v);
    }

    /* the module set the attract needs (game_main's resources_init order,
     * trimmed to the flow-relevant ones) */
    {
        const OSystem *os = &g_osys_null;
        if (res_init(os) || video_init(os) || palette_init(os) ||
            font_init(os) || attract_init(os)) {
            fprintf(stderr, "FAIL: module init\n");
            return 1;
        }
    }

    /* 1. ATTRACT holds on idle - the cycle never auto-advances (the old
     *    5 s dwell is gone; only the escape exits it). */
    {
        InputState in = _make_input();
        GameFlow s = FLOW_ATTRACT;
        for (tick = 1; tick <= 2000u; tick++) {
            s = gameflow_step(s, &in);
            palette_tick();               // the ISR body's fade step
            if (s != FLOW_ATTRACT) {
                fprintf(stderr,
                        "FAIL at tick %u: idle attract left the "
                        "state (%d)\n",
                        tick, (int32_t)s);
                bad = 1;
                break;
            }
        }
    }

    /* 2. the effect order 1 -> 3 -> 2 -> 4: without data each LOAD fails
     *    and the cycle spins one effect per tick, so the phase sequence
     *    observed before each step shows the cycle. */
    {
        InputState in = _make_input();
        GameFlow s = FLOW_ATTRACT;
        uint32_t seq[12];
        uint32_t n = 0, runs = 0;
        uint32_t last = 0;
        for (tick = 0; tick < 16u && runs < 8u; tick++) {
            uint32_t ph = attract_phase_for_probe();
            uint32_t e = _effect_of(ph);
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
         * sequence's start is arbitrary - its structure is not) */
        static const uint32_t after[5] = { 0u, 3u, 4u, 2u, 1u };
        if (runs < 8u) {
            fprintf(stderr, "FAIL: only %u effect runs observed\n", runs);
            bad = 1;
        }
        for (uint32_t i = 0; i + 1 < 8u; i++) {
            if (seq[i] < 1u || seq[i] > 4u || after[seq[i]] != seq[i + 1]) {
                fprintf(stderr,
                        "FAIL: effect order[%u] = %u -> %u, want "
                        "the 1->3->2->4 cycle\n",
                        i, seq[i], seq[i + 1]);
                bad = 1;
                break;
            }
        }
    }

    /* 3. the escape: the action bit latches the 13B2 fade, whose ~35
     *    ticks (div 0x23) end the attract -> MENU. */
    {
        InputState in = _make_input();
        in.joyBits = 0x80u;               // the configured action key
        GameFlow s = FLOW_ATTRACT;
        s = gameflow_step(s, &in);
        palette_tick();
        bad |= _expect_state(s, FLOW_ATTRACT, 0, "escape: fade still running");
        InputState idle = _make_input();
        uint32_t guard = 0;
        while (s == FLOW_ATTRACT && guard++ < 200u) {
            s = gameflow_step(s, &idle);
            palette_tick();
        }
        if (s != FLOW_MENU) {
            fprintf(stderr,
                    "FAIL: the escape never reached MENU "
                    "(state %d after %u ticks)\n",
                    (int32_t)s, guard);
            bad = 1;
        }
    }

    /* 4. the room holds: idle, ESC (clears the name - 0x5F11's act0, no
     *    exit), and the action key (the option cycle; nothing at cell 0)
     *    all stay in MENU. */
    {
        InputState idle = _make_input();
        GameFlow s = FLOW_MENU;
        s = gameflow_step(s, &idle);
        bad |= _expect_state(s, FLOW_MENU, 0, "menu holds");
        InputState esc = _make_input();
        esc.lastKey = 0x1Bu;
        s = gameflow_step(s, &esc);
        bad |=
            _expect_state(s, FLOW_MENU, 0, "menu ESC stays (clears the name)");
        InputState act = _make_input();
        act.joyBits = 0x80u;
        s = gameflow_step(s, &act);
        bad |=
            _expect_state(s, FLOW_MENU, 0, "menu action stays (option cycle)");
    }

    /* 5. the re-entry: the attract re-arms (game_loop's prologue runs
     *    again - LAB_000E) and plays until the next escape. */
    {
        InputState idle = _make_input();
        GameFlow s = FLOW_ATTRACT;
        s = gameflow_step(s, &idle);      // the restart + the first effect
        palette_tick();
        bad |= _expect_state(s, FLOW_ATTRACT, 0, "re-entry plays");
        uint32_t ph = attract_phase_for_probe();
        if (ph == ATTRACT_DONE || ph == ATTRACT_IDLE) {
            fprintf(stderr, "FAIL: the re-entry did not re-arm (phase %u)\n",
                    ph);
            bad = 1;
        }
        InputState fire = _make_input();
        fire.joyBits = 0x80u;
        s = gameflow_step(s, &fire);      // escape again
        palette_tick();
        uint32_t guard = 0;
        while (s == FLOW_ATTRACT && guard++ < 200u) {
            InputState idle2 = _make_input();
            s = gameflow_step(s, &idle2);
            palette_tick();
        }
        bad |= _expect_state(s, FLOW_MENU, 0, "second escape -> menu");
    }

    /* 6. the room's confirm: hold RIGHT - the rails walk 0x1D2 -> 0x208
     *    (cell 6) -> 0x280, the right-edge exit runs the codes.bin match
     *    and the fade, and the flow lands in LEVEL. */
    {
        InputState right = _make_input();
        right.joyBits = 0x08u;
        GameFlow s = FLOW_MENU;
        uint32_t guard = 0;
        do {
            s = gameflow_step(s, &right);
            palette_tick();
        } while (s == FLOW_MENU && guard++ < 400u);
        if (s != FLOW_LEVEL) {
            fprintf(stderr,
                    "FAIL: the right-edge walk never reached LEVEL "
                    "(state %d after %u ticks)\n",
                    (int32_t)s, guard);
            bad = 1;
        }
        s = gameflow_step(s, &right);        // level holds
        bad |= _expect_state(s, FLOW_LEVEL, 0, "level holds");
    }

    /* 7. the full-game_main smoke test: tests/test_smoke.c drives the
     * flow through the null backend's scripted-input hook. */

    if (compat_chdir("..") == 0) (void)compat_rmdir(nodata);

    if (!bad) printf("flow tests: OK (attract cycle + escape + menu)\n");
    return bad;
}
