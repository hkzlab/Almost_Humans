/* The Beeper module's shared declarations: the driver's record layout, the
 * state the themes share, and the helpers that cross one.  The module
 * contract is sound_driver_beeper.h.
 */
#ifndef HUMANSRE_BEEPER_INTERNAL_H
#define HUMANSRE_BEEPER_INTERNAL_H

#include "audio/sound_driver.h"
#include "thirdparty/pcspeaker_impulse.h"

#include <stdint.h>

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
static inline uint8_t _tune_u8(const SoundDriver *drv, uint32_t off)
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
void _beeper_set_divider(SoundDriver *drv, uint16_t divider);

/**
 * @brief Enable the speaker (1E95:07ED): port 0x61 bits 0-1.
 *
 * The original is idempotent; the PIT mode-3 write it also does is part of
 * the divider path here.
 *
 * @param drv the driver
 */
void _beeper_speaker_on(SoundDriver *drv);

/**
 * @brief Disable the speaker (1E95:0809): clear port 0x61 bits 0-1.
 *
 * @param drv the driver
 */
void _beeper_speaker_off(SoundDriver *drv);

/**
 * @brief Arm a slot for the next multiplex window (1E95:07CC).
 *
 * The driver's extra gate at CS:0x0E75+slot is 0xFF and never written.
 *
 * @param drv the driver
 * @param slot the slot index
 * @param note the note byte, indexing the period table directly
 */
void _beeper_set_note_frequency(SoundDriver *drv, uint32_t slot, uint8_t note);

/**
 * @brief Hand a slot to (or take it from) the speaker (1E95:058A).
 *
 * An armed slot (0xFF) turns the speaker on and programs its divider; any
 * other value turns the speaker off.
 *
 * @param drv the driver
 * @param slot the slot index
 */
void _beeper_switch_slot(SoundDriver *drv, uint32_t slot);

/**
 * @brief Free a record's slots and mark it inactive (1E95:03D1).
 *
 * The original turns the speaker off once per track; it does not clear the
 * slots' arm flags.
 *
 * @param drv the driver
 * @param rec the record
 */
void _beeper_release_record(SoundDriver *drv, BeeperRecord *rec);

/**
 * @brief Step one record by one driver tick (1E95:05AA).
 *
 * @param drv the driver
 * @param rec the record
 */
void _beeper_step_record(SoundDriver *drv, BeeperRecord *rec);

#endif /* HUMANSRE_BEEPER_INTERNAL_H */
