/* The Beeper module's INT 60h surface: install, the commands the game sends,
 * the sample pull and the module table.  The themes it drives are
 * beeper_speaker.c and beeper_seq.c.
 */
#include "audio/beeper/beeper_internal.h"
#include "audio/beeper/sound_driver_beeper.h"

#include "arch_mem.h"
#include "byteorder.h"
#include "thirdparty/pcspeaker_impulse.h"

#include <stdint.h>
#include <string.h>

/**
 * @brief cmd 2: one driver tick - the records and the speaker multiplex.
 *
 * 1E95:051A: step every active record, turn the speaker on/off when record
 * 0's active flag changes, then split the row window between slots 0 and 1.
 *
 * @param drv the driver
 */
static void _beeper_tick(SoundDriver *drv)
{
    uint32_t i;
    uint8_t active;

    if (!drv || !drv->ready) return;
    for (i = 0; i < BEEP_RECORDS; i++)
        _beeper_step_record(drv, &drv->record[i]);

    /* 1E95:0543: the global speaker follows record 0's active edge. */
    active = drv->record[0].active;
    if (active != drv->record0_seen) {
        drv->record0_seen = active;
        if (active) _beeper_speaker_on(drv);
        else _beeper_speaker_off(drv);
    }

    /* 1E95:055D: slot 0 at the row boundary, slot 1 at half the row. */
    drv->mux_counter++;
    if (drv->mux_counter >= drv->mux_period) {
        drv->mux_counter = 0;
        _beeper_switch_slot(drv, 0);
    } else if (drv->mux_counter == drv->mux_half) {
        _beeper_switch_slot(drv, 1);
    }
}

/**
 * @brief Install the Beeper driver emulation (INT 60h install, 1E95:00DE).
 *
 * The original's probe (1E95:0893) silences the speaker and programs a zero
 * divider before setting the ready flag.
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when out of memory
 */
static SoundDriver *_beeper_create(uint32_t sampleRate)
{
    SoundDriver *drv = arch_mem_alloc(sizeof(*drv));

    if (!drv) return NULL;
    memset(drv, 0, sizeof(*drv));
    if (sampleRate == 0) sampleRate = 48000;
    drv->sampleRate = sampleRate;
    drv->ready = 1;
    drv->tempo = BEEP_TEMPO_DEFAULT;
    drv->slot_free[0] = 0;
    drv->slot_free[1] = 1;
    /* The image's initial CS:0x0E85/0E86 are both 6; cmd 5 sets 6 and 3. */
    drv->mux_period = 6;
    drv->mux_half = 6;
    drv->speaker = pcspeaker_impulse_create(sampleRate);
    if (!drv->speaker) {
        arch_mem_free(drv);
        return NULL;
    }
    _beeper_speaker_off(drv);
    _beeper_set_divider(drv, 0);
    return drv;
}

/**
 * @brief cmd 0: uninstall (1E95:019E).
 *
 * @param drv the driver; NULL is accepted
 */
static void _beeper_destroy(SoundDriver *drv)
{
    if (!drv) return;
    pcspeaker_impulse_destroy(drv->speaker);
    arch_mem_free(drv->tune);
    arch_mem_free(drv);
}

/**
 * @brief cmd 0x0A: the master-volume byte (1E95:01DB, no effect).
 *
 * @param drv the driver
 * @param tempo CL, the game's byte; clamped to 0x64
 */
static void _beeper_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    uint8_t value;

    if (!drv) return;
    if (tempo > 0x64u) tempo = 0x64u;
    value = (uint8_t)((0xA3u * (uint32_t)tempo) >> 8);
    if (value != drv->tempo) drv->tempo = value; // 1E95:01FF is a RET
}

/**
 * @brief cmd 0x0E: identify, the 8-byte command block (1E95:01BE).
 *
 * @param drv the driver
 * @return the block (CS:0x0E87), all zero as observed
 */
static const uint8_t *_beeper_command_block(SoundDriver *drv)
{
    static const uint8_t block[8];
    (void)drv;
    return block;
}

/**
 * @brief cmd 0x0C: load a tune image and derive its geometry (1E95:022E).
 *
 * @param drv the driver
 * @param image the .BEP image
 * @param size its byte length
 */
static void _beeper_load_tune(SoundDriver *drv, const uint8_t *image,
                              uint32_t size)
{
    uint32_t max_order = 0;
    uint32_t patterns;
    uint32_t records;
    uint32_t stream;
    uint32_t table;
    uint32_t sds;
    uint32_t i;

    if (!drv || !drv->ready || !image || size < BEEP_HEADER) return;

    /* 1E95:04E3: P = max(order[0..0x7E]) + 1; K = header[0x11B]. */
    for (i = 0; i < 0x7Fu; i++)
        if (image[0x18u + i] > max_order) max_order = image[0x18u + i];
    patterns = max_order + 1u;
    records = image[0x11Bu];
    stream = BEEP_HEADER + records * 16u;
    table = ((patterns * 2u) & 0xFFF0u) + 0x10u;
    sds = stream + table;
    if (sds > size) return;
    if (table / 2u > BEEP_OFFSETS) return;

    arch_mem_free(drv->tune);
    drv->tune = arch_mem_alloc(size);
    if (!drv->tune) {
        drv->tune_size = 0;
        return;
    }
    memcpy(drv->tune, image, size);
    drv->tune_size = size;
    memcpy(drv->header, image, BEEP_HEADER);

    for (i = 0; i < table / 2u; i++)
        drv->offset[i] = rd_le_u16(image + stream + 2u * i);
    drv->offset_count = (uint16_t)(table / 2u);
    drv->song_data = sds;
    _beeper_speaker_off(drv); // 1E95:04DD at the end of cmd 0x0C
}

/**
 * @brief cmd 5: start a record as a voice set (1E95:02B0).
 *
 * The handle is the record index; the original returns its record pointer,
 * which the game only stores and hands back to cmd 6.
 *
 * @param drv the driver
 * @param event CX, the record index in the loaded image
 * @return the handle, or 0xFFFF when refused
 */
static uint16_t _beeper_start_event(SoundDriver *drv, uint16_t event)
{
    BeeperRecord *rec = NULL;
    uint32_t index;
    uint32_t tracks;
    uint32_t i;
    uint32_t off;

    if (!drv || !drv->ready || !drv->tune) return 0xFFFF;
    if (event >= drv->header[0x11Bu]) return 0xFFFF;
    for (index = 0; index < BEEP_RECORDS; index++) {
        if (!drv->record[index].active) {
            rec = &drv->record[index];
            break;
        }
    }
    if (!rec) return 0xFFFF;

    off = BEEP_HEADER + (uint32_t)event * 16u;
    tracks = _tune_u8(drv, off + 0xDu);
    if (tracks > BEEP_SLOTS) tracks = BEEP_SLOTS; // shortcut: 1 or 2 ship

    memset(rec, 0, sizeof(*rec)); // 1E95:035B copies the zero template
    for (i = 0; i < tracks; i++) {
        uint32_t slot = 0;
        while (slot < BEEP_SLOTS && drv->slot_free[slot] == 0xFFu) slot++;
        if (slot == BEEP_SLOTS) {
            /* No free speaker slot: the original's failure path. */
            memset(rec, 0, sizeof(*rec));
            return 0xFFFF;
        }
        drv->slot_free[slot] = 0xFFu;
        rec->track_slot[i] = (uint8_t)slot;
        _beeper_speaker_off(drv); // 1E95:04E0, once per track
    }

    rec->tracks = (uint8_t)tracks;
    rec->ticks_per_row = 6;
    rec->order = _tune_u8(drv, off + 0xAu);
    rec->first_order = rec->order;
    rec->cursor = (uint16_t)drv->song_data;
    rec->loop = _tune_u8(drv, off + 0xCu);
    rec->last_order = (uint8_t)(_tune_u8(drv, off + 0xBu) + 1u);
    drv->mux_period = 6;
    drv->mux_half = 3;
    /* 1E95:033D/0341: both slots' last-instrument byte is 0xFF. */
    drv->slot[0][BEEP_LAST_INSTRUMENT] = 0xFFu;
    drv->slot[1][BEEP_LAST_INSTRUMENT] = 0xFFu;
    rec->active = 0xFFu;
    return (uint16_t)index;
}

/**
 * @brief cmd 6: release a record (1E95:03F9 -> 1E95:03D1).
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _beeper_release_event(SoundDriver *drv, uint16_t voice)
{
    if (!drv || voice >= BEEP_RECORDS) return;
    _beeper_release_record(drv, &drv->record[voice]);
}

/**
 * @brief Render the PC-speaker output.
 *
 * The impulse model already produces silence when the gate is off (its
 * accumulator decays), so the driver only has to pass the sink through.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _beeper_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    if (!drv) {
        memset(dst, 0, (size_t)frames * 2u * sizeof(*dst));
        return;
    }
    pcspeaker_impulse_generate(drv->speaker, dst, frames);
}

const SoundDriverModule g_sound_driver_beeper = {
    _beeper_create,      _beeper_destroy,       NULL,
    _beeper_set_tempo,   _beeper_command_block, _beeper_load_tune,
    _beeper_start_event, _beeper_release_event, _beeper_tick,
    _beeper_generate
};
