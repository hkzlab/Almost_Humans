/* The MT-32 module's INT 60h surface: install, the commands the game sends,
 * the sample pull and the module table.  The themes it drives are
 * mt32_synth.c, mt32_seq.c and mt32_rom.c.
 */
#include "audio/mt32/mt32_internal.h"
#include "audio/mt32/sound_driver_mt32.h"

#include "arch_mem.h"
#include "byteorder.h"

#include <mt32emu.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief cmd 1: re-probe the synth and reset it (0x0209).
 *
 * The original's probe is the MPU-401 handshake, which the port does not
 * model (D-256); the emulator context create() opened is the answer to it, so
 * a ready driver re-sends what follows a successful probe: the install reset
 * (0x0CC7) and then the voice reset (0x05A4).
 *
 * @param drv the driver; NULL is accepted
 * @return 0 on success, -1 when the driver is not ready (the original's CF)
 */
static int32_t _mt32_reset(SoundDriver *drv)
{
    if (!drv || !drv->ready) return -1;
    _mt32_install_reset(drv); // 0x0216: mt32_probe -> mt32_mpu_reset
    _mt32_reset_voices(drv);  // 0x021B
    return 0;
}

/**
 * @brief Install the MT-32 emulation (INT 60h install, 0x00DE).
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when the ROMs cannot be loaded
 */
static SoundDriver *_mt32_create(uint32_t sampleRate)
{
    mt32emu_report_handler_i report;
    const char *roms = _mt32_rom_dir();
    SoundDriver *drv;
    uint32_t i;

    if (roms[0] == '\0') {
        fprintf(stderr, "audio(mt32): no ROM directory given\n");
        return NULL;
    }
    drv = arch_mem_alloc(sizeof(*drv));
    if (!drv) return NULL;
    memset(drv, 0, sizeof(*drv));
    if (sampleRate == 0) sampleRate = 48000;
    drv->sampleRate = sampleRate;

    report.v0 = &_mt32_report_handler;
    drv->ctx = mt32emu_create_context(report, NULL);
    if (!drv->ctx) {
        fprintf(stderr, "audio(mt32): cannot create the emulator context\n");
        arch_mem_free(drv);
        return NULL;
    }
    mt32emu_set_stereo_output_samplerate(drv->ctx, (double)sampleRate);
    /* The reference emulator settings, so the port plays the
     * same instrument: accurate analog stage, 16-bit renderer, NICE DAC,
     * the three "nice" quality switches, and immediate MIDI. */
    mt32emu_set_analog_output_mode(drv->ctx, MT32EMU_AOM_ACCURATE);
    mt32emu_select_renderer_type(drv->ctx, MT32EMU_RT_BIT16S);
    mt32emu_set_dac_input_mode(drv->ctx, MT32EMU_DAC_NICE);
    mt32emu_set_nice_panning_enabled(drv->ctx, MT32EMU_BOOL_TRUE);
    mt32emu_set_nice_amp_ramp_enabled(drv->ctx, MT32EMU_BOOL_TRUE);
    mt32emu_set_nice_partial_mixing_enabled(drv->ctx, MT32EMU_BOOL_TRUE);
    mt32emu_set_midi_delay_mode(drv->ctx, MT32EMU_MDM_IMMEDIATE);
    /* The dry and reverb channels have separate gain factors; scaling both
     * makes the whole output about 6 dB quieter. */
    mt32emu_set_output_gain(drv->ctx, MT32_OUTPUT_GAIN);
    mt32emu_set_reverb_output_gain(drv->ctx, MT32_OUTPUT_GAIN);
    _mt32_load_rom_dir(drv, roms);
    if (drv->rom_count == 0 || !_mt32_add_roms(drv)) {
        fprintf(stderr, "audio(mt32): no control+PCM ROM pair found in '%s'\n",
                roms);
        mt32emu_free_context(drv->ctx);
        for (i = 0; i < drv->rom_count; i++) arch_mem_free(drv->rom[i]);
        arch_mem_free(drv);
        return NULL;
    }
    if (mt32emu_open_synth(drv->ctx) != MT32EMU_RC_OK) {
        fprintf(stderr,
                "audio(mt32): cannot open the synth with the ROMs in '%s'\n",
                roms);
        mt32emu_free_context(drv->ctx);
        for (i = 0; i < drv->rom_count; i++) arch_mem_free(drv->rom[i]);
        arch_mem_free(drv);
        return NULL;
    }
    if (mt32emu_get_actual_stereo_output_samplerate(drv->ctx) != sampleRate) {
        fprintf(stderr, "audio(mt32): mt32emu did not take %u Hz\n",
                sampleRate);
        mt32emu_close_synth(drv->ctx);
        mt32emu_free_context(drv->ctx);
        for (i = 0; i < drv->rom_count; i++) arch_mem_free(drv->rom[i]);
        arch_mem_free(drv);
        return NULL;
    }

    drv->volume = 0x4Bu;
    drv->system_block[0] = 0x00u; // CS:0x0EAA..0x0EAC's image bytes
    drv->system_block[1] = 0x04u;
    drv->system_block[2] = 0x04u;
    for (i = 0; i < MT32_SLOTS; i++) {
        drv->slot_free[i] = (uint8_t)i;
        drv->slot[i].last_program = 0xFFu;
    }
    _mt32_install_reset(drv); // 0x00DE's mt32_probe -> mt32_mpu_reset
    drv->ready = 1;
    {
        mt32emu_rom_info info;
        mt32emu_get_rom_info(drv->ctx, &info);
        fprintf(stderr, "audio(mt32): mt32emu %s, %s + %s, gain %.2f, %u Hz\n",
                mt32emu_get_library_version_string(), info.control_rom_id,
                info.pcm_rom_id, (double)MT32_OUTPUT_GAIN,
                mt32emu_get_actual_stereo_output_samplerate(drv->ctx));
    }
    return drv;
}

/**
 * @brief cmd 0: uninstall (0x0178).
 *
 * @param drv the driver; NULL is accepted
 */
static void _mt32_destroy(SoundDriver *drv)
{
    uint32_t i;

    if (!drv) return;
    mt32emu_close_synth(drv->ctx);
    mt32emu_free_context(drv->ctx);
    for (i = 0; i < drv->rom_count; i++) arch_mem_free(drv->rom[i]);
    arch_mem_free(drv->tune);
    arch_mem_free(drv);
}

/**
 * @brief cmd 0x0A: the master volume (0x01CD).
 *
 * @param drv the driver
 * @param tempo CL, the game's byte; clamped to 100
 */
static void _mt32_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    if (!drv || !drv->ready) return;
    if (tempo == drv->volume) return;
    if (tempo > 100u) tempo = 100u;
    drv->volume = tempo;
    _mt32_send_master_volume(drv);
}

/**
 * @brief cmd 0x0E: identify, the 8-byte command block (0x01BB).
 *
 * @param drv the driver
 * @return the block (CS:0x15D1)
 */
static const uint8_t *_mt32_command_block(SoundDriver *drv)
{
    static const uint8_t empty[8];
    if (!drv) return empty;
    return drv->command_block;
}

/**
 * @brief cmd 0x0C: load a tune image (0x0236).
 *
 * @param drv the driver
 * @param image the .RLD image
 * @param size its byte length
 */
static void _mt32_load_tune(SoundDriver *drv, const uint8_t *image,
                            uint32_t size)
{
    uint32_t max_order = 0;
    uint32_t patterns;
    uint32_t i;

    if (!drv || !drv->ready || !image || size < MT32_HEADER) return;

    drv->off_base = rd_le_u16(image) >= 0x9AB6u ? MT32_OFF_NEW : MT32_OFF_OLD;
    drv->record_base = drv->off_base - 0x800u;
    if (drv->off_base + 0x200u > size || drv->record_base + 0x800u > size)
        return;

    arch_mem_free(drv->tune);
    drv->tune = arch_mem_alloc(size);
    if (!drv->tune) {
        drv->tune_size = 0;
        return;
    }
    memcpy(drv->tune, image, size);
    drv->tune_size = size;

    memcpy(drv->order_list, image + 0x18u, sizeof(drv->order_list));
    memcpy(drv->instruments, image + 0x98u, sizeof(drv->instruments));
    drv->trailing = image[(drv->off_base == MT32_OFF_NEW ? 0x118u : 0xD8u)];
    for (i = 0; i < 256u; i++)
        drv->offset[i] = rd_le_u16(image + drv->off_base + 2u * i);
    drv->offset_count = 256u;

    for (i = 0; i < 0x7Fu; i++)
        if (drv->order_list[i] > max_order) max_order = drv->order_list[i];
    patterns = max_order + 1u;
    if (patterns >= drv->offset_count) patterns = drv->offset_count - 1u;
    drv->bank_base = drv->off_base + (uint32_t)drv->offset[patterns] * 16u;

    _mt32_init_timbres(drv);  // 0x0299
    _mt32_reset_voices(drv);  // 0x029C
}

/**
 * @brief Find and allocate a record's MIDI slots (0x0363).
 *
 * The original scans the eight free-list entries from the start and takes the
 * first `tracks` entries that are not `0xFF` (0x0396's `JZ` goes to `INC SI`,
 * not to the failure path); only running out of entries before the track count
 * is reached fails, which restores the whole list from the scratch copy.
 *
 * @param drv the driver
 * @param tracks the track count
 * @param slots receives the allocated slot numbers
 * @return 1 on success, 0 when the slots are full
 */
static int _alloc_record(SoundDriver *drv, uint32_t tracks, uint8_t *slots)
{
    uint8_t scratch[MT32_SLOTS];
    uint32_t taken = 0;
    uint32_t i;

    if (tracks > MT32_SLOTS) tracks = MT32_SLOTS;
    memcpy(scratch, drv->slot_free, sizeof(scratch));
    for (i = 0; i < MT32_SLOTS && taken < tracks; i++) {
        if (drv->slot_free[i] == 0xFFu) continue;
        slots[taken] = drv->slot_free[i];
        drv->slot_free[i] = 0xFFu;
        _mt32_all_notes_off(drv, _channel(slots[taken]));
        taken++;
    }
    if (taken < tracks) {
        memcpy(drv->slot_free, scratch, sizeof(scratch));
        return 0;
    }
    return 1;
}

/**
 * @brief cmd 5: start a record (0x02A7).
 *
 * @param drv the driver
 * @param event CX, the record id in the loaded image
 * @return the record handle, or 0xFFFF when refused
 */
static uint16_t _mt32_start_event(SoundDriver *drv, uint16_t event)
{
    Mt32Record *rec = NULL;
    uint32_t off;
    uint32_t tracks;
    uint32_t i;

    if (!drv || !drv->ready || !drv->tune) return 0xFFFFu;
    if (event >= 128u) return 0xFFFFu;
    off = drv->record_base + (uint32_t)event * 16u;
    if (off + 16u > drv->tune_size) return 0xFFFFu;
    for (i = 0; i < MT32_RECORDS; i++) {
        if (!drv->record[i].active) {
            rec = &drv->record[i];
            break;
        }
    }
    if (!rec) return 0xFFFFu;

    memset(drv->track_volume, 0, sizeof(drv->track_volume));
    tracks = _u8(drv, off + 0x0Du);
    memset(rec, 0, sizeof(*rec));
    if (!_alloc_record(drv, tracks, rec->track_slot)) return 0xFFFFu;

    rec->tracks = (uint8_t)(tracks > MT32_SLOTS ? MT32_SLOTS : tracks);
    rec->ticks_per_row = 6;
    rec->order = _u8(drv, off + 0x0Au);
    rec->first_order = rec->order;
    rec->loop = _u8(drv, off + 0x0Cu);
    rec->last_order = (uint8_t)(_u8(drv, off + 0x0Bu) + 1u);
    rec->finished = 0;
    for (i = 0; i < MT32_SLOTS; i++) drv->slot[i].last_program = 0xFFu;
    rec->active = 0xFFu;
    return (uint16_t)(rec - drv->record);
}

/**
 * @brief cmd 6: release a record (0x03FA).
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _mt32_release_event(SoundDriver *drv, uint16_t voice)
{
    if (!drv || voice >= MT32_RECORDS) return;
    _mt32_release_record(drv, &drv->record[voice]);
}

/**
 * @brief cmd 2: one sequencer tick (0x0792 -> 0x07A0).
 *
 * @param drv the driver
 */
static void _mt32_tick(SoundDriver *drv)
{
    uint32_t i;

    if (!drv || !drv->ready) return;
    for (i = 0; i < MT32_RECORDS; i++) _mt32_step_record(drv, &drv->record[i]);
}

/**
 * @brief Render interleaved stereo samples at the requested rate.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _mt32_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    if (!drv || !drv->ctx) {
        memset(dst, 0, (size_t)frames * 2u * sizeof(*dst));
        return;
    }
    mt32emu_render_bit16s(drv->ctx, dst, frames);
}

const SoundDriverModule g_sound_driver_mt32 = {
    _mt32_create,      _mt32_destroy,       _mt32_reset,
    _mt32_set_tempo,   _mt32_command_block, _mt32_load_tune,
    _mt32_start_event, _mt32_release_event, _mt32_tick,
    _mt32_generate
};
