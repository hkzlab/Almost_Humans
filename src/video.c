/* video.c — pages, scroll, dirty cells (game_structure.md §1 subsystem 3,
 * re_formats.md §5/§7).  The core owns two abstract 320x200x8 pages; mode-X
 * plane quirks stay in the core's blitters, the backend sees flat pages.
 * The page MEMORY is the backend's (OSystem::videoPage) — the core only
 * blits into it. */
#include "video.h"
#include "osystem.h"

static const OSystem *_video_osys;

/** @brief Bind the video module to the backend. */
int32_t video_init(const OSystem *osys)
{
    _video_osys = osys;
    return 0;
}

/** @brief The video module's per-tick hook (nothing to do). */
void video_tick(void) {}

/** @brief Accessor: one display page. */
uint8_t *video_page(int32_t which) { return _video_osys->videoPage(which); }

/** The mode sets the core performs: the CRTC field the backend must present.
 * The pages stay 320x200x8; a mode-X screen's rows 192..199 are not shown. */
void video_set_mode(VideoMode mode) { _video_osys->videoSetMode(mode); }
