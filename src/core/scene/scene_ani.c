/* The .ANI/.RSC object decoder: the u32 table, the LZW objects and the AL
 * expander (blit_background_data 1000:B2EB).
 *
 * The ANI chunk image is:
 *   u32[0] = table_size_bytes (also entry[0])
 *   u32[1..n-1] = byte offsets of blobs 1..n-1 into the image
 *   blob k = [entry[k], entry[k+1])
 *
 * Each blob is an LZW object: [u16 decompressed_size][u16 checksum]
 * [u16 bit buffer][LZW bits].  Object 0 is the script object.
 */

#include "scene/scene_ani.h"

#include "byteorder.h"
#include "osystem.h"
#include "res.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** @brief AL expander: blit_background_data (1000:B2EB).
 *
 * Source = AL stream, destination = page.  Grammar (from the listing):
 *
 *   01..7F   copy CL bytes literally from the stream
 *   00       [count][byte]: store byte COUNT times
 *   81..FF   skip (CL - 0x80) bytes in the page
 *   80       16-bit control word CW:
 *             CW > 0     skip CW bytes
 *             CW == 0    END
 *             CW < 0     CX = CW & 0x7FFF bytes;
 *                        (CH & 0x7F) < 0x40  -> copy CX bytes from stream
 *                        (CH & 0x7F) >= 0x40 -> fill (CX - 0x4000) bytes
 *                          with the next stream byte (SUB CH,0x40 at b365)
 *
 * @param src the source buffer
 * @param src_size the source size
 * @param dst the destination buffer
 * @param dst_size the destination size
 */
void _scene_al_expand(const uint8_t *src, uint32_t src_size, uint8_t *dst,
                      uint32_t dst_size)
{
    uint32_t si = 0, di = 0;

    while (si < src_size) {
        uint8_t cl = src[si++];

        if (cl == 0) { // [count][byte] run
            if (si + 1 >= src_size) break;
            uint8_t count = src[si++];
            uint8_t fill = src[si++];
            for (uint16_t i = 0; i < count && di < dst_size; i++)
                dst[di++] = fill;
            continue;
        }

        if (cl < 0x80) { // literal copy
            for (uint16_t i = 0; i < cl && si < src_size && di < dst_size; i++)
                dst[di++] = src[si++];
            continue;
        }

        if (cl == 0x80) { // 16-bit control word
            if (si + 1 >= src_size) break;
            uint16_t cw = rd_le_u16(src + si);
            si += 2;
            if (cw == 0) break; // END
            if (!(cw & 0x8000u)) { // skip CW bytes
                di += cw;
                if (di > dst_size) di = dst_size;
                continue;
            }
            /* CW < 0: bit 15 set */
            uint16_t cx = (uint16_t)(cw & 0x7fff);
            uint8_t ch =
                (uint8_t)((cw >> 8) & 0x7f); // high byte, bit 15 cleared
            if (ch < 0x40) { // copy CX bytes from stream
                for (uint16_t i = 0; i < cx && si < src_size && di < dst_size;
                     i++)
                    dst[di++] = src[si++];
            } else { // fill (CX - 0x4000) bytes
                if (si >= src_size) break;
                uint8_t fill = src[si++];
                uint32_t run =
                    (uint32_t)cx - 0x4000u; // the listing's SUB CH,0x40 (b365)
                for (uint16_t i = 0; i < run && di < dst_size; i++)
                    dst[di++] = fill;
            }
            continue;
        }

        /* 0x81..0xFF: skip (CL - 0x80) bytes in the page */
        di += (uint32_t)(cl - 0x80);
        if (di > dst_size) di = dst_size;
    }
}

/** @brief Release one ANI object's buffers and clear it.
 *
 * @param a the ANI object
 */
void _ani_release(Ani *a)
{
    g_osys->memFree(a->image);
    g_osys->memFree(a->entries);
    memset(a, 0, sizeof *a);
}

/**
 * @brief Load one .ANI newspaper into the scene decoder.
 *
 * @param a the ANI object
 * @param ani_idx the animation index
 * @return 0 on success, -1 when the file is missing
 */
int32_t _ani_load(Ani *a, uint32_t ani_idx)
{
    char name[32];
    snprintf(name, sizeof name, ANI_NAME, ani_idx);

    uint16_t chk = 0;
    (void)chk;
    a->image = res_load_chunk(name, &a->image_size, &chk);
    if (!a->image) return -1;

    a->table_size = rd_le_u32(a->image);
    a->n_entries = a->table_size / 4;
    a->entries = g_osys->memAlloc(a->n_entries * sizeof(uint32_t));
    if (!a->entries) {
        g_osys->memFree(a->image);
        a->image = NULL;
        return -1;
    }
    for (uint32_t i = 0; i < a->n_entries; i++)
        a->entries[i] = rd_le_u32(a->image + i * 4u);
    return 0;
}

/** @brief Decode the LZW object at table entry `idx` into `out`.
 *
 * *chkOut receives the object's checksum accumulator (0 = the game's
 * scene-object test passes, 1000:B444).
 *
 * @param a the ANI object
 * @param idx the table entry index
 * @param out the output buffer
 * @param out_size the output capacity
 * @param chkOut the output checksum
 * @return the decompressed size, or -1 on a bad table entry
 */
int32_t _ani_decode_object(const Ani *a, uint32_t idx, uint8_t *out,
                           uint32_t out_size, uint16_t *chkOut)
{
    if (idx >= a->n_entries) return -1;
    return res_decode_lzw_mem(a->image, a->image_size, a->entries[idx], out,
                              out_size, chkOut);
}
#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): built into the test executables only,
 * never into a release binary (CMakeLists.txt's core_test). */
/** @brief probe hook: the AL expander, for the offline byte-comparisons.
 *
 * @param src the source buffer
 * @param src_size the source size
 * @param dst the destination buffer
 * @param dst_size the destination size
 */
void scene_al_expand_for_probe(const uint8_t *src, uint32_t src_size,
                               uint8_t *dst, uint32_t dst_size)
{ _scene_al_expand(src, src_size, dst, dst_size); }

#endif /* HUMANSRE_TEST_PROBES */