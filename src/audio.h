/* audio.h — the interface of src/audio.c: the tune/sfx state machine
 * (game_structure.md §1 subsystem 7) over the OSystem's INT 60h calls. */

#ifndef HUMANSRE_AUDIO_H
#define HUMANSRE_AUDIO_H

#include "osystem.h"

/* The game's tune base names (202F:002A, 0x77A, 0xF91, 0x11AA, 0x1391,
 * 0x139B and 0x62E3/0x62EC — re_snd_drv.md §2.5).  The address is the
 * dedup key in the original, so they are objects here, not string
 * literals: `audio_tune_lev` is the mutable 0x139B buffer whose letter at
 * +7 is [0x13A2]. */
extern const char audio_tune_title[];
extern const char audio_tune_hiscore[];
extern const char audio_tune_disk[];
extern const char audio_tune_party[];
extern const char audio_tune_humfx[];
extern char audio_tune_lev[]; /* "mus\levA"; the letter is rewritten */

int32_t audio_init(const OSystem *osys);
/* driver_reset's install (1000:84DE) — once, at boot. */
void audio_install(void);

/* The INT 8 body's share (1000:8B1E + the fade pair): cmd 2 on 2 of every 3
 * ticks and one master-volume step every 2 ticks.  Called from _game_tick;
 * a backend that owns the audio clock (SDL3's render path) turns this into a
 * no-op and calls audio_render_tick instead. */
void audio_tick(void);

/* One audio tick at the point in the sample stream where it is due, called by
 * the backend from inside its render path (arch/sdl3/audio.c).  Keeps the
 * sequencer's register writes sample-exact. */
void audio_render_tick(void);

/* Declare that the backend drives the audio tick from its sample clock, so
 * audio_tick (the game-tick call) must not step it too. */
void audio_set_render_clock(int32_t on);

/* play_tune (1000:8437): build "<name>.ALB", INT 63h-load it, cmd 0x0C.
 * The name is deduplicated by pointer exactly as [0x40B8] does. */
void audio_play_tune(const char *name);
/* level_status_update's lettered tune (1000:327F): "mus\levX" with
 * X = 'A' + (level1 - 1) % 18. */
void audio_play_level_tune(uint32_t level1);

/* music_resume (1000:83F7): cmd 5 CX=0, master volume 0, latch [0x40BB]. */
void audio_music_resume(void);
/* music_stop (1000:8389): clear [0x40BB], cmd 6. */
void audio_music_stop(void);

/* the front-end frame web's gate (1000:A5E8): `off` = the MUSIC board's
 * counter [0x5E5B] non-zero -> music_stop; zero -> music_resume + cmd 0x0A
 * 0x4B (1000:A5F7).  One call per room tick. */
void audio_music_gate(uint32_t off);

/* timing_loop (1000:84A5): master volume 0..0x4A, two ticks per step,
 * latched by [0x40BA].  Non-blocking: the steps run from audio_tick. */
void audio_fade_in(void);
/* present_frames_75's fade (1000:835A): 0x4B..0, two ticks per step, then
 * clears [0x40BA]. */
void audio_fade_out(void);
/* true while a fade is stepping (the sites that blocked on it can wait). */
int32_t audio_fade_busy(void);

/* driver_call (1000:83B7): cmd 5 CX=id, gated by [0x5E5B] (music off), the
 * driver id (the port links AdLib, id 1, so event 0x0E is dropped) and the
 * 0xFF sentinel. */
void audio_sfx(uint32_t id);

#endif /* HUMANSRE_AUDIO_H */
