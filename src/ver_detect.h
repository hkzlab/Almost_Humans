/* ver_detect.h — the shipped language, identified from the volume data.
 *
 * The original ships one executable per language, so the text column is fixed
 * at build time.  The port ships one binary and picks the column at run time:
 * the first 2048 bytes of HUMANS.001 are not the same for any two shipped
 * languages (the earliest difference is byte 1083), so an FNV-1a 32-bit
 * digest over that window identifies the build.  The window is read as raw
 * bytes through the filesystem module, so the digest is identical on every
 * architecture; nothing host-sized or host-ordered enters it.
 *
 * One byte-order note: compare the digest as an integer (or print it), never
 * by memcmp-ing the digest's in-memory bytes — that would be endian-tagged.
 */
#ifndef HUMANSRE_VER_DETECT_H
#define HUMANSRE_VER_DETECT_H

#include "osystem.h"

#include <stdint.h>

/* The identifying window: HUMANS.001's first 2048 bytes.  The shipped set
 * separates at byte 1083; 2048 is a comfortable read with margin, so a short
 * or absent volume is a detectable failure rather than a wrong match. */
#define VER_DETECT_VOLUME 1u
#define VER_DETECT_LEN    2048u

/**
 * @brief FNV-1a 32-bit digest of a byte buffer.
 *
 * @param buf the buffer (NULL is accepted when n is 0)
 * @param n the byte count
 * @return the digest, the offset basis for an empty buffer
 */
uint32_t ver_detect_hash(const uint8_t *buf, uint32_t n);

/**
 * @brief Map a HUMANS.001 digest to a game_strs column.
 *
 * @param hash a digest from ver_detect_hash over the VER_DETECT_LEN window
 * @return the enum GameLang index, GAME_LANG_EN for an unknown digest
 */
int32_t ver_detect_lang(uint32_t hash);

/**
 * @brief Identify the installed language from HUMANS.001.
 *
 * Reads the VER_DETECT_LEN-byte window through the filesystem module, hashes
 * it and maps it.  A volume that cannot be opened or is shorter than the
 * window keeps the English column: the port still has to start, and English
 * is the column the original's default build shows.  Every non-ENG fallback
 * (unreadable volume, unknown digest) is reported through `osys->warn`.
 *
 * @param osys the backend interface (the warning's destination)
 * @return the enum GameLang index, GAME_LANG_EN on a missing, short or
 *         unrecognized volume (with a warning)
 */
int32_t ver_detect_run(const OSystem *osys);

#endif /* HUMANSRE_VER_DETECT_H */
