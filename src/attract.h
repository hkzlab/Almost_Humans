/* The ATTRACT loop: game_loop and its four intro effects, cycled 1-3-2-4.
 *
 *   game_loop      1000:131E   the machine (attract_tick)
 *   attract_exit   1000:13E9   the escape and the 13B2 fade
 *   effects        1000:1406 / 1514 / 18CA / 19AF
 *
 * Deviations: D-19 (no-data load skip), D-262 (row 199 spill dropped).
 */

#ifndef HUMANSRE_ATTRACT_H
#define HUMANSRE_ATTRACT_H

#include "osystem.h"

/* The machine's phases - public for the flow tests (the cycle-order and
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

/* [0x1E7C] and [0xD3D] are not carried: in the decoded image they are
 * write-only (the cycle/effect tag and the skip flag; no reader is traced). */

/**
 * @brief Attract module init (the backend is bound once by osys_bind()).
 *
 * @return 0 on success
 */
int32_t attract_init(void);

/**
 * @brief Release the attract's heap blocks (the effect images, the 192-KB
 * credit strip and the published font).  The escape path calls it, and so does
 * the session teardown.
 */
void attract_shutdown(void);

/**
 * @brief One attract tick; 1 = still playing.
 *
 * @param in the input state
 * @return 1 while the attract cycle is playing
 */
uint8_t attract_tick(const InputState *in);

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */

/**
 * @brief Accessor: the attract phase (the tests' phase read).
 *
 * @return the phase enum
 */
uint32_t attract_phase_for_probe(void);

/** @brief Effect 4's strip probe: the 192-KB work area ([0xD76]) and the rows
 * built ([0xD39]).
 *
 * The byte-exact fixture test compares the fill against the reference dump.
 * Valid from effect 4's load until the effect ends (ATTRACT_E4_OUT releases
 * the strip).
 *
 * @param rowsBuilt the output row count
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *attract_strip_for_probe(uint32_t *rowsBuilt);

/** @brief Test seam: the attract's decimal formatter (1000:0AEC).
 *
 * The digits are built LSB-first and reversed once, so every value prints in
 * order.
 *
 * @param v the value
 * @param out the output buffer (at least 8 bytes)
 */
void attract_format_decimal_for_probe(uint32_t v, char *out);

#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_ATTRACT_H */
