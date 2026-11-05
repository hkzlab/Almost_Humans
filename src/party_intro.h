/* party_intro.h — the interface of src/party_intro.c: the level-intro party screen (subsystem 14)
 *
 * level_intro_screen (1000:2508): the fresh-load briefing.  The screen is
 * reached from FLOW_LEVEL's fresh-load path (level_run's 307a call; the
 * .ANI cutscene is not part of this screen — see next_step.md's note).
 * party_intro_start seeds and runs the setup (the 5 asset loads, the
 * double .BIT bake with the flip, the master-palette copy, the 35-step
 * fade-in); party_intro_tick is one display period of the machine, and
 * returns 1 while the screen plays, 0 after its exit's music_stop.
 * A headless run without the disc data skips the screen (start returns 0).
 *
 * Facts: re_map.md → "The level-intro party screen decoded end to end
 * (2026-10-29)" (all render gates closed); the memory model and the
 * machine notes are in party_intro.c's header.
 */

#ifndef HUMANSRE_PARTY_INTRO_H
#define HUMANSRE_PARTY_INTRO_H

#include "osystem.h"

/* level_intro_screen 2739 (need > members): the GAME OVER briefing variant
 * (the same art; GAME OVER / YOUR TRIBE IS TOO SMALL / IN THE RACE / PRESS
 * FIRE, one key/fire exit). */
void party_intro_set_game_over(int32_t on);
uint32_t party_intro_score(void); /* g_score [0x592] */
void party_intro_set_values(uint32_t level, uint32_t need, uint32_t members,
                            uint32_t score, const char *tribe);
void party_intro_seed_scripts(const uint8_t *bytes, uint32_t len);
void party_intro_seed_slots(const uint8_t *raw); /* the 128 B at DS:0xFA8 */
int32_t party_intro_start(void);
int32_t party_intro_tick(const InputState *in);
void party_intro_format_score(uint32_t v, char *out); /* 2b1b/2b70       */
void party_intro_compose(
    uint8_t *page320x200); /* the law compose for the fixtures */
const uint8_t *
party_intro_workpal(void); /* the 0x4bbc stand-in (diagnostics) */
const uint8_t *
party_intro_pal_upper(void); /* 0x4d3c = 0x4bbc + 0x180 (diagnostics) */

#endif /* HUMANSRE_PARTY_INTRO_H */
