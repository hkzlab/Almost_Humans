/* The level cutscene player: cd_file_op's ANI sequence, the panel and the
 * scene chain (1000:AFF8/B07B/B157/B263).
 */

#ifndef HUMANSRE_SCENE_PLAY_H
#define HUMANSRE_SCENE_PLAY_H

#include <stdint.h>

/**
 * @brief Scene module init (the backend is bound once by osys_bind()).
 *
 * @return 0 on success
 */
int32_t scene_init(void);

/**
 * @brief Play one level's .ANI newspaper for a frame (the cd_file_op body).
 *
 * @param introIndex the level header's +0x01 byte: the intro-movie index (0..9;
 *                  0xFF = the level has no cutscene)
 * @return 1 while the newspaper is still showing
 */
uint8_t scene_play_level_intro(uint16_t introIndex);

/**
 * @brief Force the next scene_play_level_intro call to reload its .ANI.
 *
 * cd_file_op always reloads, so two consecutive levels with the same intro
 * index still get a new scene; the per-tick player would otherwise keep the
 * finished one finished.
 */
void scene_reset_intro(void);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test seam: skip the intro's per-frame waits ([0x627C]).
 *
 * boot_intro_sequence's fast mode: the scene player fades 4 palette entries
 * div 2 instead of 0x100 div 0x19 and waits 1 tick per object instead of 6.
 *
 * @param on the on/off flag
 */
void scene_set_fast_mode(uint8_t on);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_SCENE_PLAY_H */
