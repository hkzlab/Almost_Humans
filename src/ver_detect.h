/* The shipped language, identified from the first 2048 bytes of HUMANS.001
 * (FNV-1a 32).
 *
 * The original ships one executable per language; the port ships one binary
 * and picks the text column at boot.  An unknown digest keeps English; a
 * missing or short volume aborts the boot.  Compare the digest as an integer
 * (or print it): memcmp-ing its bytes is endian-tagged.
 * Deviation: D-257 (detection instead of a fixed build).
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
 * it and maps it.  An unrecognized digest keeps the English column and is
 * reported through `osys->warn`: the port still starts, with the column the
 * original's default build shows.  A volume that cannot be opened or is
 * shorter than the window is not a fallback but a failure to detect the
 * installed data, so it is reported through the return value and the caller
 * aborts the boot.
 *
 * @param osys the backend interface (the warning's destination)
 * @return the enum GameLang index, GAME_LANG_EN on an unrecognized digest
 *         (with a warning); a negative value when HUMANS.001 is missing or
 *         shorter than the window
 */
int32_t ver_detect_run(const OSystem *osys);

#endif /* HUMANSRE_VER_DETECT_H */
