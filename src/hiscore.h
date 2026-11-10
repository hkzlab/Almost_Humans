/* HISCORES.HUM: the table at DS:0x38A, its obfuscation and the top-10 insert.
 *
 *   load_file_to_buffer   1000:069F   the loader (never called)
 *   crypt_table_38a       1000:06DE   the involution
 *   decrypt_region_38a    1000:06CC   encrypt and flush
 *   the insert            1000:01CD
 *   the name entry        1000:0486
 *   the per-level bests   1000:42E9
 *
 * Deviation: D-225 (writes HISCORES.HUM, not the probe-corrupted name).
 */

#ifndef HUMANSRE_HISCORE_H
#define HUMANSRE_HISCORE_H

#include "osystem.h"

#include <stdint.h>

/* --- HISCORES.HUM (menu_loop 1000:012A) ------------------------------- */

/** @brief the runtime table's first state.  `1000:069F` (the loader) has no
 * caller in the listing, so the shipped game always starts from the image's
 * own defaults at DS:0x38A and never reads HISCORES.HUM back.
 */
void hiscore_init(void);

/**
 * @brief Bind the module to the backend it writes HISCORES.HUM through.
 *
 * @param osys the backend interface
 */
void hiscore_bind(const OSystem *osys);

/**
 * @brief Restore the shipped defaults (tests and a fresh install).
 */
void hiscore_reset(void);

/** @brief 1000:06DE crypt_table_38a - the involution, same routine for both
 * directions: [di] ^= bl; bl = rol8(bl,1) + 3.
 *
 * @param p the table
 * @param n the count
 */
void hiscore_obfuscate(uint8_t *p, uint32_t n);

/**
 * @brief 1000:06CC decrypt_region_38a: encrypt the table and flush it.
 *
 * The crypt is not undone: `1000:06CC` is crypt -> create/write/close -> RET,
 * so the runtime block stays encrypted.
 */
void hiscore_write(void);

/**
 * @brief the per-level best as `1000:42E9` reads it: level N's u16 at
 * 0x38A + 2*N.
 *
 * @param level the level number
 * @return the level's best time, or 0x0999 when out of range
 */
uint16_t hiscore_best(uint32_t level);

/**
 * @brief Record the best time for one level.
 *
 * @param level the level number
 * @param value the value
 */
void hiscore_set_best(uint32_t level, uint16_t value);

/**
 * @brief Accessor: the table's entry count (10).
 *
 * @return The table's entry count.
 */
uint8_t hiscore_count(void);

/** @brief The on-disk table length (the original's 0x208 = 520 bytes).
 *
 * @return the table size
 */
uint32_t hiscore_table_size(void);

/**
 * @brief Accessor: one entry's score (the top-10, descending).
 *
 * @param i the index
 * @return One entry's score.
 */
uint32_t hiscore_score(uint32_t i);

/**
 * @brief Accessor: one entry's name.
 *
 * @param i the index
 * @return One entry's name.
 */
const char *hiscore_name(uint32_t i);

/** @brief 1000:01CD: does the score make the table?  The walk only gives up when the
 * candidate is BELOW the last entry, so an equal score still displaces it.
 *
 * @param score the score
 * @return 1 when the score makes the table, 0 otherwise
 */
uint8_t hiscore_qualifies(uint32_t score);

/** @brief 1000:01CD..0x0230: insert, shifting 18-byte entries down one slot.
 * Memory-only: the original flushes at the MAP exit (`0x06CC`). Returns the
 * slot taken, or -1 when the score beats none of the ten.
 *
 * @param score the score
 * @param name the name
 * @return the insertion position, or -1 when it did not make the table
 */
int32_t hiscore_insert(uint32_t score, const char *name);

/**
 * @brief Apply one typed character to a name; 0 = done.
 *
 * `name` must have one writable byte before name[0]: an empty delete writes
 * the original's 0x756 byte (a duplicate would take the headroom).
 *
 * @param name the name
 * @param column the column
 * @param len the length
 * @return 1 when Enter commits, 0 otherwise
 */
uint8_t hiscore_name_apply(char *name, uint8_t column, uint8_t *len);

#endif /* HUMANSRE_HISCORE_H */
