/* Resource I/O: the LZ machine, its block loop and the in-memory object
 * decode.
 *
 *   decode_bitstream       1000:91B9   the LZ machine
 *   resource_getword       1000:92AA   the word reader the machine pumps
 *
 * The machine is reader-agnostic: it pulls u16 words through a callback, so
 * the volume path (res.c) and the in-memory object tables (.ANI/.RSC) share
 * one decoder.  Nothing here touches OSystem.
 *
 * Deviations: D-23 (a corrupt decode may read past the output buffer).
 */

#ifndef HUMANSRE_LZW_H
#define HUMANSRE_LZW_H

#include <stdint.h>

/** @brief The word pump: returns the next stream word, one per call. */
typedef uint16_t (*LZWWordReader)(void *ctx);

/**
 * @brief Decode one compressed block backwards into `out`.
 *
 * Reads [checksum][bit buffer] first, then the LZ bits, filling `out`
 * downwards from `base + clen`.  A cursor that reaches 0 stops the block
 * early; that is not an error here - the checksum accumulator carries the
 * outcome, exactly as decode_bitstream leaves BP.
 *
 * @param rd the word pump
 * @param ctx the pump's context
 * @param out the destination buffer
 * @param base this block's first output byte
 * @param clen the block length
 * @return the block's checksum accumulator (BP)
 */
uint16_t lzw_block(LZWWordReader rd, void *ctx, uint8_t *out, uint32_t base,
                   uint32_t clen);

/** @brief Decode one LZW object straight out of a memory buffer (an .ANI's
 * chunk image): [u16 decompressed_size][u16 checksum][u16 bit buffer][LZW
 * bits].
 *
 * Returns the decompressed size written to `out`, or -1 on failure.  *chkOut
 * mirrors the machine's final checksum accumulator (BP) - the scene-object
 * gate tests it against 0 (1000:B444).
 *
 * @param data the data buffer
 * @param data_size the data size
 * @param off the offset
 * @param out the output buffer
 * @param out_size the output size
 * @param chkOut the output checksum
 * @return the decoded size, or -1 on a bad object
 */
int32_t lzw_decode_mem(const uint8_t *data, uint32_t data_size, uint32_t off,
                       uint8_t *out, uint32_t out_size, uint16_t *chkOut);

#endif /* HUMANSRE_LZW_H */
