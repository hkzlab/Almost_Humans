/* The level-intro party/briefing screen.
 *
 *   level_intro_screen   1000:2508   the machine
 *   draw_intro_frame     1000:2C1D   the masked band bake
 *   the animator         1000:28E9   the 8-slot cell scripts
 *   fade_and_animate     1000:2BDA   the text/background fade cycle
 *
 * Deviation: D-19 (a failed load skips the screen).
 */

#ifndef HUMANSRE_PARTY_INTRO_H
#define HUMANSRE_PARTY_INTRO_H

#include "osystem.h"

/**
 * @brief Set the briefing's GAME OVER variant.
 *
 * level_intro_screen 2739 (need > members): the GAME OVER briefing variant
 * (the same art; GAME OVER / YOUR TRIBE IS TOO SMALL / IN THE RACE / PRESS
 * FIRE, one key/fire exit).
 *
 * @param on the on/off flag
 */
void party_intro_set_game_over(uint8_t on);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: the briefing's score (g_score [0x592]).
 *
 * @return the score
 */
uint16_t party_intro_score(void);
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief Seed the briefing's level, counts, score and tribe name.
 *
 * @param level the level number
 * @param need the required count
 * @param members the member count
 * @param score the score
 * @param tribe the tribe name
 */
void party_intro_set_values(uint16_t level, uint16_t need, uint16_t members,
                            uint16_t score, const char *tribe);

/**
 * @brief Load and start the party briefing screen; 1 = playing.
 *
 * @return 1 while the briefing is playing
 */
uint8_t party_intro_start(void);

/** @brief the game_main hook's per-tick compose into the flat page (the attract's
 * contract: the tick draws, videoPresent(0) shows)
 *
 * @param in the input state
 * @return 1 while the briefing is playing, 0 when it exits
 */
uint8_t party_intro_tick(const InputState *in);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief 2B1B format_score + 2B70: format the score as a decimal string.
 *
 * @param v the value
 * @param out the output buffer
 */
void party_intro_format_score(uint32_t v, char *out);
#endif /* HUMANSRE_TEST_PROBES */

/** @brief the closed layout law, exposed for the byte-exact fixtures: the page's
 * pixel (x,y) = video_vram_group(pagebase + 8 + y*88 + (x>>2))[x&3] of the
 * DISPLAY page (_page_disp - the just-flipped bake target).
 *
 * @param page320x200 the composed page
 */
void party_intro_compose(uint8_t *page320x200);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief the palette buffers, exposed for the fade-ladder harness: the port's
 * 0x4bbc/0x4d3c(+0x180)/0x1208 stand-ins (read-only diagnostics, see
 * test_fadeladder)
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *party_intro_workpal(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: the upper palette half (0x4D3C = 0x4BBC + 0x180).
 *
 * @return the 0x180-byte upper half
 */
const uint8_t *party_intro_pal_upper(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief 2a76..2a95: the marquee's source offset and destination row for a
 * phase/cell pair (the phase is the 0x160 word [0xf9e]).
 *
 * @param phase the [0xf9e] phase (0..0x15F)
 * @param cell the [0xf9c] cell (0..0xB)
 * @param src the source offset (out)
 * @param dhi the destination high byte (out)
 */
void party_intro_marquee_for_probe(uint16_t phase, uint16_t cell, uint16_t *src,
                                   uint16_t *dhi);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_PARTY_INTRO_H */
