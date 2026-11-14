/* The MAP screen after the ending: menu_loop and its letter wheel.
 *
 *   menu_loop               1000:012A
 *   expand_nibbles_4plane   1000:0705   apl\alpha.spr into four planes
 *   rotate_spinner_cell     1000:0F93
 *   init_display_lists      1000:0CBE   the 30-cell ring
 *   draw_score_block        1000:0651   the top ten
 *   the band steps          1000:09DA / 12D3
 *
 * The declarations are split into one header per theme; this header re-exports
 * them for the rest of the port.
 */

#ifndef HUMANSRE_MAP_H
#define HUMANSRE_MAP_H

#include "map/map_flow.h"  // IWYU pragma: export
#include "map/map_wheel.h" // IWYU pragma: export
#include "map/map_band.h"  // IWYU pragma: export
#include "map/map_list.h"  // IWYU pragma: export

#endif /* HUMANSRE_MAP_H */
