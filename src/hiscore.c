#include "hiscore.h"
#include "byteorder.h"
#include "fs/fs.h"
#include "osystem.h"

#include <stdint.h>
#include <string.h>

#define HIS_LEVELS 160u // the table's 1-based level count
#define HIS_TOPN   10u
#define HIS_ENTRY  18u
#define HIS_NAME   14u // 13 chars + NUL
#define HIS_TABLE  (2u + HIS_LEVELS * 2u + 18u + HIS_TOPN * HIS_ENTRY)

static uint8_t _hiscore_table[HIS_TABLE];
static uint8_t _hiscore_loaded;

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
 * @return the entry's 18 bytes
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
    wr_le_u16(_hiscore_table, 0x0999u); // [0x38A] = 0x0999
    for (uint16_t n = 1; n <= HIS_LEVELS; n++)
        wr_le_u16(_hiscore_table + 2u * n, 0x0999u);
    for (uint16_t i = 0; i < HIS_TOPN; i++) {
        uint8_t *e = _hiscore_entry(i);
        uint32_t v = _hiscore_default[i].score;

        wr_le_u32(e, v);
        strncpy((char *)e + 4, _hiscore_default[i].name, HIS_NAME - 1u);
    }
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void hiscore_reset(void)
{
    _hiscore_defaults();
    _hiscore_loaded = 1;
}
#endif /* HUMANSRE_TEST_PROBES */

void hiscore_init(void)
{
    if (_hiscore_loaded) return;
    _hiscore_loaded = 1;
    _hiscore_defaults();
}

void hiscore_write(void)
{
    FSSave *f;

    if (!g_osys) return;
    hiscore_init();
    _hiscore_crypt();
    f = g_osys->fs->createNamed("HISCORES.HUM");
    if (f) {
        g_osys->fs->saveWrite(f, 0, _hiscore_table, HIS_TABLE);
        g_osys->fs->closeSave(f);
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

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint32_t hiscore_table_size(void) { return HIS_TABLE; }
#endif /* HUMANSRE_TEST_PROBES */

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
    if (pos >= HIS_TOPN) return -1; // the last entry still wins
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

/** @brief Append one character, or overwrite the last when the buffer is full.
 *
 * The buffer holds at most 13 characters (DS:0x757, the copy at commit time
 * is 14 bytes).
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
        name[12] = c; // 1000:044F/0x0450: a full buffer overwrites index 12
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
    if (column == 0x1Cu) { // delete the last char
        n = *len;
        if (n) {
            n--;
            name[n] = '\0';
        } else {
            name[-1] = '\0'; // 1000:046C: 0x757 - 2 = 0x756
        }
        *len = n;
        return 0;
    }
    if (column == 0x1Du) return 1; // Enter: commit
    return 0;
}
