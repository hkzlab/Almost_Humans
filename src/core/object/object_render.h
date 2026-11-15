/* The object and terrain renderers
 */

#ifndef HUMANSRE_OBJECT_RENDER_H
#define HUMANSRE_OBJECT_RENDER_H

#include <stdint.h>


/**
 * @brief Publish the G sprite blocks.
 *
 * DEFS field G: the per-level 3-layer 0x100 sprites the frame slots 0xC1+4L+i
 * point at (level_setup_screen writes their far pointers into DS:0x3CAC).
 *
 * @param data the data buffer
 * @param blocks the block count
 * @param n the sprites per layer
 */
void object_render_publish_g_sprites(const uint8_t *data, uint16_t blocks,
                                     uint8_t n);


/**
 * @brief Publish the DEH sprite banks.
 *
 * DEFS fields E and H: the 0xA9 log sprite (frames 0x33..0x37) and the two
 * sprites the frame slots 0x26/0x27 point at.
 *
 * @param e the E sprite bank
 * @param h the H sprite bank
 */
void object_render_publish_deh_sprites(const uint8_t *e, const uint8_t *h);


/** @brief The one blitter, for the entity pass (1000:5C5F -> 1000:6C74): the same law,
 * mode 2's 32-px cells.
 *
 * @param page the page buffer
 * @param src the source buffer
 * @param rows the row count
 * @param cols the column count
 * @param x the x coordinate
 * @param y the y coordinate
 */
void object_render_blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                               uint32_t cols, int32_t x, int32_t y);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: one terrain slot.
 *
 * @param i the index
 * @return the slot, or NULL past the two
 */
uint8_t *object_render_ter_slot_at(uint32_t i);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief Publish the terrain bank.
 *
 * @param bank the sprite bank
 * @param size the size
 */
void object_render_publish_ter(const uint8_t *bank, uint32_t size);


/**
 * @brief 1000:5C2F + the 0x649A loop of 1000:5823.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void object_render_ter(uint8_t *page, int32_t scrollX, int32_t scrollY);


/**
 * @brief 1000:C7A0/0xC7A9: the two TER slots' patrol, once per world frame.
 *
 * A slot with +0x10 bit 15 set is disabled.  Otherwise +0/+2 steps one unit
 * toward +4/+6 (or +8/+0xA while +0x10 is 0); on arrival +0x10 toggles its
 * low byte, so the slot patrols the two cells the 0xAF+0xB0 (or 0xB1+0xB2)
 * tiles name.  +0xE reloads every 6 frames and bumps +0xC, which is both that
 * counter and the TER sprite index (0x2D7E).  The (new - old) delta is added
 * to the eight static 0x3E75 records and the 0x6F22 groups under the
 * byte-swapped +0x12 mask (0x80>>i / 1<<(7-index)); that byte is also the
 * stack mask 0x64AD/0x64C1, so a riding human moves with the platform.
 */
void object_render_camera_slots_tick(void);


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief Accessor: the frame slot count (the 0x11E sprite-table bound).
 *
 * @return the count
 */
uint32_t object_render_frame_slots(void);
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
/**
 * @brief one sprite-table entry, for the tests (the D-69 tail copies).
 *
 * @param frame the frame index
 * @param off the offset
 * @param rows the row count
 */
void object_render_frame_for_probe(uint32_t frame, uint32_t *off,
                                   uint32_t *rows);
#endif /* HUMANSRE_TEST_PROBES */


/**
 * @brief Publish the object sprite bank.
 *
 * render_objects' object pass (1000:5D56 -> 0x653C): the base sprite of every
 * live record, drawn at (x - scrollX, y - scrollY).
 *
 * @param bank the sprite bank
 * @param size the size
 */
void object_render_publish_bank(const uint8_t *bank, uint32_t size);


/** @brief render_objects' object pass (1000:5D56 -> draw_object_group 1000:6421):
 * the 0x650C list, then scan_object_rows (slots 8..[0x3E73]), then the
 * 0x651E list.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void object_render(uint8_t *page, int32_t scrollX, int32_t scrollY);


/** @brief 1000:40CA render_frame_objects: the claim's presented frames draw
 * only the selected record (`[0x180A]`) and the claimed one (`[0x1AF2]`) over
 * the frozen page - never the whole world.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void object_render_claim_draw_records(uint8_t *page, int32_t scrollX,
                                      int32_t scrollY);

#endif /* HUMANSRE_OBJECT_RENDER_H */
