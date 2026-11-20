/* The Beeper sequencer: both command tables, the row decoder and the record
 * stepper.  The speaker they drive is in beeper_speaker.c.
 */
#include "audio/beeper/beeper_internal.h"

#include <stdint.h>

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
        _beeper_set_note_frequency(drv, slot, v[BEEP_NOTE]);
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
        uint8_t *cell = drv->slot[i]; // DI = slot 0 + i * 0x16
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

void _beeper_release_record(SoundDriver *drv, BeeperRecord *rec)
{
    uint32_t i;

    if (!rec->active) return;
    rec->active = 0;
    for (i = 0; i < rec->tracks; i++) {
        uint32_t slot = rec->track_slot[i];
        drv->slot_free[slot] = (uint8_t)slot;
        _beeper_speaker_off(drv);
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
    _beeper_release_record(drv, rec);
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

void _beeper_step_record(SoundDriver *drv, BeeperRecord *rec)
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
