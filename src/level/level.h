/* The level loader and runtime: LEVELS\L*.BIN, the BD*.BIN sets, the view.
 *
 *   load_level          1000:30BC   the record and the 640-byte map
 *   level_video_init    1000:32CD   the level's mode and palette
 *   level_fade_in       1000:3334   the start chain
 *   load_level_assets   1000:3468   the per-style visual set
 *   level_run           1000:3054   the spine (game_main's FLOW_LEVEL)
 *
 * Deviations: D-25 (out-of-table follower pair), D-126 (>= 0x40 bytes kept),
 * D-128 (missing-volume animation not drawn).
 *
 * The declarations are split into one header per theme; this header re-exports
 * them for the rest of the port.
 */

#ifndef HUMANSRE_LEVEL_H
#define HUMANSRE_LEVEL_H

#include "level/level_load.h" // IWYU pragma: export
#include "level/level_assets.h" // IWYU pragma: export
#include "level/level_play.h" // IWYU pragma: export
#include "level/level_view.h" // IWYU pragma: export
#include "level/level_tribe.h" // IWYU pragma: export
#include "level/level_palette.h" // IWYU pragma: export
#include "level/level_loading.h" // IWYU pragma: export
#include "level/level_spark.h" // IWYU pragma: export
#include "level/level_disc.h" // IWYU pragma: export

#endif /* HUMANSRE_LEVEL_H */
