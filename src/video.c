/* video.c - pages, scroll, dirty cells
 *
 * The core owns two abstract 320x200x8 pages; mode-X plane quirks stay in the
 * core's blitters, the backend sees flat pages.
 * 
 * The page MEMORY is the backend's (OSystem::videoPage): the core only
 * blits into it.
 */
#include "video.h"
#include "osystem.h"

static const OSystem *_video_osys;

int32_t video_init(const OSystem *osys)
{
    _video_osys = osys;
    return 0;
}

void video_tick(void) { /* Nothing to do here... */ }

uint8_t *video_page(int32_t which) { return _video_osys->videoPage(which); }

void video_set_mode(VideoMode mode) { _video_osys->videoSetMode(mode); }
