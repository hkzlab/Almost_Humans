/* video.c — pages, scroll, dirty cells (game_structure.md §1 subsystem 3,
 * re_formats.md §5/§7).  The core owns two abstract 320x200x8 pages; mode-X
 * plane quirks stay in the core's blitters, the backend sees flat pages.
 * The page MEMORY is the backend's (OSystem::videoPage) — the core only
 * blits into it. */
#include "video.h"
#include "object_ds.h"
#include "osystem.h"

static const OSystem *video_osys;

int32_t video_init(const OSystem *osys)
{
    video_osys = osys;
    return 0;
}

void video_tick(void) {}

uint8_t *video_page(int32_t which) { return video_osys->videoPage(which); }

/* The mode sets the core performs: the CRTC field the backend must present.
 * The pages stay 320x200x8; a mode-X screen's rows 192..199 are not shown. */
void video_setMode(VideoMode mode) { video_osys->videoSetMode(mode); }

/* The CRTC page-flip bytes [0x4105..0x410B] as the present path consumes
 * them: [0x4106] is the pending start word, [0x4108] its consumed copy,
 * [0x410A] the polarity byte latched into [0x410B] when [0x4105] is set.
 * The port has one composed page; a nonzero start presents page 1, as a
 * corrupted original start address would show the other field. */
int32_t video_dsPresentPage(void)
{
    uint16_t start =
        (uint16_t)(objDsImage[0x4106] | ((uint16_t)objDsImage[0x4107] << 8));

    objDsImage[0x4108] = objDsImage[0x4106];
    objDsImage[0x4109] = objDsImage[0x4107];
    if (objDsImage[0x4105]) objDsImage[0x410B] = objDsImage[0x410A];
    return start ? 1 : 0;
}
