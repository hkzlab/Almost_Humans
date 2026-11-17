/* Video: the two flat pages and the shared A000 address model.
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
 * @brief Video module init (the backend is bound once by osys_bind()).
 *
 * @return 0 on success
 */
int32_t video_init(void);

/** @brief Release the shared text arena.  Every screen that uses it calls
 * this when it exits; the next user allocates it again.
 */
void video_arena_release(void);

/**
 * @brief The video per-tick hook: empty, the flip is video_vram_present.
 */
void video_tick(void);

/**
 * @brief Accessor: one display page (the active size, video_page_w() by
 * video_page_h(), 8bpp).  Handing out page 0 marks the frame's surface as the
 * display page rather than the logical screen.
 *
 * @param which the page selector (0 or 1)
 * @return the selected display page
 */
uint8_t *video_page(int32_t which);

/**
 * @brief Accessor: the logical 320x200 screen the original draws on.
 *
 * Every screen that is not the level draws here.  At scale 1 it *is* page 0;
 * at scale 2 it is a separate buffer that video_present_frame() expands into
 * page 0, so those screens look exactly as they do at scale 1.  A
 * HUMANSRE_VIEW_SCALE=1 build compiles the buffer out.
 *
 * @return the logical screen
 */
uint8_t *video_screen(void);

/**
 * @brief Accessor: the view scale, 1 (the original's framing) or 2.
 *
 * @return the scale
 */
int32_t video_scale(void);

/**
 * @brief Accessor: the display page's active width (320 or 640).
 *
 * @return the width in pixels
 */
uint32_t video_page_w(void);

/**
 * @brief Accessor: the display page's active height (200 or 400).
 *
 * @return the height in rows
 */
uint32_t video_page_h(void);

/**
 * @brief Present this frame: expand the logical screen into page 0 first when
 * that was the surface drawn this frame (scale 2 only), then present page 0
 * as the current field.
 *
 * At scale 1 it is the plain videoPresent(0) the original's CRTC flip is.
 */
void video_present_frame(void);

/**
 * @brief Set the field's scan-out rows for the current screen.
 *
 * Defaults to the mode's field (192 * scale in mode X, 200 * scale in mode
 * 13h) and is reset by video_set_mode.  The level sets it to its own field
 * (`level_field_rows()`): the whole 400-row page at scale 2, because its bar
 * ends at the page's bottom.  A no-op in a HUMANSRE_VIEW_SCALE=1 build.
 *
 * @param rows the field rows
 */
void video_set_field(uint32_t rows);

/**
 * @brief Accessor: the field's scan-out rows for the current screen.
 *
 * @return the rows
 */
uint32_t video_field_rows(void);


/**
 * @brief The four pixels at one A000 address.
 *
 * A000 is the original's 64-KB unchained byte-mode window: one address byte
 * is four pixels, one per plane.  The port stores them flat, byte 4*addr + p.
 *
 * @param addr the address (0..0xFFFF)
 * @return the four pixels; plane p is the pixel at x = 4j + p
 */
uint8_t *video_vram_group(uint16_t addr);

/**
 * @brief _clear_vram (1000:833A): map mask 0xF, 64 KB zeroed.
 */
void video_vram_clear(void);

/**
 * @brief blit_planar_to_vga (1000:888E), deplanarise_intro_page (1000:2ED7)
 * and blit_planar_strip (1000:AEAD): blit chunky rows into the A000 window.
 *
 * Row r lands at base + r*pitch (addresses, 4 pixels each); group c is
 * src[r*cols*4 + c*4 .. +3].  Bytes at or past `size` (a loaded chunk, whose
 * tail may be a palette) are written as zeros.  Installs a resource image and
 * copies a whole composed page between surfaces.
 *
 * @param src the chunky source
 * @param size the source's usable byte count
 * @param base the destination base address
 * @param pitch the destination row pitch in address bytes
 * @param cols the 4-pixel groups per row
 * @param rows the row count
 */
void video_vram_blit(const uint8_t *src, uint32_t size, uint16_t base,
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
 * chunky frame there before blitting it into A000.
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

/** @brief Set the mode: the backend scans out the new field.
 *
 * The page layout never changes, only which field the display scans out.
 *
 * @param mode the mode
 */
void video_set_mode(VideoMode mode);

#endif /* HUMANSRE_VIDEO_H */
