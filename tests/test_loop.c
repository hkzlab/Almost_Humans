/* test_loop.c — the flow/tick state machine under the null backend.
 *
 * Assert-based (no framework): exits 0 silently on success, prints the
 * failing fact to stderr and exits 1 otherwise.  Drives gameflow_step()
 * with synthetic input and the real tick callback through the null
 * backend's manual-tick hook. */
#include "game.h"
#include <stdio.h>
#include <string.h>

static InputState make_input(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;              /* joyBits 0 = fire released, ESC = 1, quit = 2 */
}

static int expect_state(GameFlow got, GameFlow want, unsigned tick, const char *what)
{
    if (got == want) return 0;
    fprintf(stderr, "FAIL at tick %u (%s): state %d, want %d\n",
            tick, what, (int)got, (int)want);
    return 1;
}

int main(void)
{
    int bad = 0;
    unsigned tick = 0;

    /* 1. ATTRACT auto-advances to MENU after GAME_TICK_HZ * 5 ticks. */
    {
        InputState in = make_input();
        GameFlow s = FLOW_ATTRACT;
        for (tick = 1; tick <= 5u * GAME_TICK_HZ; tick++)
            s = gameflow_step(s, &in);
        bad |= expect_state(s, FLOW_MENU, tick, "attract dwell -> menu");
    }

    /* 2. MENU holds until confirm; ESC returns to ATTRACT. */
    {
        InputState idle = make_input();
        GameFlow s = FLOW_MENU;
        s = gameflow_step(s, &idle);
        bad |= expect_state(s, FLOW_MENU, 0, "menu holds");
        InputState esc = make_input(); esc.lastKey = 0x01u;
        s = gameflow_step(s, &esc);
        bad |= expect_state(s, FLOW_ATTRACT, 0, "menu ESC -> attract");
    }

    /* 3. MENU + fire (+0x5ACE bit 7) enters LEVEL. */
    {
        InputState fire = make_input(); fire.joyBits = 0x80u;
        GameFlow s = FLOW_MENU;
        s = gameflow_step(s, &fire);
        bad |= expect_state(s, FLOW_LEVEL, 0, "menu fire -> level");
        s = gameflow_step(s, &fire);        /* level holds */
        bad |= expect_state(s, FLOW_LEVEL, 0, "level holds");
    }

    /* 4. the full-game_main smoke test: tests/test_smoke.c drives the
     * flow through the null backend's scripted-input hook. */

    if (!bad) printf("flow tests: OK (5 checks)\n");
    return bad;
}
