/* The AdLib module's INT 60h surface: install, the commands the game sends,
 * the sample pull and the module table.  The themes it drives are
 * adlib_opl.c and adlib_seq.c.
 */
#include "audio/adlib/adlib_internal.h"
#include "audio/adlib/sound_driver_adlib.h"

#include "arch_mem.h"
#include "byteorder.h"
#include "thirdparty/opl3.h"

#include <stdint.h>
#include <string.h>

/**
 * @brief Install the AdLib driver emulation (INT 60h install, 1E95:00DD).
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when out of memory
 */
static SoundDriver *_adlib_create(uint32_t sampleRate)
{
    SoundDriver *drv = arch_mem_alloc(sizeof(*drv));
    uint32_t i;

    if (!drv) return NULL;
    memset(drv, 0, sizeof(*drv));
    if (sampleRate == 0) sampleRate = 48000;
    drv->sampleRate = sampleRate;
    OPL3_Reset(&drv->opl, sampleRate);
    _adlib_opl_probe_reset(drv); // 1E95:0DDF
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
    arch_mem_free(drv->tune);
    arch_mem_free(drv);
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
    value = (uint8_t)((0xA3u * (uint32_t)tempo) >> 8);
    if (value == drv->tempo) return;
    drv->tempo = value;
    _adlib_apply_tempo(drv);
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
    uint32_t i;

    for (i = 0; i < ADLIB_VOICES; i++) {
        drv->slot[i].last_instrument = 0;
        _adlib_silence(drv, i);
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
    uint32_t max_order = 0;
    uint32_t patterns;
    uint32_t records;
    uint32_t stream;
    uint32_t table;
    uint32_t sds;
    uint32_t table_end;
    uint32_t i;

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

    arch_mem_free(drv->tune);
    drv->tune = arch_mem_alloc(size);
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
    _reset_voices(drv); // 1E95:031E
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
    uint32_t index;
    AdlibRecord *rec = NULL;
    uint32_t off;
    uint32_t tracks;
    uint32_t i;

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
        uint32_t slot;
        for (slot = 0; slot < ADLIB_TRACKS; slot++)
            if (drv->slot_free[slot] != 0xFFu) break;
        if (slot == ADLIB_TRACKS) {
            /* The original's no-free-slot path (1E95:044F) is unreachable in
             * this title; give back what this call already took. */
            uint32_t j;
            for (j = 0; j < i; j++) {
                uint32_t taken = rec->track_slot[j];
                drv->slot_free[taken] = (uint8_t)taken;
                _adlib_silence(drv, taken);
            }
            memset(rec, 0, sizeof(*rec));
            return 0xFFFF;
        }
        drv->slot_free[slot] = 0xFFu;
        rec->track_slot[i] = (uint8_t)slot;
        /* 1E95:0437 passes voice+1 to the silence helper; kept as found. */
        _adlib_silence(drv, slot + 1u);
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
    _adlib_release_record(drv, &drv->record[voice]);
}

/**
 * @brief cmd 2: step the sequencer one tick (1E95:05CA).
 *
 * @param drv the driver
 */
static void _adlib_tick(SoundDriver *drv)
{
    uint32_t i;

    if (!drv || !drv->ready) return;
    for (i = 0; i < ADLIB_RECORDS; i++)
        _adlib_step_record(drv, &drv->record[i]);
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
        /* Resampled from the chip's 49716 Hz: OPL3_Generate at 48 kHz would
         * play about 3.5% flat. */
        OPL3_GenerateResampled(&drv->opl, sample);
        dst[2u * i] = sample[0];
        dst[2u * i + 1u] = sample[1];
    }
}

const SoundDriverModule g_sound_driver_adlib = {
    _adlib_create,      _adlib_destroy,       NULL,
    _adlib_set_tempo,   _adlib_command_block, _adlib_load_tune,
    _adlib_start_event, _adlib_release_event, _adlib_tick,
    _adlib_generate
};
