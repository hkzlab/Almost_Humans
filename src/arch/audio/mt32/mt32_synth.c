/* The MT-32 synth side: the MIDI/sysex stream to mt32emu, the slots' note
 * and pitch-bend state, the timbre dump and the two resets.  The slot/record
 * layouts and the shared helpers are in mt32_internal.h.
 */
#include "audio/mt32/mt32_internal.h"

#include <stdint.h>
#include <string.h>

/* CS:0x12A0, the semitone period table 0x12A0..0x1377 (108 words); only
 * mt32_slide_setup (0x0BCD) reads it. */
static const uint16_t _period[108] = {
    6848, 6464, 6096, 5760, 5424, 5120, 4832, 4560, 4304, 4064, 3840, 3616,
    3424, 3232, 3048, 2880, 2712, 2560, 2416, 2280, 2152, 2032, 1920, 1808,
    1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960,  904,
    856,  808,  762,  720,  678,  640,  604,  570,  538,  508,  480,  452,
    428,  404,  381,  360,  339,  320,  302,  285,  269,  254,  240,  226,
    214,  202,  190,  180,  169,  160,  151,  142,  134,  127,  120,  113,
    107,  101,  95,   90,   84,   80,   75,   71,   67,   63,   60,   56,
    53,   50,   47,   45,   42,   40,   37,   35,   33,   31,   30,   28,
    26,   25,   23,   22,   21,   20,   18,   17,   16,   15,   15,   14
};

void _mt32_midi(SoundDriver *drv, uint8_t status, uint8_t d1, uint8_t d2)
{
    uint8_t m[3];

    m[0] = status;
    m[1] = (uint8_t)(d1 & 0x7Fu);
    m[2] = (uint8_t)(d2 & 0x7Fu);
    mt32emu_parse_stream(drv->ctx, m, 3);
}

void _mt32_program(SoundDriver *drv, uint8_t channel, uint8_t program)
{
    uint8_t m[2];

    m[0] = (uint8_t)(0xC0u + channel);
    m[1] = (uint8_t)(program & 0x7Fu);
    mt32emu_parse_stream(drv->ctx, m, 2);
}

void _mt32_sysex(SoundDriver *drv, uint8_t a0, uint8_t a1, uint8_t a2,
                 const uint8_t *data, uint32_t n)
{
    uint8_t buf[8u + 0x3Au + 2u];
    uint32_t i;
    uint8_t sum = 0;

    buf[0] = 0xF0u;
    buf[1] = 0x41u; // Roland
    buf[2] = 0x10u; // unit 0x10
    buf[3] = 0x16u; // MT-32 model
    buf[4] = 0x12u; // DT1
    buf[5] = a0;
    buf[6] = a1;
    buf[7] = a2;
    sum = (uint8_t)(sum + a0 + a1 + a2);
    for (i = 0; i < n; i++) buf[8u + i] = data[i];
    for (i = 0; i < n; i++) sum = (uint8_t)(sum + data[i]);
    buf[8u + n] = (uint8_t)((0u - (uint32_t)sum) & 0x7Fu);
    buf[8u + n + 1u] = 0xF7u;
    mt32emu_parse_stream(drv->ctx, buf, n + 10u);
}

void _mt32_send_master_volume(SoundDriver *drv)
{
    uint8_t data = drv->volume;
    _mt32_sysex(drv, 0x10u, 0x00u, 0x16u, &data, 1u);
}

void _mt32_all_notes_off(SoundDriver *drv, uint32_t channel)
{
    uint8_t status = (uint8_t)(0xB0u + channel);

    _mt32_midi(drv, status, 0x7Bu, 0x00u);
    _mt32_midi(drv, status, 0x79u, 0x00u);
    _mt32_midi(drv, status, 0x07u, 0x7Fu);
    _mt32_midi(drv, status, 0x0Au, 0x40u);
}

void _mt32_send_bend(SoundDriver *drv, uint32_t channel, const Mt32Slot *slot)
{
    _mt32_midi(drv, (uint8_t)(0xE0u + channel), (uint8_t)(slot->bend & 0x7Fu),
               (uint8_t)((slot->bend >> 7) & 0x7Fu));
}

void _mt32_note_on(SoundDriver *drv, uint32_t slot_no, uint8_t note)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint32_t ch = _channel(slot_no);

    if (note == 0) return;
    if (s->bend != 0x2000u) {
        s->bend = 0x2000u;
        _mt32_send_bend(drv, ch, s);
    }
    _mt32_midi(drv, (uint8_t)(0xB0u + ch), 0x7Bu, 0x00u);
    s->last_note = note;
    _mt32_midi(drv, (uint8_t)(0x90u + ch), note, 0x7Fu);
}

void _mt32_note_on_extra(SoundDriver *drv, uint32_t slot_no, uint8_t note)
{
    if (note == 0) return;
    _mt32_midi(drv, (uint8_t)(0x90u + _channel(slot_no)), note, 0x7Fu);
}

/**
 * @brief The period-table bias for a note (0x0BCD).
 *
 * @param note the note number
 * @return the period; out-of-range reads the last entry
 */
static uint16_t _period_of(uint8_t note)
{
    if (note >= 108u) return _period[107];
    return _period[note];
}

void _mt32_slide_setup(SoundDriver *drv, uint32_t slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint16_t target = _period_of(s->note[0]);
    uint16_t current;

    s->slide_note = s->note[0];
    s->slide_target = target;
    s->slide_acc = _period_of(s->last_note);
    current = s->slide_acc;
    s->slide_dir = 0;
    if (target == current) {
        s->slide_target = 0;
        return;
    }
    if (target > current) s->slide_dir = 1;
}

void _mt32_slide_tick(SoundDriver *drv, uint32_t slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint16_t step;

    if (s->param != 0) {
        s->slide_step = s->param;
        s->param = 0;
    }
    if (s->slide_target == 0) return;
    step = s->slide_step;
    if (s->slide_dir == 0) {
        s->slide_acc = (uint16_t)(s->slide_acc + step);
        s->bend = (uint16_t)(s->bend - (uint16_t)(step << 5));
        if (s->slide_acc < s->slide_target) {
            _mt32_send_bend(drv, _channel(slot_no), s);
            return;
        }
    } else {
        uint16_t before = s->slide_acc;
        s->slide_acc = (uint16_t)(s->slide_acc - step);
        if (s->slide_acc > before) { // borrow: the accumulator hit zero
            s->slide_target = 0;
            _mt32_note_on(drv, slot_no, s->slide_note);
            return;
        }
        s->bend = (uint16_t)(s->bend + (uint16_t)(step << 5));
        if (s->slide_acc > s->slide_target) {
            _mt32_send_bend(drv, _channel(slot_no), s);
            return;
        }
    }
    s->slide_target = 0;
    _mt32_note_on(drv, slot_no, s->slide_note);
}

/**
 * @brief Send one timbre block as six DT1 messages (0x06A7).
 *
 * The caller advances the bank pointer past every flagged instrument, so the
 * block offset is passed in.  A block whose first common byte is a space is a
 * placeholder and is not sent (0x069B returns straight away).
 *
 * @param drv the driver
 * @param index the instrument index (0-based), the timbre-memory number
 * @param block the byte offset of the 256-byte block in the tune
 */
static void _send_timbre(SoundDriver *drv, uint32_t index, uint32_t block)
{
    uint8_t common[8];
    uint8_t data[0x3A];
    uint8_t hi = (uint8_t)((index * 8u) >> 7);
    uint8_t lo = (uint8_t)((index * 8u) & 0x7Fu);
    uint8_t mid = (uint8_t)(index * 2u);
    uint8_t low_addr = 0x0Au;
    uint32_t i;

    for (i = 0; i < 8u; i++) common[i] = _u8(drv, block + i);
    if (common[0] == 0x20u) return;

    memcpy(data, common, 8);
    if (common[0] != 0x03u) data[1] = (uint8_t)index;
    _mt32_sysex(drv, 0x05u, hi, lo, data, 8u);

    for (i = 0; i < 4u; i++) data[i] = _u8(drv, block + 0x12u + i);
    _mt32_sysex(drv, 0x08u, mid, low_addr, data, 4u);
    low_addr = (uint8_t)(low_addr + 4u);

    for (i = 0; i < 4u; i++) {
        uint32_t j;
        for (j = 0; j < 0x3Au; j++)
            data[j] = _u8(drv, block + 0x16u + i * 0x3Au + j);
        _mt32_sysex(drv, 0x08u, mid, low_addr, data, 0x3Au);
        low_addr = (uint8_t)(low_addr + 0x3Au);
        if (low_addr > 0x7Fu) {
            low_addr = (uint8_t)(low_addr & 0x7Fu);
            mid++;
        }
    }
}

void _mt32_init_timbres(SoundDriver *drv)
{
    uint32_t block = 0;
    uint32_t i;

    for (i = 0; i < 64u; i++) {
        if (drv->instruments[2u * i] == 0) continue;
        _send_timbre(drv, i, drv->bank_base + block * 0x100u);
        block++;
    }
    /* the driver's final reset: programs 0..63 on channel 2 (part 1) */
    for (i = 0; i < 64u; i++) _mt32_program(drv, 1u, (uint8_t)i);
}

void _mt32_reset_voices(SoundDriver *drv)
{
    /* CS:0x0EAD's data: address 10 00 04, then the 8 bytes the routine copies
     * from CS:0x0F0E (entry 7), then the image's trailing zero. */
    static const uint8_t system[9] = { 0x04, 0x04, 0x04, 0x04, 0x04,
                                       0x04, 0x04, 0x04, 0x00 };
    uint32_t i;

    _mt32_send_master_volume(drv);
    memset(drv->record, 0, sizeof(drv->record));
    for (i = 0; i < MT32_SLOTS; i++) drv->slot_free[i] = (uint8_t)i;
    _mt32_sysex(drv, 0x10u, 0x00u, 0x04u, system, sizeof(system));
    for (i = 1; i <= 8u; i++) _mt32_all_notes_off(drv, i);
}

void _mt32_install_reset(SoundDriver *drv)
{
    static const uint8_t system[12] = { 0x00, 0x00, 0x00, 0x08, 0x08, 0x08,
                                        0x08, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t zero = 0x00u;
    uint32_t i;

    _mt32_sysex(drv, 0x7Fu, 0x00u, 0x00u, &zero, 1u);
    for (i = 1; i <= 9u; i++) {
        _mt32_midi(drv, (uint8_t)(0xB0u + i), 0x7Bu, 0x00u);
        _mt32_midi(drv, (uint8_t)(0xB0u + i), 0x40u, 0x00u);
        _mt32_midi(drv, (uint8_t)(0xE0u + i), 0x00u, 0x40u);
    }
    _mt32_sysex(drv, 0x10u, 0x00u, 0x01u, system, sizeof(system));
}
