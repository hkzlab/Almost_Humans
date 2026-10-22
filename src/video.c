/* video.c — pages, scroll, dirty cells (game_structure.md §1 subsystem 3,
 * re_formats.md §5/§7).  The core owns two abstract 320x200x8 pages; mode-X
 * plane quirks stay in the core's blitters, the backend sees flat pages. */
#include "game.h"

int video_init(const OSystem *osys) { (void)osys; return 0; }
void video_tick(void) {}
