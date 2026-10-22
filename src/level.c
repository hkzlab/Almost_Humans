/* level.c — headers/maps/defs (game_structure.md §1 subsystem 9,
 * re_formats.md §2): LEVELS\L?.BIN (13-B header + 640-B map), BD?.BIN. */
#include "game.h"

int level_init(const OSystem *osys) { (void)osys; return 0; }
int level_load(unsigned levelIndex) { (void)levelIndex; return 0; }
