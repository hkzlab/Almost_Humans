/* video.h — the interface of src/video.c: pages/scroll/dirty cells (subsystem 3, §5/§7) */

#ifndef HUMANSRE_VIDEO_H
#define HUMANSRE_VIDEO_H

#include "osystem.h"

int32_t video_init(const OSystem *osys);
void video_tick(void);
uint8_t *
video_page(int32_t which); /* 320x200x8 page 0 or 1                    */
/* the mode set, forwarded to the backend (osystem.h's VideoMode): the page
 * layout never changes, only which field the display scans out. */
void video_setMode(VideoMode mode);
/* the [0x4105..0x410B] page-flip bytes, applied by the present path */
int32_t video_dsPresentPage(void);

#endif /* HUMANSRE_VIDEO_H */
