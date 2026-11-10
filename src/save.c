#include "save.h"
#include "entity.h"
#include "object.h"
#include "osystem.h"
#include "score.h"
#include "statusbar.h"
#include "byteorder.h"
#include "fs/fs.h"

#include <stdint.h>
#include <string.h>

#define SAVE_LEN 0x1800u
#define SAVE_HDR 0x0Du           // the 13-byte header

static uint8_t _save_buf[SAVE_LEN];   // the [0x1C92] buffer
static uint8_t _save_loaded;          // [0x34]: a save is present

/* The backend (and through it the filesystem module); save_init binds it. */
static const OSystem *_save_osys;

int32_t save_init(const OSystem *osys)
{
    _save_osys = osys;
    return 0;
}

uint8_t save_load_slot(uint8_t slot)
{
    _save_loaded = 0;
    memset(_save_buf, 0, sizeof _save_buf);
    if (slot == 0u || slot > 20u) return 0;

    FSSave *f = _save_osys->fs->openSave((char)('A' + (int32_t)slot - 1));
    if (!f) return 0;                        // the JC path
    uint32_t got = _save_osys->fs->saveRead(f, 0, _save_buf, SAVE_LEN);
    _save_osys->fs->closeSave(f);
    if (got < SAVE_HDR) return 0;            // no header = not a save
    _save_loaded = 1;
    return 1;
}

void save_consume(void) { _save_loaded = 0; }

uint8_t save_present(void) { return _save_loaded; }
uint16_t save_level(void) { return rd_le_u16(_save_buf + 0x00); }
uint16_t save_time(void) { return rd_le_u16(_save_buf + 0x07); }
uint16_t save_members(void) { return rd_le_u16(_save_buf + 0x0B); }
const uint8_t *save_blob(void) { return _save_buf; }

int32_t save_write_slot(uint8_t slot, uint16_t level, uint16_t timeWord,
                        uint16_t members)
{
    static uint8_t out[SAVE_LEN];

    if (slot == 0u || slot > 20u) return 0;

    memset(out, 0, sizeof out);
    wr_le_u16(out + 0x00u, level);
    wr_le_u16(out + 0x02u, score_total_lo());
    wr_le_u16(out + 0x04u, score_total_hi());
    out[0x06] = object_action_program();   // [0x5B56]
    wr_le_u16(out + 0x07u, timeWord);               // BCD sec  | min
    out[0x09] = statusbar_tribe_digit(0u);  // [0x5B25]
    out[0x0A] = statusbar_tribe_digit(1u);  // [0x5B26]
    wr_le_u16(out + 0x0Bu, members);

    object_save_table(out + 0x000Du);          // 0x3E20, 0x25F
    object_save_pool(out + 0x026Cu);           // 0x6F20, 0x13CA
    object_save_programs(out + 0x1636u);       // 0x5B37, 0x20
    object_save_ter(out + 0x1656u);            // 0x649A, 0x28
    entity_save_fixed(out + 0x167Eu);          // 0x64C2, 0x72
    object_save_spans(out + 0x16F0u);          // 0x6F92, 0x54
    statusbar_save_params(out + 0x17F0u);      // [0x5B22], 8

    FSSave *f = _save_osys->fs->createSave((char)('A' + (int32_t)slot - 1));
    if (!f) return 0;
    uint32_t put = _save_osys->fs->saveWrite(f, 0, out, SAVE_LEN);
    _save_osys->fs->closeSave(f);
    return put == SAVE_LEN;
}

void save_restore_state(void)
{
    if (!_save_loaded) return;
    object_load_table(_save_buf + 0x000Du);
    object_load_pool(_save_buf + 0x026Cu);
    object_load_programs(_save_buf + 0x1636u);
    object_load_ter(_save_buf + 0x1656u);
    entity_load_fixed(_save_buf + 0x167Eu);
    object_load_spans(_save_buf + 0x16F0u);
    score_set_total(rd_le_u16(_save_buf + 0x02), rd_le_u16(_save_buf + 0x04));
    statusbar_load_params(_save_buf + 0x17F0u);
    statusbar_set_tribe_digits(_save_buf[0x09], _save_buf[0x0A]);
    object_action_set_program(_save_buf[0x06]);
}
