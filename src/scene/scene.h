/* The .ANI/.RSC interpreter: the display list, the frame-keyed cues and the
 * scene chain.
 *
 *   cd_file_op              1000:AFF8   one level intro, per tick
 *   blit_background_data    1000:B2EB   the AL expander
 *   blit_display_list       1000:B955
 *   run_screen_transition   1000:B82E
 *   boot_intro_sequence     1000:B590   the boot intro
 *
 * Deviations: D-248 (the evolution scene waits 4 ticks, not 2).
 *
 * The declarations are split into one header per theme; this header re-exports
 * them for the rest of the port.
 */

#ifndef HUMANSRE_SCENE_H
#define HUMANSRE_SCENE_H

#include "scene/scene_play.h" // IWYU pragma: export
#include "scene/scene_boot.h" // IWYU pragma: export

#endif /* HUMANSRE_SCENE_H */
