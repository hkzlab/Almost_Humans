/* res.h — the interface of src/res.c: resource I/O (subsystem 2, re_formats.md §8)
 * The volume file (named chunks), the LZW decode, the offset-based pool
 * arena standing in for the original's segment pool. */

#ifndef HUMANSRE_RES_H
#define HUMANSRE_RES_H

#include "osystem.h"

int32_t res_init(const OSystem *osys);
/* The [0xF7E] analog: the disc that name-based loads come from (0..2 =
 * HUMANS.000/.001/.002; the original opens ONE volume file built from the
 * [0xF7E] disc number and never scans across discs). */
void res_set_disc(uint32_t disc);
/* draw_status_indicator's disc probe (1000:20CF): opens the volume file for
 * `disc` and closes it; 0 = the disc is not available. */
int32_t res_volume_present(uint32_t disc);
/* The level-header intro-movie/disc index -> disc byte: the ported table
 * `202F:0x6287` (1000:AFF8's lookup; corpus-verified 10/10 — every
 * ANI<idx>.ANI ships on the disc the table names).  0xFF = no entry — the
 * analog of the original's CMP AL,0xFF/JE branch (keep the current disc). */
uint32_t res_movie_disc(uint32_t idx);
/* Load a named chunk case-insensitively out of the CURRENT disc's volume
 * (the name is the relative path inside the volume, e.g. "ANM\ANI03.ANI");
 * NULL when absent — the original's CF to the INT 63 caller (the
 * "PLEASE INSERT HUMANS DISK A/B/C" prompt territory, re_formats.md §8.3).
 * `chkOut` (nullable) receives the BP analog, the resource-level checksum
 * exactly as resource_load leaves it in BP (1000:9183 OR BP,BP / RET):
 * stored blocks XOR their flag + payload words in; LZ blocks RESET the
 * accumulator to the stream's checksum field and then XOR the bit-buffer
 * word and every refill word (1000:91b9..91ce, the 91c7 XOR is dead —
 * 91c9 MOV BP,AX overwrites it, so the size word never contributes).
 * The resource-level value is NOT a 0-gate in the original: every shipped
 * block decodes to a nonzero accumulator (corpus sweep 2026-10-23) yet
 * levels\CODES.BIN loads live through the same machine.  The gate verified
 * against 0 is the scene-object one (1000:B444) and belongs to the scene
 * player. */
uint8_t *res_load_chunk(const char *name, uint32_t *sizeOut, uint16_t *chkOut);
/* Decode one LZW object straight out of a memory buffer (an .ANI's chunk
 * image): [u16 decompressed_size][u16 checksum][u16 bit buffer][LZW bits].
 * Writes `size` bytes to `out`, returns the decompressed size or -1.
 * *chkOut mirrors the machine's final checksum accumulator (BP) — the
 * scene-object gate tests it against 0 (1000:B444). */
int32_t res_decode_lzw_mem(const uint8_t *data, uint32_t data_size,
                           uint32_t off, uint8_t *out, uint32_t out_size,
                           uint16_t *chkOut);

#endif /* HUMANSRE_RES_H */
