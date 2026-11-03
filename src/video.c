/* video.c — pages, scroll, dirty cells (game_structure.md §1 subsystem 3,
 * re_formats.md §5/§7).  The core owns two abstract 320x200x8 pages; mode-X
 * plane quirks stay in the core's blitters, the backend sees flat pages.
 * The page MEMORY is the backend's (OSystem::videoPage) — the core only
 * blits into it. */
#include "game.h"

static const OSystem *video_osys;

int32_t video_init(const OSystem *osys)
{
    video_osys = osys;
    return 0;
}

void video_tick(void) {}

uint8_t *video_page(int32_t which)
{
    return video_osys->videoPage(which);
}

/* The mode sets the core performs: the CRTC field the backend must present.
 * The pages stay 320x200x8; a mode-X screen's rows 192..199 are not shown. */
void video_setMode(VideoMode mode)
{
    video_osys->videoSetMode(mode);
}
