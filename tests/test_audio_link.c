/* tests/test_audio_link.c — the core's audio state machine over the real
 * AdLib module.
 *
 * The wiring under test is the one the game uses: audio.c's play_tune /
 * music_resume / the fade pair / driver_call / the 2-of-3 tick, forwarding
 * to the linked sound-driver module through an OSystem whose audio members
 * are the module (the SDL3 backend's shape, without SDL).  The assertions
 * are on the rendered samples and on the driver's observable state, so a
 * break anywhere in the chain (loader, fade, tick, sfx) shows up here.
 *
 * Usage: test_audio_link <volume-dir>   (the dir holding HUMANS.000/001/002)
 */
#include "audio.h"
#include "compat.h"
#include "font.h"
#include "frontend.h"
#include "level.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "scene.h"
#include "video.h"

#include "audio/sound_driver.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000u

static SoundDriver *_drv;
static uint16_t _voice = 0xFFFFu;
static uint8_t _tempo_byte = 0xFFu;
static uint32_t _ticks;
static uint32_t _stops;
static uint32_t _loads;

/* --- the backend: the SDL3 one's shape, without a device -------------- */

static void _vset(VideoMode m) { (void)m; }
static uint8_t *_vpage(int32_t which)
{
    static uint8_t page[VIDEO_PAGE_BYTES];
    (void)which;
    return page;
}
static void _vpresent(int32_t which) { (void)which; }
static void _vpalette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    (void)p6;
    (void)first;
    (void)n;
}
static void _poll(InputState *out) { memset(out, 0, sizeof(*out)); }
static void _tstart(uint32_t rate, TickFn tick)
{
    (void)rate;
    (void)tick;
}
static void _tsync(uint32_t *lastSeen) { (void)lastSeen; }
static int32_t _install(void)
{
    _drv = g_sound_driver_adlib.create(RATE);
    return _drv ? 0 : -1;
}
static int32_t _load(const uint8_t *image, uint32_t size)
{
    _loads++;
    g_sound_driver_adlib.loadTune(_drv, image, size);
    return 0;
}
static void _resume(void) { _voice = g_sound_driver_adlib.startEvent(_drv, 0); }
static void _stop(void)
{
    _stops++;
    if (_voice != 0xFFFFu) g_sound_driver_adlib.releaseEvent(_drv, _voice);
    _voice = 0xFFFFu;
}
static void _sfx(uint16_t id)
{ _voice = g_sound_driver_adlib.startEvent(_drv, id); }
static void _tempo(uint8_t cl)
{
    _tempo_byte = cl;
    g_sound_driver_adlib.setTempo(_drv, cl);
}
static void _tick(void)
{
    _ticks++;
    g_sound_driver_adlib.tick(_drv);
}
static int32_t _driver_id(void) { return 1; } /* AdLib */
static void _warn(const char *msg) { fprintf(stderr, "warn: %s\n", msg); }
static void _fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

static const OSystem _osys = { _vset,  _vpage,     _vpresent,  _vpalette,
                               _poll,  _tstart,    _tsync,     _install,
                               _load,  _resume,    _stop,      _sfx,
                               _tempo, _tick,      _driver_id, _warn,
                               _fatal, &g_fs_posix };

/**
 * @brief Render some audio and return the largest absolute sample.
 *
 * @param frames the frame count
 * @return the peak magnitude
 */
static uint32_t _peak(uint32_t frames)
{
    int16_t buf[1024 * 2];
    uint32_t peak = 0;
    uint32_t i;

    while (frames > 0u) {
        uint32_t n = frames > 1024u ? 1024u : frames;
        g_sound_driver_adlib.generate(_drv, buf, n);
        for (i = 0; i < n * 2u; i++) {
            int32_t v = buf[i];
            if (v < 0) v = -v;
            if ((uint32_t)v > peak) peak = (uint32_t)v;
        }
        frames -= n;
    }
    return peak;
}

/**
 * @brief Run N ticks, rendering a chunk every 4th tick (the game's pace).
 *
 * @param n the tick count
 * @return the peak magnitude seen
 */
static uint32_t _run(uint32_t n)
{
    uint32_t peak = 0;
    uint32_t i;
    for (i = 1; i <= n; i++) {
        audio_tick();
        if ((i % 4u) == 0u) {
            uint32_t p = _peak(512);
            if (p > peak) peak = p;
        }
    }
    return peak;
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    char path[512];
    FILE *f;
    uint32_t peak;

    snprintf(path, sizeof(path), "%s/HUMANS.000", vol_dir);
    f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s\n", vol_dir);
        return 0;
    }
    fclose(f);
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    assert(audio_init(&_osys) == 0);
    res_init(&_osys);
    audio_install();
    assert(_drv != NULL);

    /* --- the boot: play_tune(mus\title) / resume / fade-in ------------ */
    audio_play_tune(audio_tune_title);
    audio_music_resume();
    audio_fade_in();
    peak = _run(600);
    assert(peak > 256u); /* audible, not the silent init state */
    assert(_tempo_byte == 0x4Au); /* 1000:84CE: the fade's last step */
    assert(_ticks > 0u);

    /* The 2-of-3 tick: 600 audio_tick calls -> 400 driver ticks.  The
     * driver's own tick counter is private; the call count is the contract
     * here and the audio proves the ticks did something. */
    /* (the fade's 600 ticks ended after 150 steps; a second fade-in is
     * latched by [0x40BA] and must not restart it) */
    audio_fade_in();
    {
        uint32_t t0 = _ticks;
        (void)_run(30);
        assert(_ticks >= t0 + 19u && _ticks <= t0 + 21u); /* 2 of 3 */
        assert(_tempo_byte == 0x4Au);
    }

    /* --- music_stop: cmd 6 ------------------------------------------- */
    audio_music_stop();
    assert(_stops >= 1u);
    {
        uint32_t t0 = _ticks;
        (void)_run(30);
        assert(_ticks >= t0 + 19u && _ticks <= t0 + 21u);
    }

    /* --- the MUSIC board's gate (1000:A5E8) --------------------------- */
    audio_music_gate(0); /* on: resume + cmd 0x0A 0x4B */
    assert(_voice != 0xFFFFu);
    assert(_tempo_byte == 0x4Bu);
    audio_music_gate(1); /* off: stop */
    assert(_voice == 0xFFFFu);

    /* --- driver_call: the effects bank when the board is off ---------- */
    frontend_option_set_for_probe(FRONTEND_OPT_MUSIC, 1u); /* music off */
    res_set_disc(1u);
    audio_play_tune(audio_tune_humfx);
    audio_fade_in();
    audio_sfx(0u); /* 1000:83B7, AX = 0 (PICKUP) */
    assert(_voice != 0xFFFFu);
    peak = _run(120);
    assert(peak > 128u); /* the effect sounded */
    audio_sfx(0x0Eu); /* the AdLib id (1) drops event 0x0E */
    audio_sfx(0xFFu); /* the "none" sentinel */

    /* --- the level's lettered tune (1000:327F) ------------------------ */
    frontend_option_set_for_probe(FRONTEND_OPT_MUSIC, 0u); /* music on */
    audio_play_level_tune(1u);
    assert(strcmp(audio_tune_lev, "mus\\levA") == 0);
    audio_play_level_tune(2u);
    assert(strcmp(audio_tune_lev, "mus\\levB") == 0);

    /* --- the disc prompt's tune (1000:2042) --------------------------- */
    assert(video_init(&_osys) == 0);
    assert(palette_init(&_osys) == 0);
    assert(font_init(&_osys) == 0);
    assert(level_init(&_osys) == 0);
    audio_music_stop();
    {
        uint32_t loads0 = _loads;
        assert(level_disk_status_begin(3u) == 1); /* no volume 3: the prompt */
        assert(_loads == loads0 + 1u); /* "mus\disk" loaded */
    }
    level_disk_status_reset();

    /* --- cd_file_op's tune (1000:B061) -------------------------------- */
    assert(scene_init(&_osys) == 0);
    scene_reset_intro();
    res_set_disc(0u);
    {
        uint32_t loads0 = _loads;
        (void)scene_play_level_intro(1u); /* the level-1 cutscene */
        assert(_loads == loads0 + 1u);
    }

    /* --- the fade-out and its deferred music_stop (1000:835A/8389) ---- */
    audio_music_resume();
    audio_fade_out();
    {
        uint32_t stops0 = _stops;
        (void)_run(160);
        assert(_tempo_byte == 0u);
        assert(_stops == stops0 + 1u);
    }

    g_sound_driver_adlib.destroy(_drv);
    _drv = NULL;
    printf("test_audio_link: ok\n");
    return 0;
}
