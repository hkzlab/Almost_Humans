/* The .ANI/.RSC object decoder: the u32 table, the LZW objects and the AL
 * expander.
 */
#ifndef HUMANSRE_SCENE_ANI_H
#define HUMANSRE_SCENE_ANI_H

#include "game_resources.h"

#include <stdint.h>

#define ANI_NAME game_resource_name(GAME_RES_ANM_ANI_FMT)

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

/**
 * @brief Load ANI<ani_idx>.ANI from the movie's disc.
 *
 * @param a the ANI object
 * @param ani_idx the movie index
 * @return 0 on success, -1 when the file is missing
 */
int32_t _ani_load(Ani *a, uint32_t ani_idx);

/**
 * @brief Release one ANI object's buffers and clear it.
 *
 * @param a the ANI object
 */
void _ani_release(Ani *a);

/**
 * @brief Decode the LZW object at table entry `idx` into `out`.
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

/**
 * @brief AL expander: blit_background_data (1000:B2EB).
 *
 * @param src the source buffer
 * @param src_size the source size
 * @param dst the destination buffer
 * @param dst_size the destination size
 */
void _scene_al_expand(const uint8_t *src, uint32_t src_size, uint8_t *dst,
                      uint32_t dst_size);

#endif /* HUMANSRE_SCENE_ANI_H */
