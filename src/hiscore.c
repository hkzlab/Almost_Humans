/* hiscore.c — HISCORES.HUM: the table at DS:0x38A, its obfuscation, the
 * top-10 insert and the name entry (re_formats.md §11).
 *
 * The file is a runtime file in the game directory (not on the disc), 520
 * bytes (0x208), written whole from DS:0x38A and obfuscated in place.  The
 * original's name is corrupted: the CD probe (`1FE7:027B`) stamps the MSCDEX
 * drive letter over the 'H' of the image's "HISCORES.HUM" at `[0x3B]`, so the
 * name DOS opens is "<letter>ISCORES.HUM".  The port keeps the intended name;
 * that is the accepted deviation D-225 (an original bug, not reproduced).
 *
 *   +0x000  2    the reserved word (0x0999); level 0's slot, never used
 *   +0x002  320  160 x u16 per-level best, level N at +2*N
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
 * and decrypts only when the read succeeded, but **nothing calls it**: the
 * table's only initial state is the image's own defaults at DS:0x38A and the
 * file is write-only.  `decrypt_region_38a` (`1000:06CC`) encrypts and
 * flushes (the saver), and leaves the runtime block encrypted.
 *
 * The insert (`1000:01CD`, inside `menu_loop`) walks the ten entries from the
 * bottom up and drops the candidate in below the first entry it beats; a
 * candidate that beats none is discarded.  It is memory-only: the flush is the
 * MAP exit's single `0x06CC`.  The name entry (`1000:0486`) edits
 * a 13-character buffer at DS:0x757 and, on Enter, copies it into the inserted
 * entry as 14 bytes with 0x1A -> space and 0x1B -> '.'.
 */
#include "hiscore.h"
#include "byteorder.h"
#include "fs/fs.h"

#include <string.h>

#define HIS_LEVELS 160u /* the table's 1-based level count        */
#define HIS_TOPN   10u
#define HIS_ENTRY  18u
#define HIS_NAME   14u /* 13 chars + NUL                         */
#define HIS_TABLE  (2u + HIS_LEVELS * 2u + 18u + HIS_TOPN * HIS_ENTRY)

static uint8_t _hiscore_table[HIS_TABLE];
static uint8_t _hiscore_loaded;

/* The backend (and through it the filesystem module); hiscore_bind sets it. */
static const OSystem *_hiscore_osys;

/* the shipped defaults (the image's DS:0x4DE block): what a fresh boot shows
 * before any HISCORES.HUM exists */
static const struct {
    uint32_t score;
    const char *name;
} _hiscore_default[10] = {
    { 100000u, "STEVE JONES" },  { 90000u, "NIKKI" },
    { 80000u, "NATASHA" },       { 70000u, "MARV" },
    { 60000u, "JAMIE IRVINE" },  { 50000u, "STEVE OLDACRE" },
    { 40000u, "KEV MACINTOSH" }, { 30000u, "IAN HOWIE" },
    { 20000u, "SIMON HIRST" },   { 10000u, "CRAIG MASSEY" }
};

/**
 * @brief Accessor: one high-score entry.
 *
 * @param i the index
 * @return One high-score entry.
 */
static uint8_t *_hiscore_entry(uint32_t i)
{ return _hiscore_table + 0x154u + i * HIS_ENTRY; }

void hiscore_obfuscate(uint8_t *p, uint32_t n)
{
    uint8_t key = 0x47u;

    for (uint16_t i = 0; i < n; i++) {
        p[i] ^= key;
        key = (uint8_t)(((uint8_t)((key << 1) | (key >> 7))) + 3u);
    }
}

/**
 * @brief Crypt/decrypt the high-score table in place.
 */
static void _hiscore_crypt(void)
{ hiscore_obfuscate(_hiscore_table, HIS_TABLE); }

/**
 * @brief the shipped state: every level 9:99 and the ten defaults
 */
static void _hiscore_defaults(void)
{
    memset(_hiscore_table, 0, sizeof _hiscore_table);
    wr_le_u16(_hiscore_table, 0x0999u); /* [0x38A] = 0x0999 */
    for (uint16_t n = 1; n <= HIS_LEVELS; n++)
        wr_le_u16(_hiscore_table + 2u * n, 0x0999u);
    for (uint16_t i = 0; i < HIS_TOPN; i++) {
        uint8_t *e = _hiscore_entry(i);
        uint32_t v = _hiscore_default[i].score;

        wr_le_u32(e, v);
        strncpy((char *)e + 4, _hiscore_default[i].name, HIS_NAME - 1u);
    }
}

void hiscore_reset(void)
{
    _hiscore_defaults();
    _hiscore_loaded = 1;
}

void hiscore_init(void)
{
    if (_hiscore_loaded) return;
    _hiscore_loaded = 1;
    _hiscore_defaults();
}

void hiscore_bind(const OSystem *osys) { _hiscore_osys = osys; }

void hiscore_write(void)
{
    FSSave *f;

    if (!_hiscore_osys) return;
    hiscore_init();
    _hiscore_crypt();
    f = _hiscore_osys->fs->createNamed("HISCORES.HUM");
    if (f) {
        _hiscore_osys->fs->saveWrite(f, 0, _hiscore_table, HIS_TABLE);
        _hiscore_osys->fs->closeSave(f);
    }
}

uint16_t hiscore_best(uint32_t level)
{
    hiscore_init();
    if (level == 0u || level > HIS_LEVELS) return 0x0999u;
    return rd_le_u16(_hiscore_table + 2u * level);
}

void hiscore_set_best(uint32_t level, uint16_t value)
{
    hiscore_init();
    if (level == 0u || level > HIS_LEVELS) return;
    wr_le_u16(_hiscore_table + 2u * level, value);
}

uint8_t hiscore_count(void) { return HIS_TOPN; }

uint32_t hiscore_table_size(void) { return HIS_TABLE; }

uint32_t hiscore_score(uint32_t i)
{
    const uint8_t *e;

    hiscore_init();
    if (i >= HIS_TOPN) return 0u;
    e = _hiscore_entry(i);
    return rd_le_u32(e);
}

const char *hiscore_name(uint32_t i)
{
    hiscore_init();
    return i < HIS_TOPN ? (const char *)_hiscore_entry(i) + 4 : "";
}

/**
 * @brief 1000:0486's sanitiser: 0x1A -> space, 0x1B -> '.'
 *
 * @param c the character
 * @return the sanitised character
 */
static char _hiscore_char(char c)
{
    if (c == '\x1A') return ' ';
    if (c == '\x1B') return '.';
    return c;
}

uint8_t hiscore_qualifies(uint32_t score)
{ return score >= hiscore_score(HIS_TOPN - 1u); }

int32_t hiscore_insert(uint32_t score, const char *name)
{
    uint8_t pos = 0;

    hiscore_init();
    for (int16_t k = (int16_t)HIS_TOPN - 1; k >= 0; k--) {
        if (score < hiscore_score((uint32_t)k)) {
            pos = (uint8_t)((uint32_t)k + 1u);
            break;
        }
    }
    if (pos >= HIS_TOPN) return -1; /* the last entry still wins */
    for (uint16_t j = HIS_TOPN - 1u; j > pos; j--)
        memmove(_hiscore_entry(j), _hiscore_entry(j - 1u), HIS_ENTRY);
    {
        uint8_t *e = _hiscore_entry(pos);

        wr_le_u32(e, score);
        memset(e + 4, 0, HIS_NAME);
        if (name) {
            for (uint16_t i = 0; i < HIS_NAME - 1u && name[i]; i++)
                e[4u + i] = (uint8_t)_hiscore_char(name[i]);
        }
    }
    return (int32_t)pos;
}

/** @brief --- 1000:0486: the name-entry grid -------------------------------------
 *
 * The insert screen is a cursor over 0x1C columns: 0x00..0x19 are 'A'..'Z',
 * 0x1A is space, 0x1B is '.', 0x1C deletes the last character and 0x1D
 * confirms.  The buffer holds at most 13 characters (DS:0x757, the copy at
 * commit time is 14 bytes).  A keystroke outside the grid is ignored.
 *
 * @param name the name
 * @param len the length
 * @param c the character
 */
static void _name_put(char *name, uint8_t *len, char c)
{
    uint8_t n = *len;

    if (n > 13u) n = 13u;
    if (n < 13u) {
        name[n] = c;
        name[++n] = '\0';
    } else {
        name[12] = c; /* 1000:044F/0x0450: a full buffer overwrites index 12 */
    }
    *len = n;
}

uint8_t hiscore_name_apply(char *name, uint8_t column, uint8_t *len)
{
    uint8_t n = *len;

    if (n > 13u) n = 13u;
    name[n] = '\0';
    *len = n;
    if (column < 0x1Au) {
        _name_put(name, len, (char)('A' + column));
        return 0;
    }
    if (column == 0x1Au || column == 0x1Bu) {
        _name_put(name, len, column == 0x1Au ? ' ' : '.');
        return 0;
    }
    if (column == 0x1Cu) { /* delete the last char */
        n = *len;
        if (n) {
            n--;
            name[n] = '\0';
        } else {
            name[-1] = '\0'; /* 1000:046C: 0x757 - 2 = 0x756 */
        }
        *len = n;
        return 0;
    }
    if (column == 0x1Du) return 1; /* Enter: commit */
    return 0;
}
