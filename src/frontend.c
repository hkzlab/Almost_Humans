/* frontend.c — the map/menu, the code entry, the name entry, the hi-scores
 * (game_structure.md §1 subsystem 8, re_formats.md §10).  The level
 * selection feeds game_main's level_current; the stub hardcodes 0. */
#include "game.h"

int frontend_init(const OSystem *osys) { (void)osys; return 0; }
int frontend_tick(void) { return 0; }
