/* The scene module's shared declarations: the ANI decoder, the fast-mode
 * flag the boot sets and the pre-frame fade both players use.  Nothing here
 * is part of the port's public API; include scene.h for that.
 */
#ifndef HUMANSRE_SCENE_INTERNAL_H
#define HUMANSRE_SCENE_INTERNAL_H

#include "game_resources.h"
#include "video.h"

#include <stdint.h>

#define ANI_NAME     game_resource_name(GAME_RES_ANM_ANI_FMT)
#define PAGE_SIZE    VIDEO_PAGE_BYTES
#define OBJ_SEG_SIZE 65536u // the original decodes objects into a 64K segment

/* The ANI chunk image:
 *   u32[0] = table_size_bytes (also entry[0])
 *   u32[1..n-1] = byte offsets of blobs 1..n-1 into the image
 *   blob k = [entry[k], entry[k+1])
 *
 * Each blob is an LZW object: [u16 decompressed_size][u16 checksum]
 * [u16 bit buffer][LZW bits].  Object 0 is the script object.
 */
typedef struct {
    uint8_t *image;
    uint32_t image_size;
    uint32_t table_size;
    uint32_t n_entries;
    uint32_t *entries; // copy of the u32 offset table
} Ani;

/* --- scene_ani.c -------------------------------------------------------- */

/**
 * @brief Load ANI<ani_idx>.ANI from the movie's disc.
 *
 * @param a the ANI object
 * @param ani_idx the movie index
 * @return 0 on success, -1 when the file is missing
 */
int32_t _ani_load(Ani *a, uint32_t ani_idx);

/** @brief Decode the LZW object at table entry `idx` into `out`.
 *
 * @param a the ANI object
 * @param idx the table entry index
 * @param out the output buffer
 * @param out_size the output capacity
 * @param chkOut the output checksum
 * @return the decompressed size, or -1 on a bad table entry
 */
int32_t _ani_decode_object(const Ani *a, uint32_t idx, uint8_t *out,
                           uint32_t out_size, uint16_t *chkOut);

/** @brief AL expander: blit_background_data (1000:B2EB).
 *
 * @param src the source buffer
 * @param src_size the source size
 * @param dst the destination buffer
 * @param dst_size the destination size
 */
void _scene_al_expand(const uint8_t *src, uint32_t src_size, uint8_t *dst,
                      uint32_t dst_size);

/* --- state shared between the players (defined in scene_play.c) ---------- */

/* [0x627C]: boot_intro_sequence's fast mode - its only writer (set at
 * 1000:B59F, cleared at 1000:B623, entry's caller only).  play_scene_sequence
 * reads it twice: the pre-frame palette fade is 4 entries div 2 instead of
 * 0x100 div 0x19 (1000:B263), and wait_flag_timeout waits 1 tick instead of
 * 6 per object (1000:B2D3).  The level's .ANI always runs with it clear. */
extern uint8_t _scene_fast;
extern uint16_t _dwell_ticks;

/** @brief play_scene_sequence's pre-frame palette fade (1000:B263), shared by the
 * level's .ANI and the boot's logo.
 *
 * @param target the target value
 */
void _scene_palette_fade(const uint8_t *target);

#endif /* HUMANSRE_SCENE_INTERNAL_H */
