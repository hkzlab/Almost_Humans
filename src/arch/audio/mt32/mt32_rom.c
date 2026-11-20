/* The MT-32 emulator plumbing: the ROM directory the module reads through
 * the filesystem module, the ROM pairing, and the mt32emu report handler.
 */
#include "audio/mt32/mt32_internal.h"
#include "audio/mt32/sound_driver_mt32.h"

#include "arch_mem.h"
#include "fs/fs.h"

#include <mt32emu.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The ROM pair to prefer, newest MT-32 model first.  `mt32emu_open_synth`
 * picks the emulated machine from the control ROM, so a directory holding
 * several models (the MAME-versioned sets) must be chosen explicitly instead
 * of letting the directory order decide.  The ids are mt32emu's own
 * `control_rom_id`/`pcm_rom_id` (mt32emu_get_rom_ids). */
static const char *_rom_preference[][2] = {
    { "ctrl_mt32_2_07", "pcm_mt32" }, // MT-32 "new", latest revision
    { "ctrl_mt32_2_06", "pcm_mt32" },
    { "ctrl_mt32_2_04", "pcm_mt32" },
    { "ctrl_mt32_2_03", "pcm_mt32" },
    { "ctrl_mt32_1_07", "pcm_mt32" }, // MT-32 "old", latest revision
    { "ctrl_mt32_1_06", "pcm_mt32" },
    { "ctrl_mt32_1_05", "pcm_mt32" },
    { "ctrl_mt32_1_04", "pcm_mt32" },
    { "ctrl_mt32_bluer", "pcm_mt32" },
    { "ctrl_cm32l_1_02", "pcm_cm32l" }, // only when no MT-32 ROM is here
    { "ctrl_cm32l_1_00", "pcm_cm32l" },
    { "ctrl_cm32ln_1_00", "pcm_cm32l" }
};

/* The module the ROM directory is read through; set before create(). */
static const FSModule *_fs;
static char _rom_dir[1024];

void sound_driver_mt32_set_roms(const FSModule *fs, const char *path)
{
    _fs = fs;
    if (!path) {
        _rom_dir[0] = '\0';
        return;
    }
    snprintf(_rom_dir, sizeof(_rom_dir), "%s", path);
}

/**
 * @brief The directory sound_driver_mt32_set_roms() selected.
 *
 * @return the path, empty when none was given
 */
const char *_mt32_rom_dir(void) { return _rom_dir; }

uint8_t *_mt32_read_file(const char *path, size_t *size)
{
    FSFile *f = _fs->openFile(path);
    uint32_t n;
    uint8_t *buf;

    if (!f) return NULL;
    n = _fs->fileSize(f);
    if (n == 0) {
        _fs->closeFile(f);
        return NULL;
    }
    buf = arch_mem_alloc(n);
    if (!buf) {
        _fs->closeFile(f);
        return NULL;
    }
    if (_fs->fileRead(f, 0, buf, n) != n) {
        arch_mem_free(buf);
        _fs->closeFile(f);
        return NULL;
    }
    _fs->closeFile(f);
    *size = n;
    return buf;
}

void _mt32_load_rom_dir(SoundDriver *drv, const char *dir)
{
    char names[FS_DIR_LIST_BYTES];
    char path[1200];
    int32_t count;
    int32_t i;
    const char *name = names;

    if (!_fs) return;
    count = _fs->listDir(dir, names, sizeof names);
    for (i = 0; i < count; i++) {
        size_t dl = strlen(dir), nl = strlen(name);
        size_t size;
        uint8_t *buf;

        if (dl + 1u + nl >= sizeof path) { // cannot fit: no such file
            name += nl + 1u;
            continue;
        }
        memcpy(path, dir, dl);
        path[dl] = '/';
        memcpy(path + dl + 1u, name, nl + 1u);
        name += nl + 1u;
        buf = _mt32_read_file(path, &size);
        if (!buf) continue;
        if (drv->rom_count < MT32_ROMS_MAX) {
            drv->rom[drv->rom_count] = buf;
            drv->rom_size[drv->rom_count] = size;
            drv->rom_count++;
        } else {
            arch_mem_free(buf);
        }
    }
}

int32_t _mt32_add_roms(SoundDriver *drv)
{
    const uint32_t pairs =
        (uint32_t)(sizeof(_rom_preference) / sizeof(_rom_preference[0]));
    mt32emu_rom_info info;
    uint32_t i;
    uint32_t j;

    for (i = 0; i < drv->rom_count; i++) {
        memset(&info, 0, sizeof(info));
        (void)mt32emu_identify_rom_data(&info, drv->rom[i], drv->rom_size[i],
                                        NULL);
        drv->rom_control[i] = info.control_rom_id;
        drv->rom_pcm[i] = info.pcm_rom_id;
    }

    /* The newest MT-32 model whose full control and PCM images are both in
     * the directory.  This makes a multi-ROM set deterministic instead of
     * whichever file readdir happened to return last. */
    for (i = 0; i < pairs; i++) {
        int32_t c = -1;
        int32_t p = -1;
        for (j = 0; j < drv->rom_count; j++) {
            if (c < 0 && drv->rom_control[j] &&
                strcmp(drv->rom_control[j], _rom_preference[i][0]) == 0)
                c = (int32_t)j;
            if (p < 0 && drv->rom_pcm[j] &&
                strcmp(drv->rom_pcm[j], _rom_preference[i][1]) == 0)
                p = (int32_t)j;
        }
        if (c < 0 || p < 0) continue;
        (void)mt32emu_add_rom_data(drv->ctx, drv->rom[c], drv->rom_size[c],
                                   NULL);
        (void)mt32emu_add_rom_data(drv->ctx, drv->rom[p], drv->rom_size[p],
                                   NULL);
        return 1;
    }

    /* No full pair: hand everything over and let the pairing entry point
     * merge MAME-style split images. */
    for (i = 0; i < drv->rom_count; i++)
        (void)mt32emu_add_rom_data(drv->ctx, drv->rom[i], drv->rom_size[i],
                                   NULL);
    mt32emu_get_rom_info(drv->ctx, &info);
    if (info.control_rom_id && info.pcm_rom_id) return 1;

    for (i = 0; i < drv->rom_count; i++) {
        for (j = 0; j < drv->rom_count; j++) {
            if (i == j) continue;
            (void)mt32emu_merge_and_add_rom_data(
                drv->ctx, drv->rom[i], drv->rom_size[i], NULL, drv->rom[j],
                drv->rom_size[j], NULL);
            mt32emu_get_rom_info(drv->ctx, &info);
            if (info.control_rom_id && info.pcm_rom_id) return 1;
        }
    }
    return 0;
}

/**
 * @brief The emulator's report callbacks: silence debug and LCD chatter.
 *
 * A NULL handler makes some control ROMs write their LCD text to stderr, so
 * the port installs a handler that drops everything except the two ROM
 * errors (which create() already reports through the return code).
 */
static mt32emu_report_handler_version MT32EMU_C_CALL
_report_version(mt32emu_report_handler_i i)
{
    (void)i;
    return MT32EMU_REPORT_HANDLER_VERSION_0;
}

/* Drop the debug output. */
static void MT32EMU_C_CALL _report_debug(void *data, const char *fmt,
                                         va_list list)
{
    (void)data;
    (void)fmt;
    (void)list;
}

/* Drop the control-ROM error (create() reports it). */
static void MT32EMU_C_CALL _report_error_control(void *data) { (void)data; }

/* Drop the PCM-ROM error (create() reports it). */
static void MT32EMU_C_CALL _report_error_pcm(void *data) { (void)data; }

/* Drop the LCD text (some ROMs write it to stderr). */
static void MT32EMU_C_CALL _report_lcd(void *data, const char *message)
{
    (void)data;
    (void)message;
}

/* Drop the played-note echo. */
static void MT32EMU_C_CALL _report_midi_played(void *data) { (void)data; }

/* Never ask the host to slow down. */
static mt32emu_boolean MT32EMU_C_CALL _report_queue_overflow(void *data)
{
    (void)data;
    return MT32EMU_BOOL_FALSE;
}

/* Drop the realtime message. */
static void MT32EMU_C_CALL _report_realtime(void *data, mt32emu_bit8u byte)
{
    (void)data;
    (void)byte;
}

/* Drop the reset notice. */
static void MT32EMU_C_CALL _report_reset(void *data) { (void)data; }

/* Drop the reconfig notice. */
static void MT32EMU_C_CALL _report_reconfig(void *data) { (void)data; }

/* Drop the reverb-mode echo. */
static void MT32EMU_C_CALL _report_reverb_mode(void *data, mt32emu_bit8u mode)
{
    (void)data;
    (void)mode;
}

/* Drop the reverb-time echo. */
static void MT32EMU_C_CALL _report_reverb_time(void *data, mt32emu_bit8u time)
{
    (void)data;
    (void)time;
}

/* Drop the reverb-level echo. */
static void MT32EMU_C_CALL _report_reverb_level(void *data, mt32emu_bit8u level)
{
    (void)data;
    (void)level;
}

/* Drop the poly change echo. */
static void MT32EMU_C_CALL _report_poly(void *data, mt32emu_bit8u part)
{
    (void)data;
    (void)part;
}

/* Drop the program change echo. */
void MT32EMU_C_CALL _report_program(void *data, mt32emu_bit8u part,
                                           const char *group, const char *patch)
{
    (void)data;
    (void)part;
    (void)group;
    (void)patch;
}

const mt32emu_report_handler_i_v0 _mt32_report_handler = {
    _report_version,        _report_debug,       _report_error_control,
    _report_error_pcm,      _report_lcd,         _report_midi_played,
    _report_queue_overflow, _report_realtime,    _report_reset,
    _report_reconfig,       _report_reverb_mode, _report_reverb_time,
    _report_reverb_level,   _report_poly,        _report_program
};
