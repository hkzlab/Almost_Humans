/* hiscore.c — HISCORES.HUM: the persistent table at DS:0x38A, its obfuscation,
 * the top-10 insert and the name entry (re_formats.md §11).
 *
 * The file is a runtime file in the game directory (not on the disc), 520
 * bytes (0x208), loaded whole into DS:0x38A and obfuscated in place:
 *
 *   +0x000  2    the reserved word (0x0999); level 0's slot, never used
 *   +0x002  320  160 x u16 per-level best, level N at +0x002 + 2*N
 *                (`1000:42E9`: BX = 0x38A + [0x1F41]*2)
 *   +0x142  18   unused
 *   +0x154  180  10 x 18: u32 score (LE) at +0, ASCIIZ name at +4
 *
 * Obfuscation `crypt_table_38a` (`1000:06DE`), an involution used by both the
 * load and the save:
 *
 *   bl = 0x47
 *   for each byte:  [di] ^= bl;  bl = rol8(bl,1);  bl += 3
 *
 * `load_file_to_buffer` (`1000:069F`) reads the 520 bytes from DOS handle 0x3B
 * and decrypts only when the read succeeded; `decrypt_region_38a`
 * (`1000:06CC`) encrypts and flushes (the saver).
 *
 * The insert (`1000:01CD`, inside `menu_loop`) walks the ten entries from the
 * bottom up and drops the candidate in below the first entry it beats; a
 * candidate that beats none is discarded.  The name entry (`1000:0486`) edits
 * a 13-character buffer at DS:0x757 and, on Enter, copies it into the inserted
 * entry as 14 bytes with 0x1A -> space and 0x1B -> '.'.
 */
#include "game.h"
#include "byteorder.h"
#include "fs/fs.h"
#include <string.h>

#define HIS_LEVELS 161u /* the table's 1-based level index        */
#define HIS_TOPN   10u
#define HIS_ENTRY  18u
#define HIS_NAME   14u /* 13 chars + NUL                         */
#define HIS_TABLE  (2u + HIS_LEVELS * 2u + 18u + HIS_TOPN * HIS_ENTRY)

static uint8_t hiscoreTable[HIS_TABLE];
static int32_t hiscoreLoaded;

/* the shipped defaults (the image's DS:0x4DE block): what a fresh boot shows
 * before any HISCORES.HUM exists */
static const struct {
    uint32_t score;
    const char *name;
} hiscoreDefault[10] = {
    { 100000u, "STEVE JONES" },  { 90000u, "NIKKI" },
    { 80000u, "NATASHA" },       { 70000u, "MARV" },
    { 60000u, "JAMIE IRVINE" },  { 50000u, "STEVE OLDACRE" },
    { 40000u, "KEV MACINTOSH" }, { 30000u, "IAN HOWIE" },
    { 20000u, "SIMON HIRST" },   { 10000u, "CRAIG MASSEY" }
};

static uint8_t *hiscoreEntry(uint32_t i)
{ return hiscoreTable + 0x154u + i * HIS_ENTRY; }

/* 1000:06DE crypt_table_38a — the involution, same routine for both
 * directions: [di] ^= bl; bl = rol8(bl,1) + 3. */
void hiscore_obfuscate(uint8_t *p, uint32_t n)
{
    uint8_t key = 0x47u;

    for (uint32_t i = 0; i < n; i++) {
        p[i] ^= key;
        key = (uint8_t)(((uint8_t)((key << 1) | (key >> 7))) + 3u);
    }
}

static void hiscore_crypt(void) { hiscore_obfuscate(hiscoreTable, HIS_TABLE); }

/* the shipped state: every level 9:99 and the ten defaults */
static void hiscoreDefaults(void)
{
    memset(hiscoreTable, 0, sizeof hiscoreTable);
    wr_le_u16(hiscoreTable, 0x0999u); /* [0x38A] = 0x0999 */
    for (uint32_t n = 1; n < HIS_LEVELS; n++)
        wr_le_u16(hiscoreTable + 2u + 2u * n, 0x0999u);
    for (uint32_t i = 0; i < HIS_TOPN; i++) {
        uint8_t *e = hiscoreEntry(i);
        uint32_t v = hiscoreDefault[i].score;

        wr_le_u32(e, v);
        strncpy((char *)e + 4, hiscoreDefault[i].name, HIS_NAME - 1u);
    }
}

/* Restore the shipped state (tests and a fresh install). */
void hiscore_reset(void)
{
    hiscoreDefaults();
    hiscoreLoaded = 1;
}

/* 1000:069F load_file_to_buffer: DS:0x38A <- 520 bytes, then decrypt.  A
 * missing or short file leaves the shipped defaults in place. */
void hiscore_init(void)
{
    if (hiscoreLoaded) return;
    hiscoreLoaded = 1;
    hiscoreDefaults();
    {
        FSSave *f = fs.openNamed("HISCORES.HUM");

        if (f) {
            uint32_t n = fs.saveRead(f, 0, hiscoreTable, HIS_TABLE);

            fs.closeSave(f);
            if (n == HIS_TABLE) hiscore_crypt();
            else hiscoreDefaults();
        }
    }
}

/* Re-read the file (a new game / a fresh boot); tests use it to prove the
 * save round-trip without a second process. */
void hiscore_reload(void)
{
    hiscoreLoaded = 0;
    hiscore_init();
}

/* 1000:06CC decrypt_region_38a: encrypt in place, write, restore. */
void hiscore_write(void)
{
    FSSave *f;

    hiscore_init();
    hiscore_crypt();
    f = fs.createNamed("HISCORES.HUM");
    if (f) {
        fs.saveWrite(f, 0, hiscoreTable, HIS_TABLE);
        fs.closeSave(f);
    }
    hiscore_crypt();
}

/* the per-level best as `1000:42E9` reads it: level N's u16 at 0x38A + 2*N */
uint16_t hiscore_best(uint32_t level)
{
    hiscore_init();
    if (level >= HIS_LEVELS) return 0x0999u;
    return rd_le_u16(hiscoreTable + 2u + 2u * level);
}

void hiscore_setBest(uint32_t level, uint16_t value)
{
    hiscore_init();
    if (level >= HIS_LEVELS) return;
    wr_le_u16(hiscoreTable + 2u + 2u * level, value);
}

uint32_t hiscore_count(void) { return HIS_TOPN; }

uint32_t hiscore_score(uint32_t i)
{
    const uint8_t *e;

    hiscore_init();
    if (i >= HIS_TOPN) return 0u;
    e = hiscoreEntry(i);
    return rd_le_u32(e);
}

const char *hiscore_name(uint32_t i)
{
    hiscore_init();
    return i < HIS_TOPN ? (const char *)hiscoreEntry(i) + 4 : "";
}

/* 1000:0486's sanitiser: 0x1A -> space, 0x1B -> '.' */
static char hiscore_char(char c)
{
    if (c == '\x1A') return ' ';
    if (c == '\x1B') return '.';
    return c;
}

/* 1000:01CD: does the score make the table?  The walk only gives up when the
 * candidate is BELOW the last entry, so an equal score still displaces it. */
int32_t hiscore_qualifies(uint32_t score)
{ return score >= hiscore_score(HIS_TOPN - 1u); }

/* 1000:01CD..0x0230: insert, shifting 18-byte entries down one slot.  Returns
 * the slot taken, or -1 when the score beats none of the ten. */
int32_t hiscore_insert(uint32_t score, const char *name)
{
    uint32_t pos = 0;
    int32_t took = -1;

    hiscore_init();
    for (int32_t k = (int32_t)HIS_TOPN - 1; k >= 0; k--) {
        if (score < hiscore_score((uint32_t)k)) {
            pos = (uint32_t)k + 1u;
            break;
        }
    }
    if (pos >= HIS_TOPN) return -1; /* the last entry still wins */
    for (uint32_t j = HIS_TOPN - 1u; j > pos; j--)
        memmove(hiscoreEntry(j), hiscoreEntry(j - 1u), HIS_ENTRY);
    {
        uint8_t *e = hiscoreEntry(pos);

        wr_le_u32(e, score);
        memset(e + 4, 0, HIS_NAME);
        if (name) {
            for (uint32_t i = 0; i < HIS_NAME - 1u && name[i]; i++)
                e[4u + i] = (uint8_t)hiscore_char(name[i]);
        }
    }
    took = (int32_t)pos;
    hiscore_write();
    return took;
}

/* --- 1000:0486: the name-entry grid -------------------------------------
 *
 * The insert screen is a cursor over 0x1C columns: 0x00..0x19 are 'A'..'Z',
 * 0x1A is space, 0x1B is '.', 0x1C deletes the last character and 0x1D
 * confirms.  The buffer holds at most 13 characters (DS:0x757, the copy at
 * commit time is 14 bytes).  A keystroke outside the grid is ignored.
 */
int32_t hiscore_nameApply(char *name, uint32_t column, uint32_t *len)
{
    uint32_t n = *len;

    if (n > 13u) n = 13u;
    name[n] = '\0';
    if (column < 0x1Au) {
        if (n < 13u) {
            name[n++] = (char)('A' + column);
            name[n] = '\0';
        }
        *len = n;
        return 0;
    }
    if (column == 0x1Au || column == 0x1Bu) {
        if (n < 13u) {
            name[n++] = column == 0x1Au ? ' ' : '.';
            name[n] = '\0';
        }
        *len = n;
        return 0;
    }
    if (column == 0x1Cu) { /* delete the last char */
        if (n) n--;
        name[n] = '\0';
        *len = n;
        return 0;
    }
    if (column == 0x1Du) return 1; /* Enter: commit */
    return 0;
}
