/* The OPL side: the register stream, the note table and the eleven voices'
 * chip state.  The slot/record layouts and the shared helpers are in
 * adlib_internal.h.
 */
#include "audio/adlib/adlib_internal.h"

#include <stdint.h>

/* The OPL operator each voice's level/ADSR/waveform registers target.
 * _op_carrier is CS:0x10D1, _op_modulator is CS:0x10DC. */
static const uint8_t _op_carrier[ADLIB_VOICES] = { 0x03, 0x04, 0x05, 0x0B,
                                                   0x0C, 0x0D, 0x13, 0x14,
                                                   0x12, 0x15, 0x11 };
static const uint8_t _op_modulator[ADLIB_VOICES] = { 0x00, 0x01, 0x02, 0x08,
                                                     0x09, 0x0A, 0x10, 0x14,
                                                     0x12, 0x15, 0x11 };
/* CS:0x10CC: the OPL2 rhythm key bits for voices 6..10 (BD, SD, TOM, CYM, HH). */
static const uint8_t _rhythm_bit[5] = { 0x10, 0x08, 0x04, 0x02, 0x01 };

/* CS:0x0E8C read as the driver reads it: one word per byte index, no bounds.
 * Entries 0..95 are the note table (8 octaves x 12 notes, F-number |
 * (octave << 12)).  The index is always a byte, so a wrapped one (a melodic
 * note below 12) reads whatever follows the table in the image: the command
 * handler tables at CS:0x0F4C / CS:0x0F8C and the OPL init state at
 * CS:0x0FCC, words 96..255 here. */
static const uint16_t _note_table[256] = {
    0x0157, 0x016C, 0x0181, 0x0198, 0x01B1, 0x01CB, 0x01E6, 0x0203, 0x0222,
    0x0243, 0x0266, 0x028A, 0x1157, 0x116C, 0x1181, 0x1198, 0x11B1, 0x11CB,
    0x11E6, 0x1203, 0x1222, 0x1243, 0x1266, 0x128A, 0x2157, 0x216C, 0x2181,
    0x2198, 0x21B1, 0x21CB, 0x21E6, 0x2203, 0x2222, 0x2243, 0x2266, 0x228A,
    0x3157, 0x316C, 0x3181, 0x3198, 0x31B1, 0x31CB, 0x31E6, 0x3203, 0x3222,
    0x3243, 0x3266, 0x328A, 0x4157, 0x416C, 0x4181, 0x4198, 0x41B1, 0x41CB,
    0x41E6, 0x4203, 0x4222, 0x4243, 0x4266, 0x428A, 0x5157, 0x516C, 0x5181,
    0x5198, 0x51B1, 0x51CB, 0x51E6, 0x5203, 0x5222, 0x5243, 0x5266, 0x528A,
    0x6157, 0x616C, 0x6181, 0x6198, 0x61B1, 0x61CB, 0x61E6, 0x6203, 0x6222,
    0x6243, 0x6266, 0x628A, 0x7157, 0x716C, 0x7181, 0x7198, 0x71B1, 0x71CB,
    0x71E6, 0x7203, 0x7222, 0x7243, 0x7266, 0x728A, 0x0814, 0x08AA, 0x08D4,
    0x0972, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0874, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0A04, 0x0813, 0x0900,
    0x0813, 0x0813, 0x0813, 0x0813, 0x090F, 0x091F, 0x0907, 0x0813, 0x0932,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x2000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0101, 0x0101, 0x0101, 0x0000,
    0x0101, 0x0101, 0x0101, 0x0000, 0x0101, 0x0101, 0x0101, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x3F3F, 0x3F3F, 0x3F3F, 0x0000, 0x3F3F, 0x3F3F,
    0x3F3F, 0x0000, 0x3F3F, 0x3F3F, 0x3F3F, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000,
    0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF0F0,
    0xF0F0, 0xF0F0, 0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0xF0F0, 0xF0F0,
    0xF0F0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x2000, 0x0000
};

/* CS:0x0FCC: the 256-byte OPL state the probe installs.  The non-zero
 * entries are reg 0x01 = 0x20 (waveform select enable); MULT = 1 on all 18
 * operators (0x20..0x25, 0x28..0x2D, 0x30..0x35); every level at 0x3F
 * (0x40 + the same three ranges); AR/DR = 0xF0 and SR/RR = 0xF0 on those
 * ranges at 0x60/0x70 and 0x80/0x90; and reg 0xBD = 0x20, rhythm mode
 * enabled with no drum keyed. */
static const uint8_t _opl_init[256] = {
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x00, 0x00,
    0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x3F,
    0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x00, 0x00, 0xF0, 0xF0, 0xF0, 0xF0,
    0xF0, 0xF0, 0x00, 0x00, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xF0, 0xF0, 0xF0,
    0xF0, 0xF0, 0x00, 0x00, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x00, 0x00,
    0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

void _adlib_write_reg(SoundDriver *drv, uint8_t reg, uint8_t value)
{
    drv->shadow[reg] = value;
    OPL3_WriteReg(&drv->opl, reg, value);
}

/**
 * @brief Install the probe's 256-byte OPL state (1E95:0DDF).
 *
 * Copies the image's state into the shadow, writing every register on the way.
 *
 * @param drv the driver
 */
void _adlib_opl_probe_reset(SoundDriver *drv)
{
    uint32_t i;

    for (i = 0; i < 256u; i++) _adlib_write_reg(drv, (uint8_t)i, _opl_init[i]);
}

/**
 * @brief The note-table entry at a raw byte index (the rhythm voices' indexing).
 *
 * The original's index register is always a byte (`MOV BL,AL / XOR BH,BH`), so
 * this reads `[BX+0x0E8C]` for all 256 values with no range test.
 *
 * @param idx the table index, 0..255
 * @return the F-number | octave word
 */
static uint16_t _note_raw(uint8_t idx) { return _note_table[idx]; }

/**
 * @brief The note-table entry for a melodic note (note 12 = entry 0).
 *
 * The subtraction is a byte operation in the original (`SUB AL,0x0C`), so a
 * note below 12 wraps to index 244..255 instead of reading the bottom entry.
 *
 * @param note the note number
 * @return the F-number | octave word
 */
static uint16_t _note_entry(uint8_t note)
{ return _note_raw((uint8_t)(note - 12u)); }

/**
 * @brief The level byte an OPL volume register gets for velocity and volume.
 *
 * 1E95:0B89: 0x3F - (velocity * volume) / 0x3F, integer.
 *
 * @param velocity the instrument's 6-bit loudness byte
 * @param volume the voice volume 0..0x40
 * @return the OPL attenuation 0..0x3F
 */
static uint8_t _level(uint8_t velocity, uint8_t volume)
{
    uint32_t scaled = ((uint32_t)velocity * volume) / 0x3Fu;
    if (scaled > 0x3Fu) scaled = 0x3Fu;
    return (uint8_t)(0x3Fu - scaled);
}

void _adlib_set_frequency(SoundDriver *drv, uint32_t voice, uint16_t entry)
{
    uint8_t hi;
    uint8_t lo;
    uint8_t block;
    uint8_t reg;

    if (voice >= 8u) return;
    hi = (uint8_t)(entry >> 8);
    lo = (uint8_t)(entry & 0xFFu);
    block = (uint8_t)(((hi & 0x70u) >> 2) | (hi & 0x03u));
    reg = (uint8_t)(0xB0u + voice);
    _adlib_write_reg(drv, reg, (uint8_t)((drv->shadow[reg] & 0x20u) | block));
    _adlib_write_reg(drv, (uint8_t)(0xA0u + voice), lo);
}

/**
 * @brief Key a voice on (1E95:0C3A).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _key_on(SoundDriver *drv, uint32_t voice)
{
    if (voice < ADLIB_MELODIC) {
        uint8_t reg = (uint8_t)(0xB0u + voice);
        _adlib_write_reg(drv, reg, (uint8_t)(drv->shadow[reg] | 0x20u));
    } else {
        uint8_t bit = _rhythm_bit[voice - ADLIB_MELODIC];
        _adlib_write_reg(drv, 0xBDu, (uint8_t)(drv->shadow[0xBDu] | bit));
    }
}

void _adlib_key_off(SoundDriver *drv, uint32_t voice)
{
    if (voice < ADLIB_MELODIC) {
        uint8_t reg = (uint8_t)(0xB0u + voice);
        _adlib_write_reg(drv, reg, (uint8_t)(drv->shadow[reg] & 0xDFu));
    } else {
        uint8_t mask = (uint8_t)~_rhythm_bit[voice - ADLIB_MELODIC];
        _adlib_write_reg(drv, 0xBDu, (uint8_t)(drv->shadow[0xBDu] & mask));
    }
}

void _adlib_silence(SoundDriver *drv, uint32_t voice)
{
    uint8_t op;

    if (voice >= ADLIB_VOICES) return;
    /* 1E95:0CD4: clear the cached instrument first, so the next note of this
     * voice reloads its patch even when the instrument number repeats. */
    drv->slot[voice].last_instrument = 0;
    _adlib_key_off(drv, voice);
    drv->voice_volume[voice] = 0;
    op = _op_carrier[voice];
    _adlib_write_reg(drv, (uint8_t)(0x40u + op),
                     (uint8_t)((drv->shadow[0x40u + op] & 0xC0u) | 0x3Fu));
}

void _adlib_apply_tempo(SoundDriver *drv)
{
    uint8_t floor = (uint8_t)(0x3Fu - drv->tempo);
    uint32_t i;

    for (i = 0; i < ADLIB_VOICES; i++) {
        uint8_t reg = (uint8_t)(0x40u + _op_carrier[i]);
        uint8_t b = drv->shadow[reg];
        if ((b & 0x3Fu) >= floor) continue;
        _adlib_write_reg(drv, reg, (uint8_t)((b & 0xC0u) | floor));
    }
}

/**
 * @brief Apply one instrument descriptor block to one operator.
 *
 * The 13-byte block is the image's KSL, MULT, FB, attack, 15-sustain, EG,
 * decay, release, loudness, AM, VIB, KSR, connection layout.  The write order
 * is 1E95:0B6D/0B38/0AA4/0ADF/0AF7/0B13: the channel register, the level, the
 * waveform, then the three ADSR bytes.
 *
 * @param drv the driver
 * @param voice the voice (its channel selects reg 0xC0+voice)
 * @param op the OPL operator offset
 * @param desc the 13-byte descriptor
 * @param level the 6-bit attenuation already computed for reg 0x40
 * @param waveform the 2-bit waveform select
 * @param feedback true when this call also writes reg 0xC0+voice
 */
static void _apply_operator(SoundDriver *drv, uint32_t voice, uint8_t op,
                            const uint8_t *desc, uint8_t level,
                            uint8_t waveform, uint8_t feedback)
{
    uint8_t sustain;

    if (feedback) {
        uint8_t fb = (uint8_t)(((desc[2] & 0x07u) << 1) & 0xFEu);
        _adlib_write_reg(drv, (uint8_t)(0xC0u + voice), fb);
    }
    _adlib_write_reg(drv, (uint8_t)(0x40u + op),
                     (uint8_t)(((desc[0] & 0x03u) << 6) | (level & 0x3Fu)));
    _adlib_write_reg(drv, (uint8_t)(0xE0u + op), (uint8_t)(waveform & 0x03u));
    _adlib_write_reg(drv, (uint8_t)(0x20u + op),
                     (uint8_t)((desc[9] ? 0x80u : 0u) |
                               (desc[10] ? 0x40u : 0u) |
                               (desc[5] ? 0x20u : 0u) |
                               (desc[11] ? 0x10u : 0u) | (desc[1] & 0x0Fu)));
    _adlib_write_reg(drv, (uint8_t)(0x60u + op),
               (uint8_t)(((uint8_t)(desc[3] << 4)) | (desc[6] & 0x0Fu)));
    sustain = (uint8_t)(15u - desc[4]);
    _adlib_write_reg(drv, (uint8_t)(0x80u + op),
                     (uint8_t)(((uint8_t)(sustain << 4)) | (desc[7] & 0x0Fu)));
}

/**
 * @brief Copy an instrument's 30-byte parameter block out of the image.
 *
 * 1E95:0B94: the bank entry is 0x0A + index*0x40 into the patch bank; the
 * first 30 bytes are the modulator block, the carrier block and both
 * waveforms.
 *
 * @param drv the driver
 * @param index the 0-based patch index
 * @param out the 30-byte destination
 */
static void _build_patch(const SoundDriver *drv, uint32_t index, uint8_t *out)
{
    uint32_t off = drv->patch_base + index * 0x40u + 0x0Au;
    uint32_t i;

    for (i = 0; i < 30u; i++) out[i] = _tune_u8(drv, off + i);
}

void _adlib_apply_instrument(SoundDriver *drv, uint32_t voice, uint32_t index)
{
    uint8_t patch[30];
    AdlibSlot *v;
    const uint8_t *mod;
    const uint8_t *car;

    if (voice >= ADLIB_VOICES) return;
    _build_patch(drv, index, patch);
    mod = patch;
    car = patch + 13;
    v = &drv->slot[voice];
    v->mod_atten = mod[8];
    v->car_atten = car[8];

    if (voice <= ADLIB_MELODIC) {
        _apply_operator(drv, voice, _op_modulator[voice], mod,
                        (uint8_t)(0x3Fu - (mod[8] & 0x3Fu)), patch[26], 1);
        _apply_operator(drv, voice, _op_carrier[voice], car,
                        _level(car[8], v->volume), patch[27], 0);
    } else {
        _apply_operator(drv, voice, _op_carrier[voice], mod,
                        _level(mod[8], v->volume), patch[26], 1);
    }
}

void _adlib_set_volume(SoundDriver *drv, uint32_t voice, uint8_t volume)
{
    AdlibSlot *v = &drv->slot[voice];
    uint8_t atten;
    uint8_t reg;

    if (volume >= 0x40u) volume = 0x3Fu; // 1E95:0A05: CMP 0x40 / MOV 0x3F
    if (volume > drv->tempo) volume = drv->tempo;
    if (volume == drv->voice_volume[voice]) return;
    drv->voice_volume[voice] = volume;
    v->volume = volume;
    atten = (voice <= ADLIB_MELODIC) ? v->car_atten : v->mod_atten;
    reg = (uint8_t)(0x40u + _op_carrier[voice]);
    _adlib_write_reg(drv, reg,
                     (uint8_t)((drv->shadow[reg] & 0xC0u) |
                               _level(atten, volume)));
}

/**
 * @brief Land a voice on its slide target's note (1E95:0CE0).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _slide_finish(SoundDriver *drv, uint32_t voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t entry = _note_entry(v->note_copy);

    v->note_entry = entry;
    v->fnum = (uint16_t)(entry & 0x03FFu);
    v->octave = (uint8_t)(entry >> 12);
    _adlib_set_frequency(drv, voice, entry);
}

void _adlib_slide_setup(SoundDriver *drv, uint32_t voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t target = _note_entry(v->note);

    v->note_copy = v->note;
    v->slide_target = target;
    v->slide_dir = 0;
    if (target > v->note_entry) v->slide_dir = 1;
    else v->slide_target = 0;
}

void _adlib_note_on(SoundDriver *drv, uint32_t voice, uint8_t note)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t entry;

    _adlib_key_off(drv, voice);
    if (voice < 7u) {
        entry = _note_entry(note);
        v->note_entry = entry;
        v->fnum = (uint16_t)(entry & 0x03FFu);
        v->octave = (uint8_t)(entry >> 12);
        _adlib_set_frequency(drv, voice, entry);
    } else if (voice == 8u) {
        /* 1E95:0C18: the tom's note also retunes channel 7 by seven steps. */
        _adlib_set_frequency(drv, 8u, _note_raw(note));
        _adlib_set_frequency(drv, 7u, _note_raw((uint8_t)(note + 7u)));
    } else {
        _adlib_set_frequency(drv, voice, _note_raw(note));
    }
    _key_on(drv, voice);
}

void _adlib_slide_tick(SoundDriver *drv, uint32_t voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t step;
    uint16_t entry;

    if (v->param != 0) {
        v->slide_step = v->param;
        v->param = 0;
    }
    if (v->slide_target == 0) return;

    step = (uint16_t)(2u * v->slide_step);
    if (v->slide_dir == 0) {
        uint16_t fnum = (uint16_t)(v->fnum - step);
        while (fnum <= ADLIB_FNUM_LOW) {
            fnum = (uint16_t)(ADLIB_FNUM_TOP + (ADLIB_FNUM_LOW - fnum));
            v->octave--;
        }
        v->fnum = fnum;
    } else {
        uint16_t fnum = (uint16_t)(v->fnum + step);
        while (fnum >= ADLIB_FNUM_WRAP) {
            fnum = (uint16_t)((fnum - ADLIB_FNUM_WRAP) + ADLIB_FNUM_FIRST);
            v->octave++;
        }
        v->fnum = fnum;
    }
    entry = (uint16_t)(v->fnum | (v->octave << 12));
    v->note_entry = entry;
    if (v->slide_dir == 0 ? entry > v->slide_target : entry < v->slide_target) {
        _adlib_set_frequency(drv, voice, entry);
        return;
    }
    v->slide_target = 0;
    _slide_finish(drv, voice);
}
