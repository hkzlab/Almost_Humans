/* The MT-32 module's shared declarations: the driver's slot, record and play
 * states, the state the themes share, and the helpers that cross one.  The
 * module contract is sound_driver_mt32.h.
 */
#ifndef HUMANSRE_MT32_INTERNAL_H
#define HUMANSRE_MT32_INTERNAL_H

#include "audio/sound_driver.h"
#include "byteorder.h"

#include <mt32emu.h>

#include <stddef.h>
#include <stdint.h>

#define MT32_RECORDS  8u
#define MT32_SLOTS    8u
#define MT32_ROWS     0x40u
#define MT32_HEADER   0x158u
#define MT32_OFF_NEW  0x958u
#define MT32_OFF_OLD  0x918u
#define MT32_ROMS_MAX 64u
/* mt32emu's output gain is 1.0 by default; 0.501 is the reference -6 dB
 * mixer master gain, so the port's absolute level matches the intended mix. */
#define MT32_OUTPUT_GAIN 0.501f

/* The driver's MIDI-slot fields (CS:0x1401 + slot*0x20, 32 bytes); the
 * offsets are the driver's. */
typedef struct Mt32Slot {
    uint8_t note[4];      // +0x00 four chord notes, 0 = none
    uint8_t instrument;   // +0x04 1-based, 0 = no change
    uint8_t command;      // +0x05 table index (cmd & 0x1F)
    uint8_t param;        // +0x06
    uint8_t volume;       // +0x07  0..0x40
    uint16_t bend;        // +0x08  centre 0x2000
    uint8_t last_note;    // +0x0A
    uint8_t last_program; // +0x0B  0xFF = nothing sent yet
    uint16_t slide_target;// +0x0C  period, 0 = no slide
    uint8_t slide_dir;    // +0x0E  0 down, 1 up
    uint8_t slide_step;   // +0x0F
    uint16_t slide_acc;   // +0x10
    uint8_t slide_note;   // +0x12  the note the slide lands on
} Mt32Slot;

/* The driver's record fields (CS:0x1501 + id*0x16, 22 bytes); the offsets
 * are the driver's. */
typedef struct Mt32Record {
    uint8_t active;              // +0x00 0xFF while playing
    uint8_t tracks;              // +0x01
    uint8_t ticks_per_row;       // +0x02 default 6
    uint8_t order;               // +0x03 index into the order list
    uint16_t cursor;             // +0x04 pattern-relative byte offset
    uint8_t tick;                // +0x06
    uint8_t advance;             // +0x07 NOT toggled by cmds 0x0B/0x0D
    uint8_t row;                 // +0x08 rows decoded, 0x40 = end
    uint8_t loop;                // +0x09 file +0x0C
    uint8_t first_order;         // +0x0A
    uint8_t last_order;          // +0x0B file +0x0B + 1
    uint8_t finished;            // +0x0C
    uint8_t track_slot[MT32_SLOTS]; // +0x0D free-list slot numbers
} Mt32Record;

struct SoundDriver {
    uint32_t sampleRate;
    mt32emu_context ctx;
    uint8_t ready;
    uint8_t volume;                   // CS:0x108E, initial 0x4B
    uint8_t track_volume[16];         // CS:0x1070, indexed slot+1
    uint8_t command_block[8];         // CS:0x15D1, cmd 0x0E's buffer
    /* CS:0x0EAA..0x0EAC: the DT1 data command 7/8 writes and sends.  It
     * persists between messages, so command 8 keeps the mode command 7
     * left in byte 0 (the image starts 00 04 04). */
    uint8_t system_block[3];
    Mt32Slot slot[MT32_SLOTS];
    Mt32Record record[MT32_RECORDS];
    uint8_t slot_free[MT32_SLOTS];    // CS:0x15C1
    uint8_t *tune;
    uint32_t tune_size;
    uint32_t off_base;                // CS:0x13FD: 0x958 / 0x918
    uint32_t record_base;             // off_base - 0x800
    uint32_t bank_base;               // off_base + T[P]*16
    uint8_t order_list[0x7F];         // file +0x18
    uint8_t instruments[0x80];        // file +0x98
    uint8_t trailing;                 // file +0x118
    uint16_t offset[256];             // CS:0x1098
    uint16_t offset_count;
    /* The mt32emu ROM buffers: mt32emu_add_rom_data does not copy, so they
     * must outlive the context.  `rom_control`/`rom_pcm` are the ids
     * mt32emu_identify_rom_data assigned to them. */
    uint8_t *rom[MT32_ROMS_MAX];
    size_t rom_size[MT32_ROMS_MAX];
    const char *rom_control[MT32_ROMS_MAX];
    const char *rom_pcm[MT32_ROMS_MAX];
    uint32_t rom_count;
};

/**
 * @brief The play state a note command acts on (the original's CS:0x13FF).
 *
 * @param drv the driver
 * @param rec the record being played
 */
typedef struct Mt32Play {
    SoundDriver *drv;
    Mt32Record *rec;
} Mt32Play;

/**
 * @brief One MIDI data byte from the tune image, or 0 past its end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the byte
 */
static inline uint8_t _u8(const SoundDriver *drv, uint32_t off)
{
    if (off >= drv->tune_size) return 0;
    return drv->tune[off];
}

/**
 * @brief One little-endian word from the tune image, or 0 past its end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the word
 */
static inline uint16_t _u16(const SoundDriver *drv, uint32_t off)
{
    if (off + 2u > drv->tune_size) return 0;
    return rd_le_u16(drv->tune + off);
}

/**
 * @brief The channel status a slot uses: the original's BP = slot + 1.
 *
 * @param slot the slot
 * @return the MIDI channel number (1..8)
 */
static inline uint32_t _channel(uint32_t slot) { return slot + 1u; }

/* The emulator's report handler table (mt32_rom.c); create() installs it. */
extern const mt32emu_report_handler_i_v0 _mt32_report_handler;

/**
 * @brief Emit a complete short MIDI message (mt32_send_byte's payload).
 *
 * @param drv the driver
 * @param status the status byte (0x90/0xB0/0xE0 + channel)
 * @param d1 the first data byte
 * @param d2 the second data byte
 */
void _mt32_midi(SoundDriver *drv, uint8_t status, uint8_t d1, uint8_t d2);

/**
 * @brief Emit one program change - the driver's only 2-byte message.
 *
 * @param drv the driver
 * @param channel the part (1..8)
 * @param program the 0-based program
 */
void _mt32_program(SoundDriver *drv, uint8_t channel, uint8_t program);

/**
 * @brief Send one Roland DT1 sysex the way mt32_send_block (0x0D54) does.
 *
 * @param drv the driver
 * @param a0 the address high byte
 * @param a1 the address middle byte
 * @param a2 the address low byte
 * @param data the data bytes
 * @param n the data byte count
 */
void _mt32_sysex(SoundDriver *drv, uint8_t a0, uint8_t a1, uint8_t a2,
                 const uint8_t *data, uint32_t n);

/**
 * @brief Send the master volume (mt32_send_master_volume, 0x058A).
 *
 * @param drv the driver
 */
void _mt32_send_master_volume(SoundDriver *drv);

/**
 * @brief CC 0x7B / 0x79 / 7 / 0x0A on one part (0x05F1).
 *
 * @param drv the driver
 * @param channel the part (1..8)
 */
void _mt32_all_notes_off(SoundDriver *drv, uint32_t channel);

/**
 * @brief Send the 14-bit pitch bend (0x0A94).
 *
 * @param drv the driver
 * @param channel the part (1..8)
 * @param slot the slot holding the bend
 */
void _mt32_send_bend(SoundDriver *drv, uint32_t channel, const Mt32Slot *slot);

/**
 * @brief Note on with the driver's preceding all-notes-off (0x0B7C).
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param note the note (0 = skip)
 */
void _mt32_note_on(SoundDriver *drv, uint32_t slot_no, uint8_t note);

/**
 * @brief One additional chord note (0x0BB7).
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param note the note (0 = skip)
 */
void _mt32_note_on_extra(SoundDriver *drv, uint32_t slot_no, uint8_t note);

/**
 * @brief Note command 3 (0x0BCD): set up a pitch-bend slide.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
void _mt32_slide_setup(SoundDriver *drv, uint32_t slot_no);

/**
 * @brief Tick command 3 (0x0C05): step the slide by one tick's worth.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
void _mt32_slide_tick(SoundDriver *drv, uint32_t slot_no);

/**
 * @brief Release a record and its slots (0x03D9).
 *
 * @param drv the driver
 * @param rec the record
 */
void _mt32_release_record(SoundDriver *drv, Mt32Record *rec);

/**
 * @brief Step one record by one driver tick (0x07C9).
 *
 * @param drv the driver
 * @param rec the record
 */
void _mt32_step_record(SoundDriver *drv, Mt32Record *rec);

/**
 * @brief The timbre dump + part-1 program cycle after a load (0x065A).
 *
 * @param drv the driver
 */
void _mt32_init_timbres(SoundDriver *drv);

/**
 * @brief The MT-32 reset sequence (0x05A4): volume, system block, CCs.
 *
 * @param drv the driver
 */
void _mt32_reset_voices(SoundDriver *drv);

/**
 * @brief The install reset (0x0CC7), minus the MPU-401 mode byte.
 *
 * `mt32_mpu_reset` sends 0x3F to the MPU (meaningless to the emulator), then
 * the MT-32 all-parameters reset, a CC/ bend reset on channels 1..9 and the
 * system block that clears the reverb and sets the partial reserve.  Without
 * it the emulator keeps its power-on reverb (5/3) instead of the driver's
 * post-install state (off).
 *
 * @param drv the driver
 */
void _mt32_install_reset(SoundDriver *drv);

/**
 * @brief Read one whole file into memory through the filesystem module.
 *
 * @param path the file
 * @param size receives the byte count
 * @return the buffer, or NULL
 */
uint8_t *_mt32_read_file(const char *path, size_t *size);

/**
 * @brief The directory sound_driver_mt32_set_roms() selected.
 *
 * @return the path, empty when none was given
 */
const char *_mt32_rom_dir(void);

/**
 * @brief Read every regular file of a directory into the driver's ROM list.
 *
 * The listing comes from the filesystem module, so the driver has no host
 * path or directory API of its own.
 *
 * @param drv the driver
 * @param dir the directory
 */
void _mt32_load_rom_dir(SoundDriver *drv, const char *dir);

/**
 * @brief Hand the preferred ROM pair to mt32emu, pairing split images as a
 * fallback.
 *
 * @param drv the driver
 * @return 1 when a control and a PCM ROM were accepted
 */
int32_t _mt32_add_roms(SoundDriver *drv);

#endif /* HUMANSRE_MT32_INTERNAL_H */
