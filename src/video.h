/* Video: the two flat pages and the shared A000 planar model.
 *
 *   vga_set_mode_13h        1000:8300
 *   reset_display_mode      1000:82C6
 *   clear_vram              1000:833A
 *   begin_page_flip         1000:8821
 *   set_display_ptr         1000:882D
 *   blit_planar_to_vga      1000:888E
 *   deplanarise_intro_page  1000:2ED7
 *   blit_planar_strip       1000:AEAD
 *
 * Deviation: D-04 (flat 320x200 pages, no mode-X planes).
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

/**
 * @brief The four plane bytes at one A000 address.
 *
 * A000 is the original's 64-KB unchained byte-mode window: one address byte
 * is four pixels, one per plane.
 *
 * @param addr the address (0..0xFFFF)
 * @return the four plane bytes; plane p is the pixel at x = 4j + p
 */
uint8_t *video_vram_group(uint16_t addr);

/**
 * @brief _clear_vram (1000:833A): map mask 0xF, 64 KB zeroed.
 */
void video_vram_clear(void);

/**
 * @brief blit_planar_to_vga (1000:888E), deplanarise_intro_page (1000:2ED7)
 * and blit_planar_strip (1000:AEAD): planarise chunky rows into A000.
 *
 * Row r lands at base + r*pitch; column c at that row's address byte c; plane
 * p is src[r*cols*4 + c*4 + p].  Bytes at or past `size` (a loaded chunk,
 * whose tail may be a palette) are written as zeros.
 *
 * @param src the chunky source
 * @param size the source's usable byte count
 * @param base the destination base address
 * @param pitch the destination row pitch in address bytes
 * @param cols the 4-pixel groups per row
 * @param rows the row count
 */
void video_vram_planarise(const uint8_t *src, uint32_t size, uint16_t base,
                          uint16_t pitch, uint16_t cols, uint16_t rows);

/**
 * @brief VGA write mode 2 at one A000 address: plane p keeps the bits outside
 * `mask` and takes bit p of `colour` inside it.
 *
 * @param addr the address
 * @param colour the 4-bit colour
 * @param mask the bit mask
 */
void video_vram_wm2(uint16_t addr, uint8_t colour, uint8_t mask);

/**
 * @brief Compose the 200-row display window into a flat page.
 *
 * Rows below `split` show the page at `base`, panned by `scroll` pixels (the
 * CRTC start); rows at or above `split` show the fixed band from absolute
 * address 0, unpanned.  game_main's per-tick videoPresent(0) shows page 0.
 *
 * @param page the flat 320x200 page
 * @param base the displayed page's base address
 * @param pitch the row pitch in address bytes
 * @param scroll the pixel pan
 * @param split the CRTC split row (VIDEO_PAGE_H_MODE13H for no split)
 */
void video_vram_present(uint8_t *page, uint16_t base, uint16_t pitch,
                        uint16_t scroll, uint16_t split);

/**
 * @brief The shared flat 320x200x8 scratch page.
 *
 * It is the backend's page 1, which the port never presents: page 0 is the
 * display contract and page 1 is core scratch.  The attract composes its
 * chunky frame there before planarising it.
 *
 * @return the scratch page
 */
uint8_t *video_flat(void);

/**
 * @brief The shared 200-by-80 text arena (80-byte rows).
 *
 * The attract's draw arena (DS:0xC9D) and the front-end/party arena
 * (DS:0x1E20) are different original buffers that never coexist; the port
 * keeps one flat scratch for both, the modules being mutually exclusive.
 *
 * @return the arena
 */
uint8_t *video_arena(void);

/** @brief The game will switch video modes (see the VideoMode enum) while
 *  switching between different screens. This will notify the backend.
 *
 * The page layout never changes, only which field the display scans out.
 *
 * @param mode the mode
 */
void video_set_mode(VideoMode mode);

#endif /* HUMANSRE_VIDEO_H */
