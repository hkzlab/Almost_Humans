/* The MT-32 sequencer: both command tables, the class-payload row decoder
 * and the record stepper.  The MIDI stream they drive is in mt32_synth.c.
 */
#include "audio/mt32/mt32_internal.h"

#include <stdint.h>
#include <string.h>

/**
 * @brief The CC 7 value for a 0..0x40 volume: double it, clamp at 0x7F.
 *
 * @param volume the slot volume
 * @return the 7-bit controller value
 */
static uint8_t _cc_volume(uint8_t volume)
{
    uint32_t value = (uint32_t)volume << 1;
    return (uint8_t)(value > 0x7Fu ? 0x7Fu : value);
}

/**
 * @brief The per-tick commands, table CS:0x1378 (0x0A02).
 *
 *  00 0x0A28 RET          01 0x0A82 bend up by param<<5
 *  02 0x0AAE bend down    03 0x0C05 slide tick
 *  09 0x0A29 volume delta (high nibble adds, low nibble subtracts)
 *  every other index 0x0A27 RET
 *
 * @param p the play state
 * @param slot_no the slot
 */
static void _tick_command(Mt32Play *p, uint32_t slot_no)
{
    SoundDriver *drv = p->drv;
    Mt32Slot *s = &drv->slot[slot_no];

    if ((uint8_t)(s->command | s->param) == 0) return;
    switch (s->command & 0x1Fu) {
        case 0x01: // 0x0A82: bend += param * 0x20, then send
            s->bend = (uint16_t)(s->bend + (uint16_t)(s->param << 5));
            _mt32_send_bend(drv, _channel(slot_no), s);
            break;
        case 0x02: // 0x0AAE: bend -= param * 0x20, then send
            s->bend = (uint16_t)(s->bend - (uint16_t)(s->param << 5));
            _mt32_send_bend(drv, _channel(slot_no), s);
            break;
        case 0x03: /* 0x0C05: slide */ _mt32_slide_tick(drv, slot_no); break;
        case 0x09: { // 0x0A29: volume delta, then CC 7 = volume * 2
            uint8_t high = (uint8_t)(s->param >> 4);
            uint8_t low = (uint8_t)(s->param & 0x0Fu);
            uint32_t ch = _channel(slot_no);
            uint8_t value;
            if (high != 0) {
                uint32_t next = (uint32_t)s->volume + high;
                s->volume = (uint8_t)(next > 0x40u ? 0x40u : next);
            } else if (low > s->volume) {
                s->volume = 0;
            } else {
                s->volume = (uint8_t)(s->volume - low);
            }
            value = s->volume;
            if (drv->track_volume[ch] == value) return;
            drv->track_volume[ch] = value;
            _mt32_midi(drv, (uint8_t)(0xB0u + ch), 0x07u, _cc_volume(value));
            break;
        }
        default: break;
    }
}

/**
 * @brief Note command 6 (0x0AC2): all notes off on the slot's part.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
void _cmd_all_notes_off(SoundDriver *drv, uint32_t slot_no)
{
    drv->slot[slot_no].command = 0;
    _mt32_midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x7Bu, 0x00u);
}

/**
 * @brief Note command 5 (0x0B04): pan, CC 0x0A.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _cmd_pan(SoundDriver *drv, uint32_t slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint8_t panic = s->param;
    uint8_t value;

    if ((panic & 0xF0u) != 0) {
        value = (uint8_t)(((panic & 0xF0u) >> 2) + 0x40u);
    } else if ((panic & 0x0Fu) != 0) {
        value = (uint8_t)(0x40u - (uint8_t)((panic & 0x0Fu) << 2));
    } else {
        s->command = 0;
        _mt32_midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x0Au, 0x40u);
        return;
    }
    _mt32_midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x0Au, value);
}

/**
 * @brief Note command 4 (0x0C70): modulation, CC 1.
 *
 * The value is (cell note 4 & 0x0F) << 3 - the original reads SI+3 (the
 * fourth chord note), not the parameter byte.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _cmd_modulation(SoundDriver *drv, uint32_t slot_no)
{
    uint8_t value = (uint8_t)((drv->slot[slot_no].note[3] & 0x0Fu) << 3);
    _mt32_midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x01u, value);
}

/**
 * @brief Note commands 7/8 (0x0AD7/0x0AE3): the 10 00 01 system word.
 *
 * The data bytes live in the driver's CS:0x0EAA block and are not rebuilt:
 * command 7 writes the mode byte only, command 8 the two reverb nibbles only,
 * so the mode set by command 7 survives into command 8's message.
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param nibbles true for command 8's two nibbles, false for command 7
 */
static void _cmd_sysex_system(SoundDriver *drv, uint32_t slot_no, int nibbles)
{
    Mt32Slot *s = &drv->slot[slot_no];

    s->command = 0;
    if (nibbles) {
        drv->system_block[1] = (uint8_t)(s->param & 0x0Fu);
        drv->system_block[2] = (uint8_t)(s->param >> 4);
    } else {
        drv->system_block[0] = s->param;
    }
    _mt32_sysex(drv, 0x10u, 0x00u, 0x01u, drv->system_block, 3u);
}

/**
 * @brief Note commands, table CS:0x13B8 (0x0A19).
 *
 * @param p the play state
 * @param slot_no the slot
 */
static void _note_command(Mt32Play *p, uint32_t slot_no)
{
    SoundDriver *drv = p->drv;
    Mt32Slot *s = &drv->slot[slot_no];
    Mt32Record *rec = p->rec;

    switch (s->command & 0x1Fu) {
        case 4: /* 0x0C70 modulation */ _cmd_modulation(drv, slot_no); break;
        case 5: /* 0x0B04 pan */ _cmd_pan(drv, slot_no); break;
        case 6: /* 0x0AC2 all notes off */
            _cmd_all_notes_off(drv, slot_no);
            break;
        case 7: /* 0x0AD7 sysex A */ _cmd_sysex_system(drv, slot_no, 0); break;
        case 8: /* 0x0AE3 sysex B */ _cmd_sysex_system(drv, slot_no, 1); break;
        case 0x0B: // 0x0B44 jump: order = param-1, advance <- NOT advance
            rec->order = (uint8_t)(s->param - 1u);
            rec->advance = (uint8_t)~rec->advance;
            break;
        case 0x0C: // 0x0B54 volume = min(param, 0x40), then CC 7
            if (s->param >= 0x40u) s->param = 0x40u;
            s->volume = s->param;
            if (drv->track_volume[_channel(slot_no)] != s->volume) {
                drv->track_volume[_channel(slot_no)] = s->volume;
                _mt32_midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x07u,
                           _cc_volume(s->volume));
            }
            break;
        case 0x0D: // 0x0B3C advance <- NOT advance
            rec->advance = (uint8_t)~rec->advance;
            break;
        case 0x0E: // 0x0891 mark the record finished
            rec->finished = 0xFFu;
            break;
        case 0x0F: // 0x0B67 ticks per row, param & 0x1F, 0 ignored
            if ((s->param & 0x1Fu) != 0) {
                rec->tick = 0;
                rec->ticks_per_row = (uint8_t)(s->param & 0x1Fu);
            }
            break;
        default: break;
    }
}

/**
 * @brief Play one decoded slot cell (0x0949).
 *
 * @param drv the driver
 * @param rec the record being stepped
 * @param slot_no the slot
 */
static void _play_cell(SoundDriver *drv, Mt32Record *rec, uint32_t slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    Mt32Play p;
    uint32_t ch = _channel(slot_no);

    p.drv = drv;
    p.rec = rec;

    if (s->instrument != 0) {
        uint8_t any_note =
            (s->note[0] | s->note[1] | s->note[2] | s->note[3]) != 0;
        drv->command_block[slot_no] = s->instrument;
        if (any_note) {
            uint32_t index = (uint32_t)s->instrument - 1u;
            uint8_t volume;
            if (index >= 64u) index = 63u;
            volume = drv->instruments[2u * index + 1u]; // header high byte
            volume = (uint8_t)(volume << 1);
            if (volume >= 0x80u) volume = 0x7Fu;
            if (s->command == 0x0Cu) {
                volume = (uint8_t)(s->param << 1);
                if (volume >= 0x80u) volume = 0x7Fu;
                s->command = 0;
                s->param = 0;
            }
            if (drv->track_volume[ch] != volume) {
                drv->track_volume[ch] = volume;
                _mt32_midi(drv, (uint8_t)(0xB0u + ch), 0x07u, volume);
            }
        }
        if (s->instrument - 1u != s->last_program) {
            s->last_program = (uint8_t)(s->instrument - 1u);
            _mt32_program(drv, (uint8_t)ch, s->last_program);
        }
    }

    if ((s->note[0] | s->note[1] | s->note[2] | s->note[3]) == 0) {
        _note_command(&p, slot_no);
        return;
    }
    if ((s->command & 0x1Fu) == 3u) {
        _mt32_slide_setup(drv, slot_no);
        return;
    }
    _note_command(&p, slot_no);
    _mt32_note_on(drv, slot_no, s->note[0]);
    _mt32_note_on_extra(drv, slot_no, s->note[1]);
    _mt32_note_on_extra(drv, slot_no, s->note[2]);
    _mt32_note_on_extra(drv, slot_no, s->note[3]);
}

/**
 * @brief The row's class payload sizes, CS:0x13F8 = {0,2,2,7}.
 *
 * @param cls the 2-bit class
 * @return the payload byte count
 */
static uint32_t _class_size(uint32_t cls)
{
    static const uint8_t size[4] = { 0, 2, 2, 7 };
    return size[cls & 3u];
}

/**
 * @brief Store one class's payload in a slot cell (0x0896's track loop).
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param cursor the tune cursor, advanced by the payload
 * @param cls the class
 */
static void _store_cell(SoundDriver *drv, uint32_t slot_no, uint32_t *cursor,
                        uint32_t cls)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint32_t c = *cursor;

    switch (cls & 3u) {
        case 0:
            memset(s->note, 0, 4);
            s->instrument = 0;
            s->command = 0;
            s->param = 0;
            break;
        case 1:
            s->note[0] = s->note[1] = s->note[2] = s->note[3] = 0;
            s->instrument = 0;
            s->command = _u8(drv, c);
            s->param = _u8(drv, c + 1u);
            *cursor = c + 2u;
            break;
        case 2:
            s->note[0] = _u8(drv, c);
            s->note[1] = s->note[2] = s->note[3] = 0;
            s->instrument = _u8(drv, c + 1u);
            s->command = 0;
            s->param = 0;
            *cursor = c + 2u;
            break;
        default:
            s->note[0] = _u8(drv, c);
            s->note[1] = _u8(drv, c + 1u);
            s->note[2] = _u8(drv, c + 2u);
            s->note[3] = _u8(drv, c + 3u);
            s->instrument = _u8(drv, c + 4u);
            s->command = _u8(drv, c + 5u);
            s->param = _u8(drv, c + 6u);
            *cursor = c + 7u;
            break;
    }
}

/**
 * @brief Decode one row of the current pattern into the slot cells (0x0896).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _decode_row(SoundDriver *drv, Mt32Record *rec)
{
    uint32_t pattern;
    uint32_t base;
    uint32_t cursor;
    uint32_t post_mask;
    uint16_t mask;
    uint16_t original;
    uint32_t i;

    rec->row++;
    if (rec->order >= 0x7Fu) {
        rec->row = MT32_ROWS;
        return;
    }
    pattern = drv->order_list[rec->order];
    if (pattern >= drv->offset_count) {
        rec->row = MT32_ROWS;
        return;
    }
    base = drv->off_base + (uint32_t)drv->offset[pattern] * 16u;
    cursor = base + rec->cursor;
    mask = _u16(drv, cursor);
    original = mask;
    cursor += 2u;
    post_mask = cursor;

    for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++) {
        uint32_t cls = (uint32_t)(mask >> 14) & 3u;
        mask = (uint16_t)(mask << 2);
        _store_cell(drv, rec->track_slot[i] & 7u, &cursor, cls);
    }
    /* 0x0920: the cursor is restored to the post-mask byte and the driver
     * re-sums the first header[0x118] classes of the same mask - the file
     * encodes exactly that many classes, and their sizes are the payload. */
    cursor = post_mask;
    mask = original;
    for (i = 0; i < drv->trailing; i++) {
        cursor += _class_size((uint32_t)(mask >> 14) & 3u);
        mask = (uint16_t)(mask << 2);
    }
    rec->cursor = (uint16_t)(cursor - base);
}

/**
 * @brief Play the cells the row decoder just filled (0x0810's second loop).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _play_row(SoundDriver *drv, Mt32Record *rec)
{
    uint32_t i;

    for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++)
        _play_cell(drv, rec, rec->track_slot[i] & 7u);
}

void _mt32_release_record(SoundDriver *drv, Mt32Record *rec)
{
    uint32_t i;

    if (!rec->active) return;
    rec->active = 0;
    for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++) {
        uint32_t slot = rec->track_slot[i] & 7u;
        drv->slot_free[slot] = (uint8_t)slot;
        _mt32_all_notes_off(drv, _channel(slot));
    }
}

/**
 * @brief The end-of-pattern block (0x0856) and the advance gate (0x0889).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _end_of_pattern(SoundDriver *drv, Mt32Record *rec)
{
    rec->tick = 0;
    rec->advance = 0;
    rec->row = 0;
    rec->cursor = 0;
    rec->order++;
    if (rec->order != rec->last_order) return;
    rec->finished = 0xFFu;
    if (rec->loop) {
        rec->order = rec->first_order;
        return;
    }
    _mt32_release_record(drv, rec);
}

void _mt32_step_record(SoundDriver *drv, Mt32Record *rec)
{
    uint32_t i;

    if (!rec->active) return;
    rec->tick++;
    if (rec->tick < rec->ticks_per_row) {
        Mt32Play p;
        p.drv = drv;
        p.rec = rec;
        for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++)
            _tick_command(&p, rec->track_slot[i] & 7u);
        return;
    }
    rec->tick = 0;
    _decode_row(drv, rec);
    _play_row(drv, rec);
    if (rec->row == MT32_ROWS) _end_of_pattern(drv, rec);
    if (rec->active && rec->advance) _end_of_pattern(drv, rec);
}
