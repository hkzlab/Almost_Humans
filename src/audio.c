#include "audio.h"
#include "frontend.h"
#include "osystem.h"
#include "res.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 202F:002A / 0x77A / 0xF91 / 0x11AA / 0x1391 / 0x139B. */
const char audio_tune_title[] = "mus\\title";
const char audio_tune_hiscore[] = "mus\\hiscore";
const char audio_tune_disk[] = "mus\\disk";
const char audio_tune_party[] = "mus\\party";
const char audio_tune_humfx[] = "mus\\humfx";
char audio_tune_lev[] = "mus\\levA";

/* The fade-in's last step is 0x4A (1000:84CE), the fade-out's first is
 * 0x4B (1000:8364); both map to master volume 0x2F in the driver. */
#define AUDIO_FADE_IN_LAST   0x4A
#define AUDIO_FADE_OUT_FIRST 0x4B
/* The gate's direct write (1000:A5F7). */
#define AUDIO_TEMPO_MAX 0x4B
/* Two display periods per fade step (two wait_vsync calls, 1000:84BF/84C2). */
#define AUDIO_FADE_TICKS 2

static const OSystem *_au_osys;
static uint8_t _music_active;  // [0x40BB]: the music_resume latch
static uint8_t _fade_latch;    // [0x40BA]: the fade-in latch
static uint16_t _tick_phase;   // [0x5AF2]: 1,2 tick; 3 resets
/* 1 when the backend steps the tick from its audio sample clock (the SDL3
 * render path).  The game tick then skips audio_tick: one clock only. */
static int32_t _render_clock;
static const char *_tune_name; // [0x40B8]: the dedup key
static int8_t _fade_dir;      // +1 in, -1 out, 0 idle
static uint8_t _fade_step;     // the CL currently applied
static uint8_t _fade_wait;     // ticks until the next step
static uint8_t _record_live;   // a cmd-5 record is playing
static uint8_t _stop_after_fade; // present_frames_75 then music_stop
static uint8_t _tune_missing;  // the last play_tune found no image

int32_t audio_init(const OSystem *osys)
{
    _au_osys = osys;
    return 0;
}

void audio_install(void)
{
    if (!_au_osys) return;
    if (_au_osys->audioInstall()) {
        _au_osys->warn("audio: the driver did not install; the game is silent");
    }
}

/**
 * @brief The launcher's [0x5AFE] sound_driver_id from the backend.
 *
 * @return -1 none, 0 Beeper, 1 AdLib, other MT-32
 */
static int32_t _driver_id(void)
{
    if (!_au_osys || !_au_osys->audioDriverId) return 1;
    return _au_osys->audioDriverId();
}

void audio_play_tune(const char *name)
{
    char path[24];
    char msg[96];
    uint8_t *image;
    uint32_t size = 0;
    int32_t id;
    const char *ext;

    if (!_au_osys || !name) return;
    if (name == _tune_name) return; // 1000:843E
    _tune_name = name;

    id = _driver_id();
    if (id == -1) return; // 1000:8449: no driver loaded
    ext = id == 0 ? ".BEP" : (id == 1 ? ".ALB" : ".RLD");

    snprintf(path, sizeof(path), "%s%s", name, ext);
    image = res_load_chunk(path, &size, NULL);
    if (!image) {
        snprintf(msg, sizeof msg,
                 "audio: cannot load %s; the record is silenced", path);
        _au_osys->warn(msg);
        _tune_missing = 1;
        audio_music_stop(); // D-252: silence instead of the original's exit
        return;
    }
    _tune_missing = 0;
    /* D-264: LEVJ.ALB (DOOM, level 82) is the one level arrangement whose
     * record 0 ships loop=0, so the AdLib driver releases the tune after one
     * pass and the level goes silent.  The resolved name proves the file is
     * this exact tune; the byte test keeps every already-looping tune (and
     * the MT-32 .RLD) untouched. */
    if (strcmp(path, "mus\\levJ.ALB") == 0 && size > 0x164u &&
        image[0x164u] == 0u) {
        image[0x164u] = 1u;
        snprintf(msg, sizeof msg,
                 "audio: %s record 0 has loop=0; forcing it to loop", path);
        _au_osys->warn(msg);
    }
    _au_osys->audioLoadTune(image, size);
    free(image);
}

void audio_play_level_tune(uint16_t level1)
{
    uint8_t letter = level1 ? (level1 - 1u) % 18u : 0u;
    audio_tune_lev[7] = (char)('A' + letter);
    audio_play_tune(audio_tune_lev);
}

void audio_music_resume(void)
{
    if (!_au_osys || _music_active) return;
    /* D-252: a tune that could not be loaded must not restart the previous
     * driver image; the game stays silent until a later play_tune succeeds. */
    if (_tune_missing) return;
    if (frontend_option(FRONTEND_OPT_MUSIC) != 0) return; // [0x5E5B]
    /* Every site in the original reaches music_resume *after* a completed
     * (blocking) music_stop.  The port's fades do not block, so a previous
     * record can still be live here: release it first (the same one-record
     * state the original ends up in), and cancel its deferred stop. */
    if (_record_live) audio_music_stop();
    _stop_after_fade = 0;
    _au_osys->audioResume();
    _record_live = 1;
    _au_osys->audioTempo(0);
    _music_active = 1;
}

void audio_music_stop(void)
{
    if (!_au_osys) return;
    _music_active = 0;
    _record_live = 0;
    _stop_after_fade = 0;
    _au_osys->audioStop();
}

void audio_music_gate(uint32_t off)
{
    if (!_au_osys) return;
    if (off) {
        if (_music_active) audio_music_stop();
        return;
    }
    /* 1000:A5F4: music_resume (its own [0x40BB] latch); 1000:A5F7: cmd
     * 0x0A with CL=0x4B runs *every frame*, latch or not. */
    audio_music_resume();
    _au_osys->audioTempo(AUDIO_TEMPO_MAX);
}

void audio_fade_in(void)
{
    if (!_au_osys || _fade_latch) return; // 1000:84AF
    _fade_dir = 1;
    _fade_step = 0;
    _fade_wait = AUDIO_FADE_TICKS;
    _au_osys->audioTempo(0);
}

void audio_fade_out(void)
{
    if (!_au_osys) return;
    _fade_dir = -1;
    /* The original blocks inside present_frames_75, so its [0x40BA] is
     * cleared before the next screen's timing_loop runs.  The port's fade
     * does not block; clearing the latch at the start is what lets the next
     * screen's fade-in run (the abandoned fade-out is overridden by it). */
    _fade_latch = 0;
    _fade_step = AUDIO_FADE_OUT_FIRST;
    _fade_wait = AUDIO_FADE_TICKS;
    /* 1000:835A is always followed by music_stop (1000:8389): defer the
     * stop to the end of the ramp, exactly as the blocking loop does. */
    _stop_after_fade = 1;
    _au_osys->audioTempo(_fade_step);
}

uint8_t audio_fade_busy(void) { return _fade_dir != 0; }

/**
 * @brief The tick body: the fade step and cmd 2 on 2 of every 3 ticks.
 *
 * The fade pair runs in the original's foreground, the music tick in its
 * INT 8 ISR; both are per-display-period here, so they share this function.
 */
static void _tick_body(void)
{
    if (!_au_osys) return;

    if (_fade_dir != 0 && --_fade_wait <= 0) {
        _fade_wait = AUDIO_FADE_TICKS;
        if (_fade_dir > 0) {
            if (_fade_step < AUDIO_FADE_IN_LAST) {
                _fade_step++;
                _au_osys->audioTempo(_fade_step);
            } else {
                /* 1000:84CE: the latch is set after the last hold. */
                _fade_dir = 0;
                _fade_latch = 1;
            }
        } else {
            if (_fade_step > 0) {
                _fade_step--;
                _au_osys->audioTempo(_fade_step);
            } else {
                /* 1000:8379/1000:8389: the stop that always follows the
                 * fade-out runs when the ramp ends. */
                _fade_dir = 0;
                if (_stop_after_fade) audio_music_stop();
            }
        }
    }

    /* 1000:8B1E: INC [0x5AF2]; == 3 -> 0 and skip; else cmd 2. */
    _tick_phase++;
    if (_tick_phase == 3u) {
        _tick_phase = 0;
        return;
    }
    _au_osys->audioTick();
}

void audio_tick(void)
{
    if (_render_clock) return;
    _tick_body();
}

void audio_render_tick(void) { _tick_body(); }

void audio_set_render_clock(uint8_t on) { _render_clock = on; }

void audio_sfx(uint32_t id)
{
    if (!_au_osys) return;
    if ((uint8_t)id == 0xFFu) return; // 1000:83C1
    if (frontend_option(FRONTEND_OPT_MUSIC) == 0) return; // 1000:83C5
    /* 1000:83CC: the Beeper driver (id 0) never gets cmd 5. */
    if (_driver_id() == 0) return;
    /* 1000:83D3: event 0x0E is the one MT-32 configuration's. */
    if (id == 0x0Eu && _driver_id() != 3) return;
    _record_live = 1; // the driver_call stores the sfx handle in [0x5AF0]
    _au_osys->audioSfx((uint16_t)id);
}
