/* The tune/sfx state machine over the INT 60h calls.
 *
 *   play_tune             1000:8437
 *   music_resume/stop     1000:83F7 / 8389
 *   driver_call           1000:83B7
 *   driver_reset          1000:84DE
 *   timing_loop           1000:84A5   the master-volume fade in/out
 *   present_frames_75     1000:835A
 *   the frame web's gate  1000:A5E8
 *
 * Deviations: D-251 (music tick on the sample clock), D-252 (a missing tune
 * is skipped, not fatal).
 */

#ifndef HUMANSRE_AUDIO_H
#define HUMANSRE_AUDIO_H

#include "osystem.h"

/* The game's tune base names (202F:002A, 0x77A, 0xF91, 0x11AA, 0x1391,
 * 0x139B and 0x62E3/0x62EC).  The address is the dedup key in the original,
 * so they are objects here, not string literals: `audio_tune_lev` is the
 * mutable 0x139B buffer whose letter at +7 is [0x13A2]. */
extern const char audio_tune_title[];
extern const char audio_tune_hiscore[];
extern const char audio_tune_disk[];
extern const char audio_tune_party[];
extern const char audio_tune_humfx[];
extern char audio_tune_lev[]; // "mus\levA"; the letter is rewritten

/**
 * @brief Audio module init (the backend is bound once by osys_bind()).
 *
 * @return 0 on success
 */
int32_t audio_init(void);

/**
 * @brief Install the resident driver (driver_reset 1000:84DE), once at boot.
 */
void audio_install(void);

/**
 * @brief The game tick's audio step (1000:8B1E + the fade pair).
 *
 * cmd 2 on 2 of every 3 ticks and one master-volume step every 2 ticks.
 * A backend that owns the audio clock (SDL3's render path) makes this a no-op
 * and calls audio_render_tick instead.
 */
void audio_tick(void);

/**
 * @brief One audio tick at the sample where it is due (arch/sdl3/audio.c).
 *
 * The register writes must land on that sample, not on the next render chunk's
 * edge: the original's ISR writes the OPL ports synchronously.
 */
void audio_render_tick(void);

/**
 * @brief Declare that the backend drives the audio tick from its sample clock,
 * so audio_tick (the game-tick call) must not step it too.
 *
 * @param on 1 while an audio stream is running and calls audio_render_tick
 */
void audio_set_render_clock(uint8_t on);

/**
 * @brief play_tune (1000:8437): load and start a tune image.
 *
 * The original appends the resident driver's extension (.BEP / .ALB / .RLD,
 * 202F:40BC/40C1/40C6, chosen by [0x5AFE] at 1000:8462) to the caller's base
 * name, INT 63h-loads it, then sends cmd 0x0C.  The dedup is by name
 * *pointer*, exactly as `[0x40B8]` compares: the first call for a name loads
 * it, every later call returns.
 *
 * When the file is missing the original exits with an error (D-252); the port
 * stops the current record and stays silent until a later tune loads.  That
 * is the forced-Beeper case: only `LEVA.BEP` of the lettered level tunes
 * ships.
 *
 * @param name the base name, e.g. "mus\\title"
 */
void audio_play_tune(const char *name);

/**
 * @brief The level's lettered tune (1000:326E / 0x139B+/0x13A2).
 *
 * level_status_update's lettered tune (1000:327F): "mus\levX" with X = 'A' +
 * (level1 - 1) % 18.
 *
 * @param level1 the 1-based level number ([0x1F41])
 */
void audio_play_level_tune(uint16_t level1);

/**
 * @brief music_resume (1000:83F7): start the loaded image's record 0.
 *
 * Returns when the latch `[0x40BB]` is already set or the MUSIC board is
 * off (`[0x5E5B]` non-zero).  Sends cmd 5 CX=0 then cmd 0x0A CL=0.
 */
void audio_music_resume(void);

/**
 * @brief music_stop (1000:8389): clear [0x40BB], cmd 6.
 */
void audio_music_stop(void);

/**
 * @brief The front-end frame web's music gate (1000:A5E8).
 *
 * `off` (the MUSIC board's counter [0x5E5B]) non-zero -> music_stop; zero ->
 * music_resume + cmd 0x0A 0x4B (1000:A5F7).  One call per room tick.
 *
 * @param off the MUSIC board's counter [0x5E5B]
 */
void audio_music_gate(uint32_t off);

/**
 * @brief timing_loop (1000:84A5): start the master-volume fade-in.
 *
 * Master volume 0..0x4A, two ticks per step, latched by [0x40BA].  The
 * original blocks for 75 x two vsyncs; here the steps run from audio_tick,
 * the same audio timeline, without stalling the foreground.
 */
void audio_fade_in(void);

/**
 * @brief present_frames_75 (1000:835A): start the master-volume fade-out.
 *
 * 0x4B..0, two ticks per step, then clears [0x40BA].
 */
void audio_fade_out(void);

/**
 * @brief True while a fade is still stepping.
 *
 * @return 1 while fading, 0 when idle
 */
uint8_t audio_fade_busy(void);

/**
 * @brief driver_call (1000:83B7): start a sound-effect record.
 *
 * cmd 5 CX=id, gated by [0x5E5B] (music off), the driver id (the port links
 * AdLib, id 1, so event 0x0E is dropped) and the 0xFF sentinel.
 *
 * @param id the caller's event id
 */
void audio_sfx(uint32_t id);

#endif /* HUMANSRE_AUDIO_H */
