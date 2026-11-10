/* save.h - the interface of src/save.c: the save-game format
 *
 * The 0x1800 file `SAVE<'A'+n-1>` (the fs module's
 * name for the original's `\humans\cdrom\saveX`).  The load is entry:004C's
 * path: the slot is the front-end room's RESTORE GAME counter (GAME 1..20),
 * and a hit overrides the room's level, the time limit and the tribe.
 */

#ifndef HUMANSRE_SAVE_H
#define HUMANSRE_SAVE_H

#include "osystem.h"

/**
 * @brief Bind the save module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t save_init(const OSystem *osys);

/** @brief entry:004C: slot 1..20 -> the letter 'A'+(n-1); 0 = GAME OFF.  Resets the
 * state first, so a failed/absent slot leaves the fresh-game path.
 *
 * Resets the state first; returns 1 when the file's header was read (the
 * original's "save present" [0x34]).
 *
 * @param slot the slot
 * @return 1 when the slot loaded, 0 otherwise
 */
uint8_t save_load_slot(uint8_t slot);

/**
 * @brief Accessor: a save is loaded ([0x34]).
 *
 * @return A save is loaded.
 */
uint8_t save_present(void);

/**
 * @brief Accessor: the saved level word (+0x00, 1..160 -> [0x22]).
 *
 * @return The saved level word.
 */
uint16_t save_level(void);

/**
 * @brief Accessor: the saved time word (+0x07 word -> [0x5D31]).
 *
 * @return The saved time word.
 */
uint16_t save_time(void);

/**
 * @brief Accessor: the saved member count (+0x0B -> [0x1970]).
 *
 * @return The saved member count.
 */
uint16_t save_members(void);

/**
 * @brief Accessor: the save blob.
 *
 * The whole 0x1800 buffer ([0x1C92]): the header and the six state ranges,
 * carried verbatim.  The core does not use it - save_restore_state reads the
 * buffer directly - so this accessor serves the tests only, which check that
 * the ranges were carried unmodified.
 *
 * @return The save blob.
 */
const uint8_t *save_blob(void);

/** @brief entry:004C / load_level_record_a+_b: put the saved ranges back after the
 * level has been loaded and its map spawns have run (after level_load: the
 * 0x1800 ranges).
 */
void save_restore_state(void);

/** @brief level_run's tail (1000:30B4): [0x34] := 0 after the level has started -
 * a later "restart this level" takes the LEVEL board's time again.
 */
void save_consume(void);

/** @brief SAVE GAME (save_progress 1FE7:0000): the live 13-byte header and the
 * five state ranges, 0x1800 bytes.
 *
 * save_restore_state puts them back after level_load.  The +0x17F0 parameter
 * block is the image's [0x5B22] defaults.
 *
 * @param slot the slot
 * @param level the level number
 * @param timeWord the time word
 * @param members the member count
 * @return 1 when the whole save was written, 0 otherwise
 */
int32_t save_write_slot(uint8_t slot, uint16_t level, uint16_t timeWord,
                        uint16_t members);

#endif /* HUMANSRE_SAVE_H */
