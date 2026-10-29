/* save.c — the save-game format (re_formats.md §10/§10.0), load side.
 *
 * The file is 0x1800 bytes, named by the slot letter: the original keeps
 * `"\humans\cdrom\save"` at 202F:0x1C7E whose NUL at 0x1C90 is overwritten
 * with `'A'+([0x5E6F]-1)` (entry:004C) or with the save-number prompt's
 * `'A'+(n-1)` (save_progress) — one file per game 1..20.  The filesystem
 * module owns the actual filename (fs.h).
 *
 * Layout (re_formats.md §10):
 *   +0x00 u16 level id 1..160        -> [0x1F41], and via the entry push to
 *                                       [0x22] it OVERRIDES the room's code match
 *   +0x02 u16 / +0x04 u16            -> [0x592]/[0x594], the 32-bit score
 *   +0x06 u8  state/screen id        -> [0x5B56]
 *   +0x07 u16 the remaining-time word (minutes << 8 | BCD seconds) -> [0x5D31]
 *   +0x09 u8/u8 the 2-digit counter  -> [0x5B25]/[0x5B26]
 *   +0x0B u16 tribe members          -> [0x1970]
 *   +0x0D..  the state ranges        -> the [0x3E20]/[0x6F20]/[0x5B37]/[0x649A]/
 *            [0x6F92] copies (load_level_record_a/_b) and the [0x5B22]
 *            parameter block at +0x17F0.
 *
 * The port consumes the header fields that have a consumer today: the level
 * (+0x00), the time (+0x07) and the members (+0x0B).  The rest of the header
 * and the whole state blob are carried verbatim in `save_blob` — the ranges
 * have no port consumer yet (the entity/object state is a later objective),
 * so they are NOT mapped into invented structures.
 *
 * The load mirrors entry:004C: a missing/short file is the original's JC
 * path (no save, the fresh game), never an abort. */
#include "game.h"
#include "fs/fs.h"
#include <string.h>

#define SAVE_LEN 0x1800u
#define SAVE_HDR 0x0Du           /* the 13-byte header               */

static uint8_t save_buf[SAVE_LEN];   /* the [0x1C92] buffer          */
static int     save_loaded;          /* [0x34]: a save is present    */

int save_init(const OSystem *osys) { (void)osys; return 0; }

/* entry:004C: slot 1..20 -> the letter 'A'+(n-1); 0 = GAME OFF.  Resets the
 * state first, so a failed/absent slot leaves the fresh-game path. */
int save_loadSlot(unsigned slot)
{
    save_loaded = 0;
    memset(save_buf, 0, sizeof save_buf);
    if (slot == 0u || slot > 20u) return 0;

    FSSave *f = fs.openSave((char)('A' + (int)slot - 1));
    if (!f) return 0;                        /* the JC path */
    uint32_t got = fs.saveRead(f, 0, save_buf, SAVE_LEN);
    fs.closeSave(f);
    if (got < SAVE_HDR) return 0;            /* no header = not a save */
    save_loaded = 1;
    return 1;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int save_present(void)          { return save_loaded; }
uint16_t save_level(void)       { return rd16(save_buf + 0x00); }
uint16_t save_time(void)        { return rd16(save_buf + 0x07); }
unsigned save_members(void)     { return rd16(save_buf + 0x0B); }
const uint8_t *save_blob(void)  { return save_buf; }
