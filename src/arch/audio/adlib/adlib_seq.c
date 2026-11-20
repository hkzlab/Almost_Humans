/* The AdLib sequencer: the two voice-command tables, the row decoder and
 * the record stepper.  The chip state they drive is in adlib_opl.c.
 */
#include "audio/adlib/adlib_internal.h"

#include <stdint.h>

/**
 * @brief The per-tick voice commands, table CS:0x0F4C (1E95:07EE).
 *
 * The driver's table has four live entries and sends everything else to
 * 1E95:0813 (a bare RET); command 0 goes to 1E95:0814, also a RET.  All 32
 * entries are transcribed here so that no command is silently dropped and the
 * table's shape stays visible.
 *
 *   00 1E95:0814 RET          01 1E95:08AA fnum += 2*param, wrap up
 *   02 1E95:08D4 fnum -= 2*param, wrap down
 *   03 1E95:0972 portamento toward the cached note entry
 *   0A 1E95:0874 volume delta (high nibble adds, low nibble subtracts)
 *   every other index 1E95:0813 RET
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _tick_command(SoundDriver *drv, uint32_t voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t step;
    uint16_t fnum;
    uint16_t entry;

    switch (v->command & 0x1Fu) {
        case 0x01: // 1E95:08AA: slide up by 2 * parameter, octave wrap at 0x2AE
            step = (uint16_t)(2u * v->param);
            fnum = (uint16_t)(v->fnum + step);
            while (fnum >= ADLIB_FNUM_WRAP) {
                fnum = (uint16_t)((fnum - ADLIB_FNUM_WRAP) + ADLIB_FNUM_FIRST);
                v->octave++; // 8-bit, as the driver's INC DL
            }
            v->fnum = fnum;
            entry = (uint16_t)(fnum | (v->octave << 12));
            v->note_entry = entry;
            _adlib_set_frequency(drv, voice, entry);
            break;
        case 0x02: // 1E95:08D4: slide down by 2 * parameter, wrap at 0x142
            step = (uint16_t)(2u * v->param);
            fnum = (uint16_t)(v->fnum - step);
            while (fnum <= ADLIB_FNUM_LOW) {
                /* 1E95:08E9 SUB 0x142 / NEG / ADD 0x28A: 0x28A + (0x142 - fnum) */
                fnum = (uint16_t)(ADLIB_FNUM_TOP + (ADLIB_FNUM_LOW - fnum));
                v->octave--; // 8-bit, as the driver's DEC DL
            }
            v->fnum = fnum;
            entry = (uint16_t)(fnum | (v->octave << 12));
            v->note_entry = entry;
            _adlib_set_frequency(drv, voice, entry);
            break;
        case 0x03: // 1E95:0972: portamento toward the cached note entry
            _adlib_slide_tick(drv, voice);
            break;
        case 0x0A: { /* 1E95:0874: volume delta; the high nibble adds, 0
                      * subtracts the low nibble */
            uint8_t delta = v->param;
            uint16_t next;
            if ((delta >> 4) != 0u) {
                next = v->volume + (delta >> 4);
                if (next > 0x40u) next = 0x40u;
            } else {
                next = v->volume - (delta & 0x0Fu);
                if ((delta & 0x0Fu) > v->volume) next = 0u;
            }
            v->volume = (uint8_t)next;
            _adlib_set_volume(drv, voice, (uint8_t)next);
            break;
        }
        /* 1E95:0813 / 1E95:0814 - no voice state changes. */
        case 0x00:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        default: break;
    }
}

/**
 * @brief The note-path voice commands, table CS:0x0F8C (1E95:0807).
 *
 * @param drv the driver
 * @param rec the record being stepped (CS:0x135A)
 * @param voice the voice index
 */
static void _note_command(SoundDriver *drv, AdlibRecord *rec, uint32_t voice)
{
    AdlibSlot *v = &drv->slot[voice];

    switch (v->command & 0x1Fu) {
        case 6: // 1E95:0900: note off
            v->command = 0;
            _adlib_key_off(drv, voice);
            break;
        case 0x0B: // 1E95:090F: jump to order parameter-1, advance next tick
            rec->order = (uint8_t)(v->param - 1u);
            rec->advance = (uint8_t)~rec->advance;
            break;
        case 0x0C: // 1E95:091F: volume = min(parameter, 0x40)
            if (v->param > ADLIB_ROWS) v->param = ADLIB_ROWS;
            _adlib_set_volume(drv, voice, v->param);
            break;
        case 0x0D: // 1E95:0907: advance to the next order on the next tick
            rec->advance = (uint8_t)~rec->advance;
            break;
        case 0x0F: // 1E95:0932: ticks per row
            if ((v->param & 0x1Fu) != 0) {
                rec->tick = 0;
                rec->ticks_per_row = (uint8_t)(v->param & 0x1Fu);
            }
            break;
        default: break;
    }
}

/**
 * @brief Play one decoded voice cell (1E95:0796).
 *
 * @param drv the driver
 * @param rec the record being stepped
 * @param voice the voice index
 */
static void _play_cell(SoundDriver *drv, AdlibRecord *rec, uint32_t voice)
{
    AdlibSlot *v = &drv->slot[voice];

    if (v->instrument != 0 && v->note != 0) {
        uint32_t index = (uint32_t)v->instrument - 1u;
        uint8_t volume;
        /* The header holds 64 two-byte entries; the driver trusts the cell,
         * the port bounds it (D-261). */
        if (index >= 64u) index = 63u;
        volume = drv->header[0x99u + 2u * index];
        if (v->command == 0x0Cu) {
            volume = v->param;
            v->command = 0;
            v->param = 0;
        }
        _adlib_set_volume(drv, voice, volume);
        if (v->instrument != v->last_instrument) {
            v->last_instrument = v->instrument;
            _adlib_apply_instrument(drv, voice, (uint32_t)v->instrument - 1u);
        }
    }

    if (v->note == 0) {
        _note_command(drv, rec, voice);
    } else if ((v->command & 0x1Fu) == 3u) {
        _adlib_slide_setup(drv, voice); // command 3's note-path entry is a RET
    } else {
        _note_command(drv, rec, voice);
        _adlib_note_on(drv, voice, v->note);
    }
}

/**
 * @brief The voice index a track slot names (its low three bits).
 *
 * @param rec the record
 * @param i the track index
 * @return the voice index
 */
static uint32_t _record_voice(const AdlibRecord *rec, uint32_t i)
{ return (rec->track_slot[i] & 0x07u); }

/**
 * @brief Run every voice's per-tick command for an active record.
 *
 * 1E95:0613: the record's tracks first, then the five rhythm voices.
 *
 * @param drv the driver
 * @param rec the record
 */
static void _tick_commands(SoundDriver *drv, AdlibRecord *rec)
{
    uint32_t i;

    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++) {
        uint32_t voice = _record_voice(rec, i);
        AdlibSlot *v = &drv->slot[voice];
        if ((v->command | v->param) != 0) _tick_command(drv, voice);
    }
    for (i = 0; i < 5u; i++) {
        uint32_t voice = ADLIB_MELODIC + i;
        AdlibSlot *v = &drv->slot[voice];
        if ((v->command | v->param) != 0) _tick_command(drv, voice);
    }
}

/**
 * @brief Store one 4-byte voice cell, or clear it when the mask bit is clear.
 *
 * @param drv the driver
 * @param voice the voice index
 * @param base the pattern's tune offset
 * @param cursor the stream cursor; advanced by 4 on a set bit
 * @param mask the row mask; its MSB is this cell's bit
 * @return true when the cell was read
 */
static uint8_t _read_cell(SoundDriver *drv, uint32_t voice, uint32_t base,
                          uint16_t *cursor, uint16_t mask)
{
    AdlibSlot *v = &drv->slot[voice];

    if ((mask & 0x8000u) != 0) {
        v->note = _tune_u8(drv, base + *cursor + 0u);
        v->instrument = _tune_u8(drv, base + *cursor + 1u);
        v->command = _tune_u8(drv, base + *cursor + 2u);
        v->param = _tune_u8(drv, base + *cursor + 3u);
        *cursor = (uint16_t)(*cursor + 4u);
        return 1;
    }
    v->note = 0;
    v->instrument = 0;
    v->command = 0;
    v->param = 0;
    return 0;
}

/**
 * @brief Decode one row of the current pattern into the voice cells.
 *
 * 1E95:06ED: mask MSB first.  The track store (CALL 1E95:0774) is bracketed
 * by PUSH DX/PUSH SI ... POP SI/POP DX (bytes `52 56 E8 47 00 5E 5A` at
 * 1E95:0718), so the loop at 1E95:0726 runs on the *restored* mask and cursor:
 * it re-tests bits 15..10 and only walks the cursor over the six track cells,
 * it does not consume new bits.  The five rhythm voices therefore take bits
 * 9..5.  The cursor is pattern-relative (the original's is SDS-based; every
 * pattern starts at SDS, so the difference cancels).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _decode_row(SoundDriver *drv, AdlibRecord *rec)
{
    uint32_t pattern;
    uint32_t base;
    uint16_t mask;
    uint16_t row_mask;
    uint16_t cells; // the post-mask cursor
    uint32_t i;

    if (rec->order >= 0x7Fu) {
        rec->row = ADLIB_ROWS; // the order list is 0x7F bytes
        return;
    }
    pattern = drv->header[0x18u + rec->order];
    if (pattern >= drv->offset_count) {
        rec->row = ADLIB_ROWS;
        return;
    }
    base = drv->song_data +
           ((uint32_t)drv->offset[pattern] - drv->offset[0]) * 16u;
    mask = _tune_u16(drv, base + rec->cursor);
    rec->cursor = (uint16_t)(rec->cursor + 2u);
    cells = rec->cursor; // 1E95:071C: SI after LODSW
    row_mask = mask;

    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++) {
        (void)_read_cell(drv, _record_voice(rec, i), base, &rec->cursor, mask);
        mask = (uint16_t)(mask << 1);
    }
    /* 1E95:0726: mask and cursor are both back at the post-mask position, so
     * this walks the cursor over the track cells again without consuming a new
     * bit.  header[0x118] is the track count, not a distinct cell group. */
    mask = row_mask;
    rec->cursor = cells;
    for (i = 0; i < drv->header[0x118u]; i++) {
        if ((mask & 0x8000u) != 0) rec->cursor = (uint16_t)(rec->cursor + 4u);
        mask = (uint16_t)(mask << 1);
    }
    for (i = 0; i < 5u; i++) {
        (void)_read_cell(drv, ADLIB_MELODIC + i, base, &rec->cursor, mask);
        mask = (uint16_t)(mask << 1);
    }
    rec->row++;
}

/**
 * @brief Play the cells the row decoder just filled (1E95:0663).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _play_row(SoundDriver *drv, AdlibRecord *rec)
{
    uint32_t i;

    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++)
        _play_cell(drv, rec, _record_voice(rec, i));
    for (i = 0; i < 5u; i++) _play_cell(drv, rec, ADLIB_MELODIC + i);
}

void _adlib_release_record(SoundDriver *drv, AdlibRecord *rec)
{
    uint32_t i;

    if (!rec->active) return;
    rec->active = 0;
    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++) {
        uint32_t slot = _record_voice(rec, i);
        drv->slot_free[slot] = (uint8_t)slot;
        _adlib_silence(drv, slot);
    }
}

/**
 * @brief Advance a record to the next order (1E95:06B1).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _advance_order(SoundDriver *drv, AdlibRecord *rec)
{
    rec->tick = 0;
    rec->advance = 0;
    rec->row = 0;
    rec->cursor = 0;
    rec->order++;
    if (rec->order != rec->last_order) return;
    rec->finished = 0xFF;
    if (rec->loop) {
        rec->order = rec->first_order;
        return;
    }
    _adlib_release_record(drv, rec);
}

/**
 * @brief End-of-row bookkeeping (1E95:06A7).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _row_end(SoundDriver *drv, AdlibRecord *rec)
{
    if (rec->row == ADLIB_ROWS) _advance_order(drv, rec);
    if (rec->active && rec->advance) _advance_order(drv, rec);
}

void _adlib_step_record(SoundDriver *drv, AdlibRecord *rec)
{
    if (!rec->active) return;
    rec->tick++;
    if (rec->tick < rec->ticks_per_row) {
        _tick_commands(drv, rec);
        return;
    }
    rec->tick = 0;
    _decode_row(drv, rec);
    _play_row(drv, rec);
    _row_end(drv, rec);
}
