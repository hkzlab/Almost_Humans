/* The speaker side: the PIT channel-2 divider, port 0x61 and the slot-to-
 * speaker hand-off.  The record layout and the shared helpers are in
 * beeper_internal.h.
 */
#include "audio/beeper/beeper_internal.h"

#include "byteorder.h"
#include "thirdparty/pcspeaker_impulse.h"

#include <stdint.h>

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

void _beeper_set_divider(SoundDriver *drv, uint16_t divider)
{
    drv->divider = divider;
    pcspeaker_impulse_set_counter(drv->speaker, divider);
}

void _beeper_speaker_on(SoundDriver *drv)
{
    if (drv->speaker_on) return;
    drv->speaker_on = 1;
    /* BEEPER.DRV order (1E95:07ED): port 0x61 bits 0-1, then 0x43 = 0xB6. */
    pcspeaker_impulse_set_speaker(drv->speaker, 1);
    pcspeaker_impulse_set_control(drv->speaker);
}

void _beeper_speaker_off(SoundDriver *drv)
{
    drv->speaker_on = 0;
    /* 1E95:0809 clears port 0x61, then writes 0x43 = 0xB6. */
    pcspeaker_impulse_set_speaker(drv->speaker, 0);
    pcspeaker_impulse_set_control(drv->speaker);
}

void _beeper_set_note_frequency(SoundDriver *drv, uint32_t slot, uint8_t note)
{
    uint32_t idx = note;

    if (idx >= 84u) idx = 83u; // shortcut: the original reads past its table
    drv->slot_flag[slot] = 0xFFu;
    wr_le_u16(&drv->slot[slot][BEEP_DIVIDER], _period[idx]);
}

void _beeper_switch_slot(SoundDriver *drv, uint32_t slot)
{
    uint16_t divider = rd_le_u16(&drv->slot[slot][BEEP_DIVIDER]);

    if (drv->slot_flag[slot] == 0xFFu) {
        drv->slot_flag[slot] = 0xFEu;
        _beeper_speaker_on(drv);
        _beeper_set_divider(drv, divider);
    } else {
        drv->slot_flag[slot] = 0;
        _beeper_speaker_off(drv);
    }
}
