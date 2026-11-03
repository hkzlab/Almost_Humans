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
 * The header and all five state ranges are now written from the live port
 * state (score/counter, the object table, the group/span pools, the six
 * programs, the TER slots and the entity array), and `save_restoreState` puts
 * them back after `level_load`.  The 8 bytes at +0x17F0 are the live
 * [0x5B22] odometer block (statusbar_saveParams/loadParams).
 *
 * The load mirrors entry:004C: a missing/short file is the original's JC
 * path (no save, the fresh game), never an abort. */
#include "game.h"
#include "byteorder.h"
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

/* level_run's tail (1000:30B4): [0x34] := 0 after the level has started —
 * a later "restart this level" takes the LEVEL board's time again. */
void save_consume(void) { save_loaded = 0; }

int save_present(void)          { return save_loaded; }
uint16_t save_level(void)       { return rd_le_u16(save_buf + 0x00); }
uint16_t save_time(void)        { return rd_le_u16(save_buf + 0x07); }
unsigned save_members(void)     { return rd_le_u16(save_buf + 0x0B); }
const uint8_t *save_blob(void)  { return save_buf; }

/* SAVE GAME (save_progress 1FE7:0000, re_formats.md §10.0): the 13-byte
 * header from the port's live state + the five state ranges. */
int save_writeSlot(unsigned slot, unsigned level, uint16_t timeWord,
                   unsigned members)
{
    static uint8_t out[SAVE_LEN];

    if (slot == 0u || slot > 20u) return 0;

    memset(out, 0, sizeof out);
    wr_le_u16(out + 0x00u, (uint16_t)level);
    wr_le_u16(out + 0x02u, score_totalLo());
    wr_le_u16(out + 0x04u, score_totalHi());
    out[0x06] = (uint8_t)object_actionProgram();   /* [0x5B56] */
    wr_le_u16(out + 0x07u, timeWord);               /* BCD sec  | min    */
    out[0x09] = (uint8_t)statusbar_tribeDigit(0u);  /* [0x5B25] */
    out[0x0A] = (uint8_t)statusbar_tribeDigit(1u);  /* [0x5B26] */
    wr_le_u16(out + 0x0Bu, (uint16_t)members);

    object_saveTable(out + 0x000Du);          /* 0x3E20, 0x25F          */
    object_savePool(out + 0x026Cu);           /* 0x6F20, 0x13CA         */
    object_savePrograms(out + 0x1636u);       /* 0x5B37, 0x20           */
    object_saveTer(out + 0x1656u);            /* 0x649A, 0x28           */
    entity_saveFixed(out + 0x167Eu);          /* 0x64C2, 0x72           */
    object_saveSpans(out + 0x16F0u);          /* 0x6F92, 0x54           */
    statusbar_saveParams(out + 0x17F0u);      /* [0x5B22], 8            */

    FSSave *f = fs.createSave((char)('A' + (int)slot - 1));
    if (!f) return 0;
    uint32_t put = fs.saveWrite(f, 0, out, SAVE_LEN);
    fs.closeSave(f);
    return put == SAVE_LEN;
}

/* entry:004C / load_level_record_a+_b: put the saved ranges back after the
 * level has been loaded and its map spawns have run. */
void save_restoreState(void)
{
    if (!save_loaded) return;
    object_loadTable(save_buf + 0x000Du);
    object_loadPool(save_buf + 0x026Cu);
    object_loadPrograms(save_buf + 0x1636u);
    object_loadTer(save_buf + 0x1656u);
    entity_loadFixed(save_buf + 0x167Eu);
    object_loadSpans(save_buf + 0x16F0u);
    score_setTotal(rd_le_u16(save_buf + 0x02), rd_le_u16(save_buf + 0x04));
    statusbar_loadParams(save_buf + 0x17F0u);
    statusbar_setTribeDigits(save_buf[0x09], save_buf[0x0A]);
    object_actionSetProgram(save_buf[0x06]);
}
