/* Resource I/O: the volume directory and the block loop.
 *
 *   int63_load_resource    1000:8D63   the INT 63h entry
 *   resource_load          1000:9017   named-chunk load
 *   resource_decode_chunk  1000:9186   the block loop
 *   decode_bitstream       1000:91B9   the LZ machine - lzw.h/lzw.c
 *
 * Deviations: D-198 (zero block size fails), D-263 (a truncated volume
 * reads zeros).
 */

#ifndef HUMANSRE_RES_H
#define HUMANSRE_RES_H

#include "osystem.h"

/**
 * @brief Resource module init (the backend is bound once by osys_bind()).
 *
 * @return 0 on success
 */
int32_t res_init(void);

/**
 * @brief Set the current disc.
 *
 * The [0xF7E] analog: the disc that name-based loads come from (0..2 =
 * HUMANS.000/.001/.002; the original opens ONE volume file built from the
 * [0xF7E] disc number and never scans across discs).
 *
 * @param disc the disc number
 */
void res_set_disc(uint16_t disc);

/** @brief the disc-presence probe: draw_status_indicator's open test (1000:20CF)
 * opens the volume file for the disc and closes it; 0 = the disc is not
 * available (the "PUT THE DISK INTO DRIVE n" screen).
 *
 * @param disc the disc number
 * @return 1 when the volume opens, 0 otherwise
 */
uint8_t res_volume_present(uint16_t disc);

/**
 * @brief The disc that holds one movie file.
 *
 * The level-header intro-movie/disc index -> disc byte: the ported table
 * `202F:0x6287` (1000:AFF8's lookup; corpus-verified 10/10 - every
 * ANI<idx>.ANI ships on the disc the table names).
 * 0xFF = no entry - the analog of the original's CMP AL,0xFF/JE branch (keep
 * the current disc).
 *
 * @param idx the index
 * @return the disc number, or 0xFF when out of range
 */
uint8_t res_movie_disc(uint32_t idx);

/**
 * @brief Load and decode one named resource from the current disc.
 *
 * The name is the relative path inside the volume, e.g. "ANM\ANI03.ANI";
 * NULL when absent - the original's CF to the INT 63 caller.
 *
 * `chkOut` (nullable) receives the BP analog, the checksum resource_load leaves
 * in BP (1000:9183): stored blocks XOR their flag and payload words in; LZ
 * blocks RESET it to the stream's checksum field, then XOR the bit-buffer and
 * every refill word (1000:91B9..91CE).  It is not a gate; the gate tested
 * against 0 is the scene object's (1000:B444).
 *
 * Ownership: the caller owns the returned buffer and releases it with
 * `g_osys->memFree`; the module keeps no copy.
 *
 * @param name the name
 * @param sizeOut the output size
 * @param chkOut the output checksum
 * @return the decoded buffer, or NULL on failure
 */
uint8_t *res_load_chunk(const char *name, uint32_t *sizeOut, uint16_t *chkOut);

#endif /* HUMANSRE_RES_H */
