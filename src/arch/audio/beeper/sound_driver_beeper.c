#include "audio/beeper/sound_driver_beeper.h"

#include "byteorder.h"
#include "arch_mem.h"
#include "thirdparty/pcspeaker_impulse.h"

#include <stdint.h>
#include <string.h>

#define BEEP_HEADER        0x158u
#define BEEP_RECORDS       2u   // CS:0x0D6C, stride 0x15
#define BEEP_SLOTS         2u   // only 0 and 1 are in the free list
#define BEEP_SLOT_BYTES    0x16u
#define BEEP_ROWS          0x40u
#define BEEP_OFFSETS       0x88u // the copied offset table, in words
#define BEEP_TEMPO_DEFAULT 0x30u
#define BEEP_PIT_HZ        1193182u // the test's expected pitch

/* The 0x16-byte speaker slot's fields the sequencer reads or writes; the
 * offsets are the driver's (CS:0x0C76 + 0x16*slot). */
enum {
    BEEP_NOTE = 0x00,
    BEEP_INSTRUMENT = 0x01,
    BEEP_COMMAND = 0x02,
    BEEP_PARAM = 0x03,
    BEEP_VOLUME = 0x04, // cmd 0x0A / 0x0C bookkeeping, never applied
    BEEP_LAST_INSTRUMENT = 0x08, // 0xFF after cmd 5
    BEEP_DIVIDER = 0x12 // word: the PIT channel-2 divisor
};

/* CS:0x08B2, 84 words: the note byte is the table index (1E95:07DE).
 * Indices 12..71 are the 12-TET run A1..G#6; the outer two groups are the
 * extra octaves.  Shipped songs index 14..71. */
static const uint16_t _period[84] = {
    27120, 25595, 24160, 22805, 21525, 20315, 19175, 18100, 17080, 16125, 15220,
    14365, 21696, 20476, 19328, 18244, 17220, 16252, 15340, 14480, 13664, 12900,
    12176, 11492, 10848, 10238, 9664,  9122,  8610,  8126,  7670,  7240,  6832,
    6450,  6088,  5746,  5424,  5119,  4832,  4561,  4305,  4063,  3835,  3620,
    3416,  3225,  3044,  2873,  2712,  2559,  2416,  2280,  2152,  2031,  1917,
    1810,  1708,  1612,  1522,  1436,  1356,  1279,  1208,  1140,  1076,  1015,
    958,   905,   854,   806,   761,   718,   1084,  1023,  966,   912,   861,
    812,   767,   724,   683,   645,   608,   574
};

/* CS:0x0D6C + 0x15*id.  Every field is one the driver sets or reads. */
typedef struct BeeperRecord {
    uint8_t active; // +0x0  0xFF while playing
    uint8_t tracks; // +0x1  image record +0x0D
    uint8_t ticks_per_row; // +0x2  default 6, voice cmd 0x0F
    uint8_t order; // +0x3  index into the order list
    uint16_t cursor; // +0x4  pattern-relative stream pos
    uint8_t tick; // +0x6
    uint8_t advance; // +0x7  toggled by cmds 0x0B/0x0D
    uint8_t row; // +0x8  rows decoded, 0x40 = end
    uint8_t loop; // +0x9
    uint8_t first_order; // +0xA
    uint8_t last_order; // +0xB  last order + 1
    uint8_t finished; // +0xC
    uint8_t track_slot[BEEP_SLOTS]; // +0xD  free-list slot numbers
} BeeperRecord;

struct SoundDriver {
    uint32_t sampleRate;
    uint8_t ready; // CS:0x0C65
    uint8_t tempo; // CS:0x0E64, never read
    uint8_t slot[BEEP_SLOTS][BEEP_SLOT_BYTES];
    uint8_t slot_flag[BEEP_SLOTS]; // CS:0x08A2: 0xFF armed, 0xFE on
    uint8_t slot_free[BEEP_SLOTS]; // CS:0x0E14
    uint8_t header[BEEP_HEADER]; // CS:0x0B0B
    uint16_t offset[BEEP_OFFSETS]; // CS:0x0EA5
    uint16_t offset_count;
    uint8_t *tune;
    uint32_t tune_size;
    uint32_t song_data; // SDS, CS:0x0C6E
    BeeperRecord record[BEEP_RECORDS];
    uint8_t record0_seen; // CS:0x05A9
    uint8_t mux_counter; // CS:0x05A7
    uint8_t mux_period; // CS:0x0E85: ticks per row
    uint8_t mux_half; // CS:0x0E86: ticks per row / 2
    uint8_t speaker_on; // CS:0x07EC
    uint16_t divider;
    PcSpeakerImpulse *speaker; // the vendored impulse model
};

/**
 * @brief One image byte, or 0 past the image's end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the byte
 */
static uint8_t _tune_u8(const SoundDriver *drv, uint32_t off)
{
    if (off >= drv->tune_size) return 0;
    return drv->tune[off];
}

/**
 * @brief Program the PIT channel-2 divider (1E95:081D).
 *
 * A zero divider is the 65536 count the PIT actually loads.
 *
 * @param drv the driver
 * @param divider the le16 divisor from the slot's +0x12
 */
static void _set_divider(SoundDriver *drv, uint16_t divider)
{
    drv->divider = divider;
    pcspeaker_impulse_set_counter(drv->speaker, divider);
}

/**
 * @brief Enable the speaker (1E95:07ED): port 0x61 bits 0-1.
 *
 * The original is idempotent; the PIT mode-3 write it also does is part of
 * the divider path here.
 *
 * @param drv the driver
 */
static void _speaker_on(SoundDriver *drv)
{
    if (drv->speaker_on) return;
    drv->speaker_on = 1;
    /* BEEPER.DRV order (1E95:07ED): port 0x61 bits 0-1, then 0x43 = 0xB6. */
    pcspeaker_impulse_set_speaker(drv->speaker, 1);
    pcspeaker_impulse_set_control(drv->speaker);
}

/**
 * @brief Disable the speaker (1E95:0809): clear port 0x61 bits 0-1.
 *
 * @param drv the driver
 */
static void _speaker_off(SoundDriver *drv)
{
    drv->speaker_on = 0;
    /* 1E95:0809 clears port 0x61, then writes 0x43 = 0xB6. */
    pcspeaker_impulse_set_speaker(drv->speaker, 0);
    pcspeaker_impulse_set_control(drv->speaker);
}

/**
 * @brief Arm a slot for the next multiplex window (1E95:07CC).
 *
 * The driver's extra gate at CS:0x0E75+slot is 0xFF and never written.
 *
 * @param drv the driver
 * @param slot the slot index
 * @param note the note byte, indexing the period table directly
 */
static void _set_note_frequency(SoundDriver *drv, uint32_t slot, uint8_t note)
{
    uint32_t idx = note;

    if (idx >= 84u) idx = 83u; // shortcut: the original reads past its table
    drv->slot_flag[slot] = 0xFFu;
    wr_le_u16(&drv->slot[slot][BEEP_DIVIDER], _period[idx]);
}

/**
 * @brief Hand a slot to (or take it from) the speaker (1E95:058A).
 *
 * An armed slot (0xFF) turns the speaker on and programs its divider; any
 * other value turns the speaker off.
 *
 * @param drv the driver
 * @param slot the slot index
 */
static void _switch_slot(SoundDriver *drv, uint32_t slot)
{
    uint16_t divider = rd_le_u16(&drv->slot[slot][BEEP_DIVIDER]);

    if (drv->slot_flag[slot] == 0xFFu) {
        drv->slot_flag[slot] = 0xFEu;
        _speaker_on(drv);
        _set_divider(drv, divider);
    } else {
        drv->slot_flag[slot] = 0;
        _speaker_off(drv);
    }
}

/**
 * @brief The note-path command table, CS:0x099A (1E95:0734).
 *
 * Table entries: 4 -> 0x07C9 RET, 6 note
 * off, 0x0B jump, 0x0C set volume, 0x0D toggle advance, 0x0F ticks per row,
 * everything else -> 0x0740 RET.
 *
 * @param drv the driver
 * @param rec the record being stepped
 * @param slot the slot index
 */
static void _note_command(SoundDriver *drv, BeeperRecord *rec, uint32_t slot)
{
    uint8_t *v = drv->slot[slot];

    switch (v[BEEP_COMMAND] & 0x1Fu) {
    case 6: // 1E95:0779: note off
        v[BEEP_COMMAND] = 0;
        drv->slot_flag[slot] = 0;
        break;
    case 0x0B: // 1E95:0788: jump to order param-1, advance next tick
        rec->order = (uint8_t)(v[BEEP_PARAM] - 1u);
        rec->advance = (uint8_t)~rec->advance;
        break;
    case 0x0C: // 1E95:0798: volume = min(param, 0x40), never applied
        if (v[BEEP_PARAM] > BEEP_ROWS) v[BEEP_PARAM] = BEEP_ROWS;
        v[BEEP_VOLUME] = v[BEEP_PARAM];
        break;
    case 0x0D: // 1E95:0780: advance to the next order next tick
        rec->advance = (uint8_t)~rec->advance;
        break;
    case 0x0F: // 1E95:07AA: ticks per row, param & 0x1F (0 = no change)
        if ((v[BEEP_PARAM] & 0x1Fu) != 0) {
            rec->tick = 0;
            rec->ticks_per_row = (uint8_t)(v[BEEP_PARAM] & 0x1Fu);
            drv->mux_period = rec->ticks_per_row;
            drv->mux_half = (uint8_t)(rec->ticks_per_row >> 1);
        }
        break;
    default: break; // 1E95:0740/07C9: RET
    }
}

/**
 * @brief The tick-command table, CS:0x095A (1E95:071B).
 *
 * Only command 0x0A is live (0x0742); 1, 2, 3 are RET stubs and every other
 * index is 0x0740 RET.
 *
 * @param drv the driver
 * @param slot the slot index
 */
static void _tick_command(SoundDriver *drv, uint32_t slot)
{
    uint8_t *v = drv->slot[slot];

    switch (v[BEEP_COMMAND] & 0x1Fu) {
    case 0x0A: { // 1E95:0742: high nibble adds, low nibble subtracts
        uint8_t delta = v[BEEP_PARAM];
        uint8_t hi = (uint8_t)(delta >> 4);
        if (hi != 0) {
            uint32_t next = (uint32_t)v[BEEP_VOLUME] + hi;
            v[BEEP_VOLUME] =
                next >= BEEP_ROWS ? (uint8_t)BEEP_ROWS : (uint8_t)next;
        } else {
            uint8_t lo = (uint8_t)(delta & 0x0Fu);
            v[BEEP_VOLUME] =
                v[BEEP_VOLUME] >= lo ? (uint8_t)(v[BEEP_VOLUME] - lo) : 0;
        }
        break;
    }
    default: break;
    }
}

/**
 * @brief Play one decoded cell (1E95:06C3).
 *
 * The instrument's volume is not applied (0x07CA is a RET); only the
 * note/command path is live.
 *
 * @param drv the driver
 * @param rec the record being stepped
 * @param slot the slot index
 */
static void _play_cell(SoundDriver *drv, BeeperRecord *rec, uint32_t slot)
{
    uint8_t *v = drv->slot[slot];

    if (v[BEEP_INSTRUMENT] != 0 && v[BEEP_NOTE] != 0) {
        if (v[BEEP_COMMAND] == 0x0Cu) {
            v[BEEP_COMMAND] = 0;
            v[BEEP_PARAM] = 0;
        }
        v[BEEP_LAST_INSTRUMENT] = v[BEEP_INSTRUMENT];
    }

    if (v[BEEP_NOTE] == 0) {
        _note_command(drv, rec, slot);
    } else if ((v[BEEP_COMMAND] & 0x1Fu) == 3u) {
        _note_command(drv, rec, slot); // 1E95:07C7 RET; no note-on
    } else {
        _note_command(drv, rec, slot);
        _set_note_frequency(drv, slot, v[BEEP_NOTE]);
    }
}

/**
 * @brief Decode one row of the current pattern into the speaker slots.
 *
 * 1E95:0660: `DI` is set to CS:0x0C76 (slot 0) and `beeper_copy_cells`
 * (1E95:06BB) writes the 4-byte cell, then advances `DI` by 0x12, i.e. past
 * the rest of the 0x16-byte slot.  The record's slot list is used by the play
 * loop, not here.  The row cursor then advances a fixed 8 bytes (1E95:06A7).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _decode_row(SoundDriver *drv, BeeperRecord *rec)
{
    uint32_t pattern;
    uint32_t base;
    uint32_t at;
    uint32_t i;

    if (rec->order >= 0x7Fu) { // shortcut: the order list is 0x7F bytes
        rec->row = BEEP_ROWS;
        return;
    }
    pattern = drv->header[0x18u + rec->order];
    if (pattern >= drv->offset_count) {
        rec->row = BEEP_ROWS;
        return;
    }
    base = ((uint32_t)drv->offset[pattern] - drv->offset[0]) * 16u;
    at = rec->cursor;
    for (i = 0; i < rec->tracks; i++) {
        uint8_t *cell = &drv->slot[0][i * BEEP_SLOT_BYTES];
        cell[BEEP_NOTE] = _tune_u8(drv, base + at);
        cell[BEEP_INSTRUMENT] = _tune_u8(drv, base + at + 1u);
        cell[BEEP_COMMAND] = _tune_u8(drv, base + at + 2u);
        cell[BEEP_PARAM] = _tune_u8(drv, base + at + 3u);
        at += 4u;
    }
    rec->cursor = (uint16_t)(rec->cursor + 8u);
    rec->row++;
}

/**
 * @brief Play the cells the row decoder just filled (1E95:05E7).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _play_row(SoundDriver *drv, BeeperRecord *rec)
{
    uint32_t i;

    for (i = 0; i < rec->tracks; i++) _play_cell(drv, rec, rec->track_slot[i]);
}

/**
 * @brief Free a record's slots and mark it inactive (1E95:03D1).
 *
 * The original turns the speaker off once per track; it does not clear the
 * slots' arm flags.
 *
 * @param drv the driver
 * @param rec the record
 */
static void _release_record(SoundDriver *drv, BeeperRecord *rec)
{
    uint32_t i;

    if (!rec->active) return;
    rec->active = 0;
    for (i = 0; i < rec->tracks; i++) {
        uint32_t slot = rec->track_slot[i];
        drv->slot_free[slot] = (uint8_t)slot;
        _speaker_off(drv);
    }
}

/**
 * @brief Advance a record to the next order (1E95:0624).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _advance_order(SoundDriver *drv, BeeperRecord *rec)
{
    rec->tick = 0;
    rec->advance = 0;
    rec->row = 0;
    rec->cursor = (uint16_t)drv->song_data;
    rec->order++;
    if (rec->order != rec->last_order) return;
    rec->finished = 0xFFu;
    if (rec->loop) {
        rec->order = rec->first_order;
        return;
    }
    _release_record(drv, rec);
}

/**
 * @brief End-of-row bookkeeping (1E95:0658).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _row_end(SoundDriver *drv, BeeperRecord *rec)
{
    if (rec->row == BEEP_ROWS) _advance_order(drv, rec);
    if (rec->active && rec->advance) _advance_order(drv, rec);
}

/**
 * @brief Step one record by one driver tick (1E95:05AA).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _step_record(SoundDriver *drv, BeeperRecord *rec)
{
    uint32_t i;

    if (!rec->active) return;
    rec->tick++;
    if (rec->tick < rec->ticks_per_row) {
        for (i = 0; i < rec->tracks; i++) {
            uint8_t *v = drv->slot[rec->track_slot[i]];
            if ((v[BEEP_COMMAND] | v[BEEP_PARAM]) != 0)
                _tick_command(drv, rec->track_slot[i]);
        }
        _row_end(drv, rec);
        return;
    }
    rec->tick = 0;
    _decode_row(drv, rec);
    _play_row(drv, rec);
    _row_end(drv, rec);
}

/**
 * @brief cmd 2: one driver tick - the records and the speaker multiplex.
 *
 * 1E95:051A: step every active record, turn the speaker on/off when record
 * 0's active flag changes, then split the row window between slots 0 and 1.
 *
 * @param drv the driver
 */
static void _beeper_tick(SoundDriver *drv)
{
    uint32_t i;
    uint8_t active;

    if (!drv || !drv->ready) return;
    for (i = 0; i < BEEP_RECORDS; i++) _step_record(drv, &drv->record[i]);

    /* 1E95:0543: the global speaker follows record 0's active edge. */
    active = drv->record[0].active;
    if (active != drv->record0_seen) {
        drv->record0_seen = active;
        if (active) _speaker_on(drv);
        else _speaker_off(drv);
    }

    /* 1E95:055D: slot 0 at the row boundary, slot 1 at half the row. */
    drv->mux_counter++;
    if (drv->mux_counter >= drv->mux_period) {
        drv->mux_counter = 0;
        _switch_slot(drv, 0);
    } else if (drv->mux_counter == drv->mux_half) {
        _switch_slot(drv, 1);
    }
}

/**
 * @brief Install the Beeper driver emulation (INT 60h install, 1E95:00DE).
 *
 * The original's probe (1E95:0893) silences the speaker and programs a zero
 * divider before setting the ready flag.
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when out of memory
 */
static SoundDriver *_beeper_create(uint32_t sampleRate)
{
    SoundDriver *drv = arch_mem_alloc(sizeof(*drv));

    if (!drv) return NULL;
    memset(drv, 0, sizeof(*drv));
    if (sampleRate == 0) sampleRate = 48000;
    drv->sampleRate = sampleRate;
    drv->ready = 1;
    drv->tempo = BEEP_TEMPO_DEFAULT;
    drv->slot_free[0] = 0;
    drv->slot_free[1] = 1;
    /* The image's initial CS:0x0E85/0E86 are both 6; cmd 5 sets 6 and 3. */
    drv->mux_period = 6;
    drv->mux_half = 6;
    drv->speaker = pcspeaker_impulse_create(sampleRate);
    if (!drv->speaker) {
        arch_mem_free(drv);
        return NULL;
    }
    _speaker_off(drv);
    _set_divider(drv, 0);
    return drv;
}

/**
 * @brief cmd 0: uninstall (1E95:019E).
 *
 * @param drv the driver; NULL is accepted
 */
static void _beeper_destroy(SoundDriver *drv)
{
    if (!drv) return;
    pcspeaker_impulse_destroy(drv->speaker);
    arch_mem_free(drv->tune);
    arch_mem_free(drv);
}

/**
 * @brief cmd 0x0A: the master-volume byte (1E95:01DB, no effect).
 *
 * @param drv the driver
 * @param tempo CL, the game's byte; clamped to 0x64
 */
static void _beeper_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    uint8_t value;

    if (!drv) return;
    if (tempo > 0x64u) tempo = 0x64u;
    value = (uint8_t)((0xA3u * (uint32_t)tempo) >> 8);
    if (value != drv->tempo) drv->tempo = value; // 1E95:01FF is a RET
}

/**
 * @brief cmd 0x0E: identify, the 8-byte command block (1E95:01BE).
 *
 * @param drv the driver
 * @return the block (CS:0x0E87), all zero as observed
 */
static const uint8_t *_beeper_command_block(SoundDriver *drv)
{
    static const uint8_t block[8];
    (void)drv;
    return block;
}

/**
 * @brief cmd 0x0C: load a tune image and derive its geometry (1E95:022E).
 *
 * @param drv the driver
 * @param image the .BEP image
 * @param size its byte length
 */
static void _beeper_load_tune(SoundDriver *drv, const uint8_t *image,
                              uint32_t size)
{
    uint32_t max_order = 0;
    uint32_t patterns;
    uint32_t records;
    uint32_t stream;
    uint32_t table;
    uint32_t sds;
    uint32_t i;

    if (!drv || !drv->ready || !image || size < BEEP_HEADER) return;

    /* 1E95:04E3: P = max(order[0..0x7E]) + 1; K = header[0x11B]. */
    for (i = 0; i < 0x7Fu; i++)
        if (image[0x18u + i] > max_order) max_order = image[0x18u + i];
    patterns = max_order + 1u;
    records = image[0x11Bu];
    stream = BEEP_HEADER + records * 16u;
    table = ((patterns * 2u) & 0xFFF0u) + 0x10u;
    sds = stream + table;
    if (sds > size) return;
    if (table / 2u > BEEP_OFFSETS) return;

    arch_mem_free(drv->tune);
    drv->tune = arch_mem_alloc(size);
    if (!drv->tune) {
        drv->tune_size = 0;
        return;
    }
    memcpy(drv->tune, image, size);
    drv->tune_size = size;
    memcpy(drv->header, image, BEEP_HEADER);

    for (i = 0; i < table / 2u; i++)
        drv->offset[i] = rd_le_u16(image + stream + 2u * i);
    drv->offset_count = (uint16_t)(table / 2u);
    drv->song_data = sds;
    _speaker_off(drv); // 1E95:04DD at the end of cmd 0x0C
}

/**
 * @brief cmd 5: start a record as a voice set (1E95:02B0).
 *
 * The handle is the record index; the original returns its record pointer,
 * which the game only stores and hands back to cmd 6.
 *
 * @param drv the driver
 * @param event CX, the record index in the loaded image
 * @return the handle, or 0xFFFF when refused
 */
static uint16_t _beeper_start_event(SoundDriver *drv, uint16_t event)
{
    BeeperRecord *rec = NULL;
    uint32_t index;
    uint32_t tracks;
    uint32_t i;
    uint32_t off;

    if (!drv || !drv->ready || !drv->tune) return 0xFFFF;
    if (event >= drv->header[0x11Bu]) return 0xFFFF;
    for (index = 0; index < BEEP_RECORDS; index++) {
        if (!drv->record[index].active) {
            rec = &drv->record[index];
            break;
        }
    }
    if (!rec) return 0xFFFF;

    off = BEEP_HEADER + (uint32_t)event * 16u;
    tracks = _tune_u8(drv, off + 0xDu);
    if (tracks > BEEP_SLOTS) tracks = BEEP_SLOTS; // shortcut: 1 or 2 ship

    memset(rec, 0, sizeof(*rec)); // 1E95:035B copies the zero template
    for (i = 0; i < tracks; i++) {
        uint32_t slot = 0;
        while (slot < BEEP_SLOTS && drv->slot_free[slot] == 0xFFu) slot++;
        if (slot == BEEP_SLOTS) {
            /* No free speaker slot: the original's failure path. */
            memset(rec, 0, sizeof(*rec));
            return 0xFFFF;
        }
        drv->slot_free[slot] = 0xFFu;
        rec->track_slot[i] = (uint8_t)slot;
        _speaker_off(drv); // 1E95:04E0, once per track
    }

    rec->tracks = (uint8_t)tracks;
    rec->ticks_per_row = 6;
    rec->order = _tune_u8(drv, off + 0xAu);
    rec->first_order = rec->order;
    rec->cursor = (uint16_t)drv->song_data;
    rec->loop = _tune_u8(drv, off + 0xCu);
    rec->last_order = (uint8_t)(_tune_u8(drv, off + 0xBu) + 1u);
    drv->mux_period = 6;
    drv->mux_half = 3;
    /* 1E95:033D/0341: both slots' last-instrument byte is 0xFF. */
    drv->slot[0][BEEP_LAST_INSTRUMENT] = 0xFFu;
    drv->slot[1][BEEP_LAST_INSTRUMENT] = 0xFFu;
    rec->active = 0xFFu;
    return (uint16_t)index;
}

/**
 * @brief cmd 6: release a record (1E95:03F9 -> 1E95:03D1).
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _beeper_release_event(SoundDriver *drv, uint16_t voice)
{
    if (!drv || voice >= BEEP_RECORDS) return;
    _release_record(drv, &drv->record[voice]);
}

/**
 * @brief Render the PC-speaker output.
 *
 * The impulse model already produces silence when the gate is off (its
 * accumulator decays), so the driver only has to pass the sink through.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _beeper_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    if (!drv) {
        memset(dst, 0, (size_t)frames * 2u * sizeof(*dst));
        return;
    }
    pcspeaker_impulse_generate(drv->speaker, dst, frames);
}

const SoundDriverModule g_sound_driver_beeper = {
    _beeper_create,      _beeper_destroy,       NULL,
    _beeper_set_tempo,   _beeper_command_block, _beeper_load_tune,
    _beeper_start_event, _beeper_release_event, _beeper_tick,
    _beeper_generate
};
