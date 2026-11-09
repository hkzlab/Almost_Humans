/* attract.h — the interface of src/attract.c: the ATTRACT loop (game_loop 1000:131E)
 *
 * The four intro effects in the original's order (1 -> 3 -> 2 -> 4),
 * cycled forever; the only exit is attract_exit (1000:13E9): the
 * configured action key ([0x5ACE] bit 7) or any held key ([0x48A3]),
 * which runs the 13B2 escape — the page pair restored, one fade to black
 * whose completion hands control to the front-end.  Per tick, exactly like
 * the original's wait_vsync cadence: attract_tick() advances the machine
 * by one display period, composes the visible planar window into page 0
 * (game_main's videoPresent shows it) and returns 1 while the attract
 * plays, 0 once the escape's fade completed.  The details and the one
 * documented latency deviation are in attract.c's header and
 * port_deviations.md.
 */

#ifndef HUMANSRE_ATTRACT_H
#define HUMANSRE_ATTRACT_H

#include "osystem.h"

/* The machine's phases — public for the flow tests (the cycle-order and
 * escape assertions in tests/test_loop.c). */
typedef enum AttractPhase {
    ATTRACT_IDLE = 0,
    ATTRACT_E1_LOAD,
    ATTRACT_E1_FADEWAIT,
    ATTRACT_E1_WIPE,
    ATTRACT_E1_HOLD,
    ATTRACT_E1_OUT,
    ATTRACT_E3_LOAD,
    ATTRACT_E3_FADEWAIT,
    ATTRACT_E3_SLIDE,
    ATTRACT_E3_TAIL,
    ATTRACT_E3_HOLD,
    ATTRACT_E3_OUT,
    ATTRACT_E2_LOAD,
    ATTRACT_E2_FADEWAIT,
    ATTRACT_E2_FRAME,
    ATTRACT_E2_WIPE,
    ATTRACT_E2_HOLD,
    ATTRACT_E2_OUT,
    ATTRACT_E4_LOAD,
    ATTRACT_E4_FADEWAIT,
    ATTRACT_E4_SCROLL,
    ATTRACT_E4_OUT,
    ATTRACT_ESCAPE,
    ATTRACT_DONE
} AttractPhase;

/** @brief [0x1E7C] and [0xD3D] are not carried: in the decoded image they are
 * write-only (the cycle/effect tag and the skip flag — the writers are in
 * re_map.md § "The ATTRACT loop decoded"; no reader is traced), so the
 * port records their semantics in comments instead of dead state.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t attract_init(const OSystem *osys);

/**
 * @brief One attract tick; 1 = still playing.
 *
 * @param in the input state
 * @return 1 while the attract cycle is playing
 */
uint8_t attract_tick(const InputState *in);

/**
 * @brief Accessor: the attract phase (the tests' phase read).
 *
 * @return The attract phase.
 */
uint32_t attract_phase_for_probe(void);

/** @brief the strip probe: the 192-KB work area and [0xD39] (the rows built) —
 * the byte-exact fixture test compares these against the sim's dump.
 *
 * Effect 4's strip probe (the 192-KB work area at [0xD76] + the rows built,
 * [0xD39]): the byte-exact fixture test compares the fill against
 * tools/attract_scroll_sim.py's dump.
 * Valid from the first ATTRACT_E4 load until the next effect begins.
 *
 * @param rowsOut the output row count
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *attract_strip_for_probe(uint32_t *rowsBuilt);

/** @brief Test seam: the attract's decimal formatter (1000:0AEC).
 *
 * Test seam: the attract's decimal formatter (1000:0AEC); the digits are built
 * LSB-first and reversed once, so every value prints in order.
 *
 * @param v the value
 * @param out the output buffer (at least 8 bytes)
 */
void attract_format_decimal_for_probe(uint32_t v, char *out);

#endif /* HUMANSRE_ATTRACT_H */
