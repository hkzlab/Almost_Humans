/* The AdLib module's shared declarations: the driver's slot and record
 * layouts, the state the themes share, and the helpers that cross one.  The
 * module contract is sound_driver_adlib.h.
 */
#ifndef HUMANSRE_ADLIB_INTERNAL_H
#define HUMANSRE_ADLIB_INTERNAL_H

#include "audio/sound_driver.h"
#include "byteorder.h"
#include "thirdparty/opl3.h"

#include <stdint.h>

#define ADLIB_VOICES        11u   // 6 FM channels + 5 rhythm voices
#define ADLIB_MELODIC       6u   // voices 0..6 use the two-operator path
#define ADLIB_RECORDS       6u   // CS:0x135E, stride 0x15
#define ADLIB_TRACKS        6u    // CS:0x1406, the melodic slot free list
#define ADLIB_HEADER        0x158u
#define ADLIB_ROWS          0x40u
#define ADLIB_OFFSETS       160u // the copied offset table, in words
#define ADLIB_TEMPO_DEFAULT 0x30u

/* OPL F-number window of the driver's note table (CS:0x0E8C). */
#define ADLIB_FNUM_WRAP  0x2AEu
#define ADLIB_FNUM_LOW   0x142u
#define ADLIB_FNUM_TOP   0x28Au
#define ADLIB_FNUM_FIRST 0x157u

/* The driver's voice-record fields (CS:0x1268 + voice*0x16, 22 bytes); the
 * offsets are the driver's. */
typedef struct AdlibSlot {
    uint8_t note;            // +0x0
    uint8_t instrument;      // +0x1  1-based; 0 = no voice change
    uint8_t command;         // +0x2
    uint8_t param;           // +0x3
    uint8_t volume;          // +0x4
    uint8_t last_instrument; // +0x8
    uint16_t slide_target;   // +0x9  note-table entry, 0 = not sliding
    uint8_t slide_dir;       // +0xB  1 = up
    uint8_t slide_step;      // +0xC
    uint16_t note_entry;     // +0xD  the entry last written
    uint8_t note_copy;       // +0xF  the note the slide lands on
    uint8_t octave;          // +0x11
    uint16_t fnum;           // +0x12
    uint8_t mod_atten;       // +0x14 instrument block[0x12]
    uint8_t car_atten;       // +0x15 instrument block[0x23]
} AdlibSlot;

/* The driver's song record (built by cmd 5 from a 0x158+id*16 image record). */
typedef struct AdlibRecord {
    uint8_t active;                   // +0x0  0xFF while playing
    uint8_t tracks;                   // +0x1  the image record's +0xD
    uint8_t ticks_per_row;            // +0x2  default 6, voice cmd 0x0F
    uint8_t order;                    // +0x3  index into the order list
    uint16_t cursor;                  // +0x4  pattern-relative stream pos
    uint8_t tick;                     // +0x6
    uint8_t advance;                  // +0x7  toggled by cmds 0x0B/0x0D
    uint8_t row;                      // +0x8  rows decoded, 0x40 = end
    uint8_t loop;                     // +0x9
    uint8_t first_order;              // +0xA
    uint8_t last_order;               // +0xB  last order + 1
    uint8_t finished;                 // +0xC
    uint8_t track_slot[ADLIB_TRACKS]; // +0xD  image record +0xD
} AdlibRecord;

struct SoundDriver {
    uint32_t sampleRate;
    uint8_t ready;
    uint8_t tempo;                   // [0x1556]
    uint8_t slot_free[ADLIB_TRACKS]; // [0x1406]
    uint8_t voice_volume[16];        // [0x1557+voice]
    uint8_t shadow[256];             // [0x1456]
    uint8_t header[ADLIB_HEADER];    // [0x10FD]
    uint16_t offset[ADLIB_OFFSETS];  // [0x1595]
    uint16_t offset_count;
    AdlibSlot slot[ADLIB_VOICES];
    AdlibRecord record[ADLIB_RECORDS];
    uint8_t *tune;
    uint32_t tune_size;
    uint32_t song_data;              // SDS offset into tune
    uint32_t patch_base;             // tune offset of the patch bank
    opl3_chip opl;
};

/**
 * @brief Read a little-endian word out of the tune image, or 0 past its end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the word
 */
static inline uint16_t _tune_u16(const SoundDriver *drv, uint32_t off)
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
static inline uint8_t _tune_u8(const SoundDriver *drv, uint32_t off)
{
    if (off >= drv->tune_size) return 0;
    return drv->tune[off];
}

/**
 * @brief Write one OPL register and keep the driver's shadow in step.
 *
 * @param drv the driver
 * @param reg the OPL register 0x00..0xFF
 * @param value the byte to write
 */
void _adlib_write_reg(SoundDriver *drv, uint8_t reg, uint8_t value);

/**
 * @brief Install the probe's 256-byte OPL state (1E95:0DDF).
 *
 * @param drv the driver
 */
void _adlib_opl_probe_reset(SoundDriver *drv);

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
void _adlib_set_frequency(SoundDriver *drv, uint32_t voice, uint16_t entry);

/**
 * @brief Key a voice off (1E95:0C64).
 *
 * @param drv the driver
 * @param voice the voice index
 */
void _adlib_key_off(SoundDriver *drv, uint32_t voice);

/**
 * @brief Silence a voice: key off and put its carrier at full attenuation.
 *
 * This is the driver's release path (1E95:0CD4) with the image's zero scratch
 * record, so it also clears the voice's volume cache.
 *
 * @param drv the driver
 * @param voice the voice index
 */
void _adlib_silence(SoundDriver *drv, uint32_t voice);

/**
 * @brief Apply the master volume to all eleven voices (1E95:0A01).
 *
 * The floor is 0x3F - tempo and only quieter voices are changed, so a
 * decreasing tempo (the fade-in) never lifts an already-written level - that
 * fade works through each note's own volume instead.
 *
 * @param drv the driver
 */
void _adlib_apply_tempo(SoundDriver *drv);

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
void _adlib_apply_instrument(SoundDriver *drv, uint32_t voice, uint32_t index);

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
void _adlib_set_volume(SoundDriver *drv, uint32_t voice, uint8_t volume);

/**
 * @brief Note the slide target of a voice command 3 (1E95:0947).
 *
 * @param drv the driver
 * @param voice the voice index
 */
void _adlib_slide_setup(SoundDriver *drv, uint32_t voice);

/**
 * @brief Start a note on a voice (1E95:0BD0).
 *
 * @param drv the driver
 * @param voice the voice index
 * @param note the note number
 */
void _adlib_note_on(SoundDriver *drv, uint32_t voice, uint8_t note);

/**
 * @brief Step a voice's slide by its consumed parameter (1E95:0972).
 *
 * @param drv the driver
 * @param voice the voice index
 */
void _adlib_slide_tick(SoundDriver *drv, uint32_t voice);

/**
 * @brief Free a record's voices and mark it inactive (1E95:0467).
 *
 * @param drv the driver
 * @param rec the record
 */
void _adlib_release_record(SoundDriver *drv, AdlibRecord *rec);

/**
 * @brief Step one record by one driver tick (1E95:0602).
 *
 * @param drv the driver
 * @param rec the record
 */
void _adlib_step_record(SoundDriver *drv, AdlibRecord *rec);

#endif /* HUMANSRE_ADLIB_INTERNAL_H */
