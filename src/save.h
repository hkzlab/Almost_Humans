/* save.h — the interface of src/save.c: the save-game format (load side)
 *
 * re_formats.md §10/§10.0: the 0x1800 file `SAVE<'A'+n-1>` (the fs module's
 * name for the original's `\humans\cdrom\saveX`).  The load is entry:004C's
 * path: the slot is the front-end room's RESTORE GAME counter (GAME 1..20),
 * and a hit overrides the room's level, the time limit and the tribe.
 */

#ifndef HUMANSRE_SAVE_H
#define HUMANSRE_SAVE_H

#include "osystem.h"

int32_t save_init(const OSystem *osys);
/* Load the slot (1..20; 0 = GAME OFF).  Resets the state first; returns 1
 * when the file's header was read (the original's "save present" [0x34]). */
int32_t save_load_slot(uint32_t slot);
int32_t save_present(void); /* [0x34]                              */
uint16_t save_level(void); /* +0x00, 1..160 -> [0x22]             */
uint16_t save_time(void); /* +0x07 word -> [0x5D31]              */
uint32_t save_members(void); /* +0x0B -> [0x1970]                   */
/* The whole 0x1800 buffer ([0x1C92]): the header and the state ranges are
 * carried verbatim (the ranges have no port consumer yet). */
const uint8_t *save_blob(void);
void save_restore_state(void); /* after level_load: the 0x1800 ranges */
/* level_run 1000:30B4: [0x34] := 0 after the level starts, so a later
 * "restart this level" takes the LEVEL board's time again. */
void save_consume(void);
/* SAVE GAME (save_progress 1FE7:0000): the live header and the five state
 * ranges, 0x1800 bytes; save_restore_state puts them back after level_load.
 * The +0x17F0 parameter block is the image's [0x5B22] defaults. */
int32_t save_write_slot(uint32_t slot, uint32_t level, uint16_t timeWord,
                        uint32_t members);

#endif /* HUMANSRE_SAVE_H */
