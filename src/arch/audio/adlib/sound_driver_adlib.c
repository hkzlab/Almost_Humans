/* arch/audio/adlib/sound_driver_adlib.c - ADLIB.DRV's replacement.
 *
 * The resident driver is 6 KB of position-independent 8086 code (no static
 * image: it is a CD resource loaded into a DOS-allocated segment, so every
 * address below is a segment-relative offset of the shipped image).  This
 * file is that image's behaviour, read from the disassembly on 2026-11-06:
 *
 *   INT 60h            ADLIB.DRV                    here
 *   install AX=0x60    1E95:00DD                    _adlib_create
 *   cmd 0              1E95:0140 (uninstall)        _adlib_destroy
 *   cmd 2              1E95:05CA -> 1E95:05D8       _adlib_tick
 *   cmd 5              1E95:032C                    _adlib_start_event
 *   cmd 6              1E95:048F                    _adlib_release_event
 *   cmd 0x0A           1E95:01DD (master volume)    _adlib_set_tempo
 *   cmd 0x0C           1E95:02AA (load & start)     _adlib_load_tune
 *   cmd 0x0E           1E95:01C0 (identity block)   _adlib_command_block
 *
 * The "tempo" command is a master *volume*: the driver stores
 * (0xA3 * CL) >> 8 in CS:[0x1556] and every note's level is
 * min(volume, [0x1556]) before it reaches the OPL volume register.  The game
 * fades in (1000:84A5, CL 0 -> 0x4A) and out (1000:835A, CL 0x4B -> 0) with it.
 *
 * The chip is Nuked-OPL3 (thirdparty/, LGPL-2.1-or-later) in OPL2 mode: the
 * driver never sets the OPL3 "NEW" bit, and its register stream is the one
 * adlib_write_opl_reg (1E95:0E23) pushed to ports 0x388/0x389.  Every write
 * goes through _write_reg, which keeps the driver's own 256-byte shadow
 * (CS:0x1456) because the sequencer reads it back for read-modify-writes.
 *
 * Image layout:
 *   header   0x000..0x157, copied to the driver's CS:0x10FD by cmd 0x0C
 *   records  0x158 + id*16, K of them (header[0x11B]); cmd 5 takes one
 *   offsets  0x158 + K*16, N = ((P*2) & 0xFFF0) + 0x10 bytes, paragraphs
 *   songs    0x158 + K*16 + N, pattern b at +(T[b]-T[0])*16
 *   patches  +(T[P]-T[0])*16 + 0x0A + id*0x40, 64-byte blocks
 * A row is a little-endian mask (MSB first) followed by a 4-byte voice cell
 * per set bit; a cell is note, instrument, command, parameter.
 *
 * The tune image is copied: the original keeps the game's far pointer, but a
 * copy keeps the caller's buffer lifetime out of the driver's contract.
 * cmd 5's handle is the record index (the original returns its record
 * pointer; the game only round-trips it).
 */
#include "audio/sound_driver.h"

#include "byteorder.h"
#include "thirdparty/opl3.h"

#include <stdlib.h>
#include <string.h>

#define ADLIB_VOICES        11u   /* 6 FM channels + 5 rhythm voices */
#define ADLIB_MELODIC       6u   /* voices 0..6 use the two-operator path */
#define ADLIB_RECORDS       6u   /* CS:0x135E, stride 0x15 */
#define ADLIB_TRACKS        6u    /* CS:0x1406, the melodic slot free list */
#define ADLIB_HEADER        0x158u
#define ADLIB_ROWS          0x40u
#define ADLIB_OFFSETS       160u /* the copied offset table, in words */
#define ADLIB_TEMPO_DEFAULT 0x30u

/* OPL F-number window of the driver's note table (CS:0x0E8C). */
#define ADLIB_FNUM_WRAP  0x2AEu
#define ADLIB_FNUM_LOW   0x142u
#define ADLIB_FNUM_TOP   0x28Au
#define ADLIB_FNUM_FIRST 0x157u

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
    0x71E6, 0x7203, 0x7222, 0x7243, 0x7266, 0x728A,
    0x0814, 0x08AA, 0x08D4, 0x0972, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0874, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0A04, 0x0813, 0x0900, 0x0813, 0x0813, 0x0813, 0x0813, 0x090F, 0x091F,
    0x0907, 0x0813, 0x0932, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813, 0x0813,
    0x0813, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0101,
    0x0101, 0x0101, 0x0000, 0x0101, 0x0101, 0x0101, 0x0000, 0x0101, 0x0101,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x3F3F, 0x3F3F, 0x3F3F,
    0x0000, 0x3F3F, 0x3F3F, 0x3F3F, 0x0000, 0x3F3F, 0x3F3F, 0x3F3F, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0xF0F0,
    0xF0F0, 0xF0F0, 0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0xF0F0, 0xF0F0, 0xF0F0,
    0x0000, 0xF0F0, 0xF0F0, 0xF0F0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2000, 0x0000
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

/* Every byte of the driver's 22-byte voice record (CS:0x1268 + voice*0x16)
 * that the sequencer reads or writes; the offsets are the driver's. */
typedef struct AdlibSlot {
    uint8_t note;            /* +0x0  */
    uint8_t instrument;      /* +0x1  1-based; 0 = no voice change */
    uint8_t command;         /* +0x2  */
    uint8_t param;           /* +0x3  */
    uint8_t volume;          /* +0x4  */
    uint8_t last_instrument; /* +0x8  */
    uint16_t slide_target;   /* +0x9  note-table entry, 0 = not sliding */
    uint8_t slide_dir;       /* +0xB  1 = up */
    uint8_t slide_step;      /* +0xC  */
    uint16_t note_entry;     /* +0xD  the entry last written */
    uint8_t note_copy;       /* +0xF  the note the slide lands on */
    uint8_t octave;          /* +0x11 */
    uint16_t fnum;           /* +0x12 */
    uint8_t mod_atten;       /* +0x14 instrument block[0x12] */
    uint8_t car_atten;       /* +0x15 instrument block[0x23] */
} AdlibSlot;

/* The driver's song record (built by cmd 5 from a 0x158+id*16 image record). */
typedef struct AdlibRecord {
    uint8_t active;                   /* +0x0  0xFF while playing */
    uint8_t tracks;                   /* +0x1  the image record's +0xD */
    uint8_t ticks_per_row;            /* +0x2  default 6, voice cmd 0x0F */
    uint8_t order;                    /* +0x3  index into the order list */
    uint16_t cursor;                  /* +0x4  pattern-relative stream pos */
    uint8_t tick;                     /* +0x6  */
    uint8_t advance;                  /* +0x7  toggled by cmds 0x0B/0x0D */
    uint8_t row;                      /* +0x8  rows decoded, 0x40 = end */
    uint8_t loop;                     /* +0x9  */
    uint8_t first_order;              /* +0xA  */
    uint8_t last_order;               /* +0xB  last order + 1 */
    uint8_t finished;                 /* +0xC  */
    uint8_t track_slot[ADLIB_TRACKS]; /* +0xD  image record +0xD */
} AdlibRecord;

struct SoundDriver {
    uint32_t sampleRate;
    uint8_t ready;
    uint8_t tempo;                   /* [0x1556] */
    uint8_t slot_free[ADLIB_TRACKS]; /* [0x1406] */
    uint8_t voice_volume[16];        /* [0x1557+voice] */
    uint8_t shadow[256];             /* [0x1456] */
    uint8_t header[ADLIB_HEADER];    /* [0x10FD] */
    uint16_t offset[ADLIB_OFFSETS];  /* [0x1595] */
    uint16_t offset_count;
    AdlibSlot slot[ADLIB_VOICES];
    AdlibRecord record[ADLIB_RECORDS];
    uint8_t *tune;
    uint32_t tune_size;
    uint32_t song_data;              /* SDS offset into tune */
    uint32_t patch_base;             /* tune offset of the patch bank */
    opl3_chip opl;
};

/**
 * @brief Write one OPL register and keep the driver's shadow in step.
 *
 * @param drv the driver
 * @param reg the OPL register 0x00..0xFF
 * @param value the byte to write
 */
static void _write_reg(SoundDriver *drv, uint8_t reg, uint8_t value)
{
    drv->shadow[reg] = value;
    OPL3_WriteReg(&drv->opl, reg, value);
}

/**
 * @brief Read a little-endian word out of the tune image, or 0 past its end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the word
 */
static uint16_t _tune_u16(const SoundDriver *drv, uint32_t off)
{
    if (off + 2u > drv->tune_size) return 0;
    return rd_le_u16(drv->tune + off);
}

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
{
    return _note_raw((uint8_t)(note - 12u));
}

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

/**
 * @brief Set a channel's F-number and block (1E95:0CA0).
 *
 * Channel 8 has no usable frequency register pair in the driver's model, so
 * the routine returns for it.
 *
 * @param drv the driver
 * @param voice the channel / voice index
 * @param entry the F-number | (octave << 12) word
 */
static void _set_frequency(SoundDriver *drv, unsigned voice, uint16_t entry)
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
    _write_reg(drv, reg, (uint8_t)((drv->shadow[reg] & 0x20u) | block));
    _write_reg(drv, (uint8_t)(0xA0u + voice), lo);
}

/**
 * @brief Key a voice on (1E95:0C3A).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _key_on(SoundDriver *drv, unsigned voice)
{
    if (voice < ADLIB_MELODIC) {
        uint8_t reg = (uint8_t)(0xB0u + voice);
        _write_reg(drv, reg, (uint8_t)(drv->shadow[reg] | 0x20u));
    } else {
        uint8_t bit = _rhythm_bit[voice - ADLIB_MELODIC];
        _write_reg(drv, 0xBDu, (uint8_t)(drv->shadow[0xBDu] | bit));
    }
}

/**
 * @brief Key a voice off (1E95:0C64).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _key_off(SoundDriver *drv, unsigned voice)
{
    if (voice < ADLIB_MELODIC) {
        uint8_t reg = (uint8_t)(0xB0u + voice);
        _write_reg(drv, reg, (uint8_t)(drv->shadow[reg] & 0xDFu));
    } else {
        uint8_t mask = (uint8_t)~_rhythm_bit[voice - ADLIB_MELODIC];
        _write_reg(drv, 0xBDu, (uint8_t)(drv->shadow[0xBDu] & mask));
    }
}

/**
 * @brief Silence a voice: key off and put its carrier at full attenuation.
 *
 * This is the driver's release path (1E95:0CD4) with the image's zero scratch
 * record, so it also clears the voice's volume cache.
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _silence(SoundDriver *drv, unsigned voice)
{
    uint8_t op;

    if (voice >= ADLIB_VOICES) return;
    /* 1E95:0CD4: clear the cached instrument first, so the next note of this
     * voice reloads its patch even when the instrument number repeats. */
    drv->slot[voice].last_instrument = 0;
    _key_off(drv, voice);
    drv->voice_volume[voice] = 0;
    op = _op_carrier[voice];
    _write_reg(drv, (uint8_t)(0x40u + op),
               (uint8_t)((drv->shadow[0x40u + op] & 0xC0u) | 0x3Fu));
}

/**
 * @brief Apply the master volume to all eleven voices (1E95:0A01).
 *
 * The floor is 0x3F - tempo and only quieter voices are changed, so a
 * decreasing tempo (the fade-in) never lifts an already-written level - that
 * fade works through each note's own volume instead.
 *
 * @param drv the driver
 */
static void _apply_tempo(SoundDriver *drv)
{
    uint8_t floor = (uint8_t)(0x3Fu - drv->tempo);
    unsigned i;

    for (i = 0; i < ADLIB_VOICES; i++) {
        uint8_t reg = (uint8_t)(0x40u + _op_carrier[i]);
        uint8_t b = drv->shadow[reg];
        if ((b & 0x3Fu) >= floor) continue;
        _write_reg(drv, reg, (uint8_t)((b & 0xC0u) | floor));
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
static void _apply_operator(SoundDriver *drv, unsigned voice, uint8_t op,
                            const uint8_t *desc, uint8_t level,
                            uint8_t waveform, uint8_t feedback)
{
    uint8_t sustain;

    if (feedback) {
        uint8_t fb = (uint8_t)(((desc[2] & 0x07u) << 1) & 0xFEu);
        _write_reg(drv, (uint8_t)(0xC0u + voice), fb);
    }
    _write_reg(drv, (uint8_t)(0x40u + op),
               (uint8_t)(((desc[0] & 0x03u) << 6) | (level & 0x3Fu)));
    _write_reg(drv, (uint8_t)(0xE0u + op), (uint8_t)(waveform & 0x03u));
    _write_reg(drv, (uint8_t)(0x20u + op),
               (uint8_t)((desc[9] ? 0x80u : 0u) | (desc[10] ? 0x40u : 0u) |
                         (desc[5] ? 0x20u : 0u) | (desc[11] ? 0x10u : 0u) |
                         (desc[1] & 0x0Fu)));
    _write_reg(drv, (uint8_t)(0x60u + op),
               (uint8_t)(((uint8_t)(desc[3] << 4)) | (desc[6] & 0x0Fu)));
    sustain = (uint8_t)(15u - desc[4]);
    _write_reg(drv, (uint8_t)(0x80u + op),
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
static void _build_patch(const SoundDriver *drv, unsigned index, uint8_t *out)
{
    uint32_t off = drv->patch_base + index * 0x40u + 0x0Au;
    unsigned i;

    for (i = 0; i < 30u; i++) out[i] = _tune_u8(drv, off + i);
}

/**
 * @brief Apply an instrument to a voice (1E95:0A4D).
 *
 * Voices 0..6 are the two-operator path: the modulator block drives the
 * modulator operator (its level is the block's own loudness), the carrier
 * block drives the carrier with the voice's volume.  Voices 7..10 are the
 * rhythm path: one block, applied to the voice's single rhythm operator.
 *
 * @param drv the driver
 * @param voice the voice index
 * @param index the 0-based patch index
 */
static void _apply_instrument(SoundDriver *drv, unsigned voice, unsigned index)
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

/**
 * @brief Write a voice's volume (1E95:0A05).
 *
 * The value is clamped to 0x40, floored by the master volume, deduplicated
 * against the last value written for the voice, then folded into the
 * carrier's reg 0x40 with the instrument's KSL bits kept.
 *
 * @param drv the driver
 * @param voice the voice index
 * @param volume the requested volume 0..0x40
 */
static void _set_volume(SoundDriver *drv, unsigned voice, uint8_t volume)
{
    AdlibSlot *v = &drv->slot[voice];
    uint8_t atten;
    uint8_t reg;

    if (volume >= 0x40u) volume = 0x3Fu; /* 1E95:0A05: CMP 0x40 / MOV 0x3F */
    if (volume > drv->tempo) volume = drv->tempo;
    if (volume == drv->voice_volume[voice]) return;
    drv->voice_volume[voice] = volume;
    v->volume = volume;
    atten = (voice <= ADLIB_MELODIC) ? v->car_atten : v->mod_atten;
    reg = (uint8_t)(0x40u + _op_carrier[voice]);
    _write_reg(drv, reg,
               (uint8_t)((drv->shadow[reg] & 0xC0u) | _level(atten, volume)));
}

/**
 * @brief Land a voice on its slide target's note (1E95:0CE0).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _slide_finish(SoundDriver *drv, unsigned voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t entry = _note_entry(v->note_copy);

    v->note_entry = entry;
    v->fnum = (uint16_t)(entry & 0x03FFu);
    v->octave = (uint8_t)(entry >> 12);
    _set_frequency(drv, voice, entry);
}

/**
 * @brief Note the slide target of a voice command 3 (1E95:0947).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _slide_setup(SoundDriver *drv, unsigned voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t target = _note_entry(v->note);

    v->note_copy = v->note;
    v->slide_target = target;
    v->slide_dir = 0;
    if (target > v->note_entry) v->slide_dir = 1;
    else v->slide_target = 0;
}

/**
 * @brief Start a note on a voice (1E95:0BD0).
 *
 * @param drv the driver
 * @param voice the voice index
 * @param note the note number
 */
static void _note_on(SoundDriver *drv, unsigned voice, uint8_t note)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t entry;

    _key_off(drv, voice);
    if (voice < 7u) {
        entry = _note_entry(note);
        v->note_entry = entry;
        v->fnum = (uint16_t)(entry & 0x03FFu);
        v->octave = (uint8_t)(entry >> 12);
        _set_frequency(drv, voice, entry);
    } else if (voice == 8u) {
        /* 1E95:0C18: the tom's note also retunes channel 7 by seven steps. */
        _set_frequency(drv, 8u, _note_raw(note));
        _set_frequency(drv, 7u, _note_raw((uint8_t)(note + 7u)));
    } else {
        _set_frequency(drv, voice, _note_raw(note));
    }
    _key_on(drv, voice);
}

/**
 * @brief Step a voice's slide by its consumed parameter (1E95:0972).
 *
 * @param drv the driver
 * @param voice the voice index
 */
static void _slide_tick(SoundDriver *drv, unsigned voice)
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
        _set_frequency(drv, voice, entry);
        return;
    }
    v->slide_target = 0;
    _slide_finish(drv, voice);
}

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
static void _tick_command(SoundDriver *drv, unsigned voice)
{
    AdlibSlot *v = &drv->slot[voice];
    uint16_t step;
    uint16_t fnum;
    uint16_t entry;

    switch (v->command & 0x1Fu) {
    case 0x01: /* 1E95:08AA: slide up by 2 * parameter, octave wrap at 0x2AE */
        step = (uint16_t)(2u * v->param);
        fnum = (uint16_t)(v->fnum + step);
        while (fnum >= ADLIB_FNUM_WRAP) {
            fnum = (uint16_t)((fnum - ADLIB_FNUM_WRAP) + ADLIB_FNUM_FIRST);
            v->octave++; /* 8-bit, as the driver's INC DL */
        }
        v->fnum = fnum;
        entry = (uint16_t)(fnum | (v->octave << 12));
        v->note_entry = entry;
        _set_frequency(drv, voice, entry);
        break;
    case 0x02: /* 1E95:08D4: slide down by 2 * parameter, wrap at 0x142 */
        step = (uint16_t)(2u * v->param);
        fnum = (uint16_t)(v->fnum - step);
        while (fnum <= ADLIB_FNUM_LOW) {
            /* 1E95:08E9 SUB 0x142 / NEG / ADD 0x28A: 0x28A + (0x142 - fnum) */
            fnum = (uint16_t)(ADLIB_FNUM_TOP + (ADLIB_FNUM_LOW - fnum));
            v->octave--; /* 8-bit, as the driver's DEC DL */
        }
        v->fnum = fnum;
        entry = (uint16_t)(fnum | (v->octave << 12));
        v->note_entry = entry;
        _set_frequency(drv, voice, entry);
        break;
    case 0x03: /* 1E95:0972: portamento toward the cached note entry */
        _slide_tick(drv, voice);
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
        _set_volume(drv, voice, (uint8_t)next);
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
static void _note_command(SoundDriver *drv, AdlibRecord *rec, unsigned voice)
{
    AdlibSlot *v = &drv->slot[voice];

    switch (v->command & 0x1Fu) {
    case 6: /* 1E95:0900: note off */
        v->command = 0;
        _key_off(drv, voice);
        break;
    case 0x0B: /* 1E95:090F: jump to order parameter-1, advance next tick */
        rec->order = (uint8_t)(v->param - 1u);
        rec->advance = (uint8_t)~rec->advance;
        break;
    case 0x0C: /* 1E95:091F: volume = min(parameter, 0x40) */
        if (v->param > ADLIB_ROWS) v->param = ADLIB_ROWS;
        _set_volume(drv, voice, v->param);
        break;
    case 0x0D: /* 1E95:0907: advance to the next order on the next tick */
        rec->advance = (uint8_t)~rec->advance;
        break;
    case 0x0F: /* 1E95:0932: ticks per row */
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
static void _play_cell(SoundDriver *drv, AdlibRecord *rec, unsigned voice)
{
    AdlibSlot *v = &drv->slot[voice];

    if (v->instrument != 0 && v->note != 0) {
        unsigned index = (unsigned)v->instrument - 1u;
        uint8_t volume;
        /* The header holds 64 two-byte entries; the driver trusts the cell. */
        if (index >= 64u) index = 63u;
        volume = drv->header[0x99u + 2u * index];
        if (v->command == 0x0Cu) {
            volume = v->param;
            v->command = 0;
            v->param = 0;
        }
        _set_volume(drv, voice, volume);
        if (v->instrument != v->last_instrument) {
            v->last_instrument = v->instrument;
            _apply_instrument(drv, voice, (unsigned)v->instrument - 1u);
        }
    }

    if (v->note == 0) {
        _note_command(drv, rec, voice);
    } else if ((v->command & 0x1Fu) == 3u) {
        _slide_setup(drv, voice); /* command 3's note-path entry is a RET */
    } else {
        _note_command(drv, rec, voice);
        _note_on(drv, voice, v->note);
    }
}

/**
 * @brief The record's live slot list, or NULL when a slot index is invalid.
 *
 * @param drv the driver
 * @param rec the record
 * @param i the track index
 * @return the voice index
 */
static unsigned _record_voice(const AdlibRecord *rec, unsigned i)
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
    unsigned i;

    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++) {
        unsigned voice = _record_voice(rec, i);
        AdlibSlot *v = &drv->slot[voice];
        if ((v->command | v->param) != 0) _tick_command(drv, voice);
    }
    for (i = 0; i < 5u; i++) {
        unsigned voice = ADLIB_MELODIC + i;
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
static uint8_t _read_cell(SoundDriver *drv, unsigned voice, uint32_t base,
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
    unsigned pattern;
    uint32_t base;
    uint16_t mask;
    uint16_t row_mask;
    uint16_t cells; /* the post-mask cursor */
    unsigned i;

    if (rec->order >= 0x7Fu) {
        rec->row = ADLIB_ROWS; /* the order list is 0x7F bytes */
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
    cells = rec->cursor; /* 1E95:071C: SI after LODSW */
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
    unsigned i;

    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++)
        _play_cell(drv, rec, _record_voice(rec, i));
    for (i = 0; i < 5u; i++) _play_cell(drv, rec, ADLIB_MELODIC + i);
}

/**
 * @brief Free a record's voices and mark it inactive (1E95:0467).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _release_record(SoundDriver *drv, AdlibRecord *rec)
{
    unsigned i;

    if (!rec->active) return;
    rec->active = 0;
    for (i = 0; i < rec->tracks && i < ADLIB_TRACKS; i++) {
        unsigned slot = _record_voice(rec, i);
        drv->slot_free[slot] = (uint8_t)slot;
        _silence(drv, slot);
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
    _release_record(drv, rec);
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

/**
 * @brief Step one record by one driver tick (1E95:0602).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _step_record(SoundDriver *drv, AdlibRecord *rec)
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

/**
 * @brief Install the AdLib driver emulation (INT 60h install, 1E95:00DD).
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when out of memory
 */
static SoundDriver *_adlib_create(uint32_t sampleRate)
{
    SoundDriver *drv = calloc(1, sizeof(*drv));
    unsigned i;

    if (!drv) return NULL;
    if (sampleRate == 0) sampleRate = 48000;
    drv->sampleRate = sampleRate;
    OPL3_Reset(&drv->opl, sampleRate);
    /* 1E95:0DDF: copy the probe's 256-byte state into the shadow, writing
     * every register on the way. */
    for (i = 0; i < 256u; i++) _write_reg(drv, (uint8_t)i, _opl_init[i]);
    for (i = 0; i < ADLIB_TRACKS; i++) drv->slot_free[i] = (uint8_t)i;
    drv->ready = 1;
    drv->tempo = ADLIB_TEMPO_DEFAULT;
    return drv;
}

/**
 * @brief cmd 0: uninstall (1E95:0140).
 *
 * @param drv the driver; NULL is accepted
 */
static void _adlib_destroy(SoundDriver *drv)
{
    if (!drv) return;
    free(drv->tune);
    free(drv);
}

/**
 * @brief cmd 0x0A: the master volume (1E95:01DD).
 *
 * @param drv the driver
 * @param tempo CL, the game's byte; clamped to 0x64
 */
static void _adlib_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    uint8_t value;

    if (!drv) return;
    if (tempo > 0x64u) tempo = 0x64u;
    value = (uint8_t)((0xA3u * (unsigned)tempo) >> 8);
    if (value == drv->tempo) return;
    drv->tempo = value;
    _apply_tempo(drv);
}

/**
 * @brief cmd 0x0E: identify, the 8-byte command block (1E95:01C0).
 *
 * @param drv the driver
 * @return the block (ES:DI in the original), all zero as observed
 */
static const uint8_t *_adlib_command_block(SoundDriver *drv)
{
    static const uint8_t block[8];
    (void)drv;
    return block;
}

/**
 * @brief Reset all eleven voices after a tune load (1E95:0573).
 *
 * The original calls this at the end of cmd 0x0C (1E95:031E): for each voice
 * it clears the slot's last-instrument byte (+0x8) and silences it (key off +
 * level 0x3F, 1E95:0CD4).  Without the +0x8 clear the next note of a voice
 * whose instrument number matches the *previous* tune's would skip the patch
 * apply and keep a stale instrument from the old bank.
 *
 * @param drv the driver
 */
static void _reset_voices(SoundDriver *drv)
{
    unsigned i;

    for (i = 0; i < ADLIB_VOICES; i++) {
        drv->slot[i].last_instrument = 0;
        _silence(drv, i);
    }
}

/**
 * @brief cmd 0x0C: load and start a tune image (1E95:02AA).
 *
 * @param drv the driver
 * @param image the .ALB image
 * @param size its byte length
 */
static void _adlib_load_tune(SoundDriver *drv, const uint8_t *image,
                             uint32_t size)
{
    unsigned max_order = 0;
    unsigned patterns;
    unsigned records;
    uint32_t stream;
    uint32_t table;
    uint32_t sds;
    uint32_t table_end;
    unsigned i;

    if (!drv || !drv->ready || !image || size < ADLIB_HEADER) return;

    /* Same derivation as 1E95:02AA, from the caller's image before it is
     * copied (P = the number of patterns, K = the record slots). */
    for (i = 0; i < 0x7Fu; i++)
        if (image[0x18u + i] > max_order) max_order = image[0x18u + i];
    patterns = max_order + 1u;
    records = image[0x11Bu];
    stream = 0x158u + records * 16u;
    table = ((patterns * 2u) & 0xFFF0u) + 0x10u;
    table_end = stream + table;
    if (table_end > size) return;
    if (table / 2u > ADLIB_OFFSETS) return;

    free(drv->tune);
    drv->tune = malloc(size);
    if (!drv->tune) {
        drv->tune_size = 0;
        return;
    }
    memcpy(drv->tune, image, size);
    drv->tune_size = size;
    memcpy(drv->header, image, ADLIB_HEADER);

    for (i = 0; i < table / 2u; i++)
        drv->offset[i] = rd_le_u16(image + stream + 2u * i);
    drv->offset_count = (uint16_t)(table / 2u);
    sds = table_end;
    drv->song_data = sds;
    drv->patch_base =
        sds + ((uint32_t)drv->offset[patterns] - drv->offset[0]) * 16u;
    _reset_voices(drv); /* 1E95:031E */
}

/**
 * @brief cmd 5: start a record (event id) as a voice set (1E95:032C).
 *
 * The handle is the record index; the original returns its record pointer,
 * which the game only stores and hands back to cmd 6.
 *
 * @param drv the driver
 * @param event CX, the record index in the loaded image
 * @return the handle, or 0xFFFF when refused
 */
static uint16_t _adlib_start_event(SoundDriver *drv, uint16_t event)
{
    unsigned index;
    AdlibRecord *rec = NULL;
    uint32_t off;
    unsigned tracks;
    unsigned i;

    if (!drv || !drv->ready || !drv->tune) return 0xFFFF;
    if (event >= drv->header[0x11Bu]) return 0xFFFF;
    for (index = 0; index < ADLIB_RECORDS; index++) {
        if (!drv->record[index].active) {
            rec = &drv->record[index];
            break;
        }
    }
    if (!rec) return 0xFFFF;

    off = 0x158u + (uint32_t)event * 16u;
    memset(rec, 0, sizeof(*rec));
    tracks = _tune_u8(drv, off + 0xDu);
    if (tracks > ADLIB_TRACKS) tracks = ADLIB_TRACKS;
    rec->tracks = (uint8_t)tracks;
    rec->ticks_per_row = 6;
    rec->order = _tune_u8(drv, off + 0xAu);
    rec->first_order = rec->order;
    rec->loop = _tune_u8(drv, off + 0xCu);
    rec->last_order = (uint8_t)(_tune_u8(drv, off + 0xBu) + 1u);
    memset(drv->voice_volume, 0, sizeof(drv->voice_volume));

    for (i = 0; i < tracks; i++) {
        unsigned slot;
        for (slot = 0; slot < ADLIB_TRACKS; slot++)
            if (drv->slot_free[slot] != 0xFFu) break;
        if (slot == ADLIB_TRACKS) {
            /* The original's no-free-slot path (1E95:044F) is unreachable in
             * this title; give back what this call already took. */
            unsigned j;
            for (j = 0; j < i; j++) {
                unsigned taken = rec->track_slot[j];
                drv->slot_free[taken] = (uint8_t)taken;
                _silence(drv, taken);
            }
            memset(rec, 0, sizeof(*rec));
            return 0xFFFF;
        }
        drv->slot_free[slot] = 0xFFu;
        rec->track_slot[i] = (uint8_t)slot;
        /* 1E95:0437 passes voice+1 to the silence helper; kept as found. */
        _silence(drv, slot + 1u);
    }
    rec->active = 0xFFu;
    return (uint16_t)index;
}

/**
 * @brief cmd 6: release a record (1E95:048F).
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _adlib_release_event(SoundDriver *drv, uint16_t voice)
{
    if (!drv || voice >= ADLIB_RECORDS) return;
    _release_record(drv, &drv->record[voice]);
}

/**
 * @brief cmd 2: step the sequencer one tick (1E95:05CA).
 *
 * @param drv the driver
 */
static void _adlib_tick(SoundDriver *drv)
{
    unsigned i;

    if (!drv || !drv->ready) return;
    for (i = 0; i < ADLIB_RECORDS; i++) _step_record(drv, &drv->record[i]);
}

/**
 * @brief Render interleaved stereo samples at the requested rate.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _adlib_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    uint32_t i;

    if (!drv) {
        memset(dst, 0, (size_t)frames * 2u * sizeof(*dst));
        return;
    }
    for (i = 0; i < frames; i++) {
        int16_t sample[2];
        /* The chip runs at its native 49716 Hz; OPL3_Generate would clock it
         * once per output sample and play everything 48000/49716 flat. */
        OPL3_GenerateResampled(&drv->opl, sample);
        dst[2u * i] = sample[0];
        dst[2u * i + 1u] = sample[1];
    }
}

const SoundDriverModule g_sound_driver_adlib = {
    _adlib_create,        _adlib_destroy,   _adlib_set_tempo,
    _adlib_command_block, _adlib_load_tune, _adlib_start_event,
    _adlib_release_event, _adlib_tick,      _adlib_generate
};
