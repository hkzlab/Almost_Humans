/* audio.c — the tune/sfx state machine: play_tune, music_resume/music_stop,
 * driver_call, the master-volume fade pair and the 2-of-3 music tick.
 *
 * The facts are re_snd_drv.md §2.4–§2.5; the driver itself is the linked
 * sound-driver module behind the OSystem's INT 60h calls.  Nothing here
 * knows a chip: this is the game side of the conversation.
 */
#include "audio.h"
#include "frontend.h"
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
#define AUDIO_FADE_IN_LAST 0x4A
#define AUDIO_FADE_OUT_FIRST 0x4B
/* The gate's direct write (1000:A5F7). */
#define AUDIO_TEMPO_MAX 0x4B
/* Two display periods per fade step (two wait_vsync calls, 1000:84BF/84C2). */
#define AUDIO_FADE_TICKS 2

static const OSystem *_au_osys;
static int32_t _music_active;  /* [0x40BB]: the music_resume latch   */
static int32_t _fade_latch;    /* [0x40BA]: the fade-in latch        */
static uint32_t _tick_phase;   /* [0x5AF2]: 1,2 tick; 3 resets       */
/* 1 when the backend steps the tick from its audio sample clock (the SDL3
 * render path).  The game tick then skips audio_tick: one clock only. */
static int32_t _render_clock;
static const char *_tune_name; /* [0x40B8]: the dedup key            */
static int32_t _fade_dir;      /* +1 in, -1 out, 0 idle              */
static int32_t _fade_step;     /* the CL currently applied           */
static int32_t _fade_wait;     /* ticks until the next step          */
static int32_t _record_live;   /* a cmd-5 record is playing          */
static int32_t _stop_after_fade; /* present_frames_75 then music_stop */

/**
 * @brief Bind the audio module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t audio_init(const OSystem *osys)
{
    _au_osys = osys;
    return 0;
}

/**
 * @brief Install the resident driver (driver_reset 1000:84DE).
 */
void audio_install(void)
{
    if (!_au_osys) return;
    if (_au_osys->audioInstall()) {
        fprintf(stderr, "audio: the driver did not install; the game is silent\n");
    }
}

/**
 * @brief play_tune (1000:8437): load and start a tune image.
 *
 * The original appends the driver's extension (".ALB" for AdLib) to the
 * caller's base name, INT 63h-loads it, then sends cmd 0x0C.  The dedup is
 * by name *pointer*, exactly as `[0x40B8]` compares: the first call for a
 * name loads it, every later call returns.
 *
 * @param name the base name, e.g. "mus\\title"
 */
void audio_play_tune(const char *name)
{
    char path[24];
    uint8_t *image;
    uint32_t size = 0;

    if (!_au_osys || !name) return;
    if (name == _tune_name) return; /* 1000:843E */
    _tune_name = name;

    snprintf(path, sizeof(path), "%s.ALB", name);
    image = res_load_chunk(path, &size, NULL);
    if (!image) {
        fprintf(stderr, "audio: cannot load %s\n", path);
        return;
    }
    _au_osys->audioLoadTune(image, size);
    free(image);
}

/**
 * @brief The level's lettered tune (1000:326E / 0x139B+/0x13A2).
 *
 * @param level1 the 1-based level number ([0x1F41])
 */
void audio_play_level_tune(uint32_t level1)
{
    uint32_t letter = level1 ? (level1 - 1u) % 18u : 0u;
    audio_tune_lev[7] = (char)('A' + letter);
    audio_play_tune(audio_tune_lev);
}

/**
 * @brief music_resume (1000:83F7): start the loaded image's record 0.
 *
 * Returns when the latch `[0x40BB]` is already set or the MUSIC board is
 * off (`[0x5E5B]` non-zero).  Sends cmd 5 CX=0 then cmd 0x0A CL=0.
 */
void audio_music_resume(void)
{
    if (!_au_osys || _music_active) return;
    if (frontend_option(FRONTEND_OPT_MUSIC) != 0) return; /* [0x5E5B] */
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

/**
 * @brief music_stop (1000:8389): clear the latch and release the record.
 */
void audio_music_stop(void)
{
    if (!_au_osys) return;
    _music_active = 0;
    _record_live = 0;
    _stop_after_fade = 0;
    _au_osys->audioStop();
}

/**
 * @brief The front-end frame web's music gate (1000:A5E8).
 *
 * @param off the MUSIC board's counter [0x5E5B]
 */
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

/**
 * @brief timing_loop (1000:84A5): start the master-volume fade-in.
 *
 * The original blocks for 75 x two vsyncs; the steps are spread over the
 * tick here (audio_tick), which is the same audio timeline and does not
 * stall the foreground.
 */
void audio_fade_in(void)
{
    if (!_au_osys || _fade_latch) return; /* 1000:84AF */
    _fade_dir = 1;
    _fade_step = 0;
    _fade_wait = AUDIO_FADE_TICKS;
    _au_osys->audioTempo(0);
}

/**
 * @brief present_frames_75 (1000:835A): start the master-volume fade-out.
 */
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
    _au_osys->audioTempo((uint8_t)_fade_step);
}

/**
 * @brief True while a fade is still stepping.
 *
 * @return 1 while fading, 0 when idle
 */
int32_t audio_fade_busy(void) { return _fade_dir != 0; }

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
                _au_osys->audioTempo((uint8_t)_fade_step);
            } else {
                /* 1000:84CE: the latch is set after the last hold. */
                _fade_dir = 0;
                _fade_latch = 1;
            }
        } else {
            if (_fade_step > 0) {
                _fade_step--;
                _au_osys->audioTempo((uint8_t)_fade_step);
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

/**
 * @brief The game tick's audio step: one tick, unless the backend owns it.
 *
 * This is the original's per-display-period call (1000:8B1E).  A backend that
 * drives the tick from its audio sample clock (audio_render_tick) turns this
 * into a no-op so the sequencer advances on exactly one clock.
 */
void audio_tick(void)
{
    if (_render_clock) return;
    _tick_body();
}

/**
 * @brief One audio tick, called by the backend inside its render path.
 *
 * The register writes must land on the sample the tick is due at, not on the
 * next render chunk's edge: the original's ISR writes the OPL ports
 * synchronously.  See arch/sdl3/audio.c.
 */
void audio_render_tick(void) { _tick_body(); }

/**
 * @brief Say whether the backend drives the audio tick from the sample clock.
 *
 * @param on 1 while an audio stream is running and calls audio_render_tick
 */
void audio_set_render_clock(int32_t on) { _render_clock = on; }

/**
 * @brief driver_call (1000:83B7): start a sound-effect record.
 *
 * @param id the caller's event id
 */
void audio_sfx(uint32_t id)
{
    if (!_au_osys) return;
    if ((uint8_t)id == 0xFFu) return;                     /* 1000:83C1 */
    if (frontend_option(FRONTEND_OPT_MUSIC) == 0) return; /* 1000:83C5 */
    /* 1000:83CC: the Beeper driver (id 0) has no cmd 5.  This port links the
     * AdLib module (id 1), so the id-3 event 0x0E is always dropped
     * (1000:83D3). */
    if (id == 0x0Eu) return;
    _record_live = 1; /* the driver_call stores the sfx handle in [0x5AF0] */
    _au_osys->audioSfx((uint16_t)id);
}
