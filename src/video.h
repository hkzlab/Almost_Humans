/* video.h — the interface of src/video.c: pages/scroll/dirty cells (subsystem 3, §5/§7) */

#ifndef HUMANSRE_VIDEO_H
#define HUMANSRE_VIDEO_H

#include "osystem.h"

/**
 * @brief Bind the video module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t video_init(const OSystem *osys);

/**
 * @brief The video module's per-tick hook (nothing to do).
 */
void video_tick(void);

/**
 * @brief Accessor: one 320x200x8 display page.
 *
 * @param which the page selector (0 or 1)
 * @return the selected display page
 */
uint8_t *video_page(int32_t which);

/** @brief The mode sets the core performs: the CRTC field the backend must present.
 * The pages stay 320x200x8; a mode-X screen's rows 192..199 are not shown.
 *
 * the mode set, forwarded to the backend (osystem.h's VideoMode): the page
 * layout never changes, only which field the display scans out.
 *
 * @param mode the mode
 */
void video_set_mode(VideoMode mode);

#endif /* HUMANSRE_VIDEO_H */
