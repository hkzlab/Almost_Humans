/* load_sprite_pal's LOADING screen (1000:1DC0): the walkers and the
 * falling letters.
 */

#ifndef HUMANSRE_LEVEL_LOADING_H
#define HUMANSRE_LEVEL_LOADING_H

#include <stdint.h>

/** @brief level_run (1000:3054) runs level_video_init's two fades (3083) BEFORE
 * load_sprite_pal (3086), so the loading screen exists only once the chain has
 * poured the palette to black.
 *
 * load_sprite_pal's LOADING screen (1000:1DC0): ten walkers for 0xA0 frames,
 * then the "LOADING!" letters for 0x56.
 * level_loading_screen() starts it and reports whether it is showing;
 * level_loading_tick draws one frame.
 *
 * @return 1 while the loading screen is up, 0 otherwise
 */
uint8_t level_loading_screen(void);

/** @brief particle_frame (1000:1E78) + update_particles (1000:1E98): one frame of
 * the list - the counter reload, the per-entry step and sprite cycle - then
 * the draw and the present.
 */
void level_loading_tick(void);

#endif /* HUMANSRE_LEVEL_LOADING_H */
