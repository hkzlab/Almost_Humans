/* video.h - the interface of src/video.c: pages/scroll/dirty cells
 * 
 * It's mostly an empty shell that interfaces to the backend code.
 */

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
 * @brief The video module's per-tick hook.
 */
void video_tick(void);

/**
 * @brief Accessor: one 320x200x8 display page.
 *
 * @param which the page selector (0 or 1)
 * @return the selected display page
 */
uint8_t *video_page(int32_t which);

/** @brief The game will switch video modes (see the VideoMode enum) while
 *  switching between different screens. This will notify the backend.
 *
 * The page layout never changes, only which field the display scans out.
 *
 * @param mode the mode
 */
void video_set_mode(VideoMode mode);

#endif /* HUMANSRE_VIDEO_H */
