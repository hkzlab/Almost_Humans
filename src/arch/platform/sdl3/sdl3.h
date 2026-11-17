/* The SDL3 backend's five modules: video, events, timer, audio, and the
 * OSystem instance that binds them (OSystem.c).
 *
 * One header for the five tightly related translation units, per the C
 * standard's shared-header rule: it declares only the OSystem entry points
 * OSystem.c binds; each module's own state and helpers stay static.
 */
#ifndef HUMANSRE_SDL3_H
#define HUMANSRE_SDL3_H

#include "osystem.h"

#include <stdint.h>

/**
 * @brief Set the mode, scaling the active field to fill 4:3.
 *
 * @param mode the mode
 */
void sdl3_video_set_mode(VideoMode mode);

/**
 * @brief Record the view scale the command line asked for (1 or 2).
 *
 * Called by main() before game_main; the texture is built from it.
 *
 * @param scale the scale
 */
void sdl3_video_set_scale(int32_t scale);

/**
 * @brief Accessor: the recorded view scale.
 *
 * @return the scale
 */
int32_t sdl3_video_scale(void);

/**
 * @brief Accessor: one of the two display pages.
 *
 * @param which the page selector
 * @return One of the two display pages.
 */
uint8_t *sdl3_video_page(int32_t which);

/**
 * @brief Present one page as a field of `rows` rows.
 *
 * @param which the page selector
 * @param rows the field's row count
 */
void sdl3_video_present(int32_t which, uint32_t rows);

/**
 * @brief Upload a 6-bit palette range to the display.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param n the count
 */
void sdl3_video_set_palette(const uint8_t *p6, uint32_t first, uint32_t n);

/**
 * @brief Clear the input state.
 */
void sdl3_events_init(void);

/**
 * @brief Pump SDL events into the input state.
 *
 * @param out the output buffer
 */
void sdl3_events_poll(InputState *out);

/**
 * @brief Start the fixed-tick timer.
 *
 * @param ticksPerSec the ticks per second
 * @param tick the tick callback
 */
void sdl3_timer_start(uint32_t ticksPerSec, TickFn tick);

/**
 * @brief Block until the next fixed tick has fired.
 *
 * @param lastSeen updated with the counter just run
 */
void sdl3_tick_sync(uint32_t *lastSeen);

/**
 * @brief Install the AdLib driver emulation and open the stream.
 *
 * @return 0 on success, -1 when the driver or the stream cannot be created
 */
int32_t sdl3_audio_install(void);

/**
 * @brief cmd 0x0C: hand the loaded tune image to the driver.
 *
 * @param image the .ALB image
 * @param size its byte length
 * @return 0, always
 */
int32_t sdl3_audio_load_tune(const uint8_t *image, uint32_t size);

/**
 * @brief cmd 5, CX = 0: start the loaded tune's record 0 (music_resume).
 */
void sdl3_audio_resume(void);

/**
 * @brief cmd 6: release the current record (music_stop).
 */
void sdl3_audio_stop(void);

/**
 * @brief cmd 5, CX = id: start a sound-effect record.
 *
 * The original's driver_call stores the new handle in [0x5AF0], overwriting
 * the music handle; that is reproduced here.
 *
 * @param id the event id
 */
void sdl3_audio_sfx(uint16_t id);

/**
 * @brief cmd 0x0A: the master-volume byte.
 *
 * @param cl the game's byte
 */
void sdl3_audio_tempo(uint8_t cl);

/**
 * @brief cmd 2: step the sequencer one tick.
 */
void sdl3_audio_tick(void);

/**
 * @brief Select the resident driver before the install ([0x5AFE]).
 *
 * @param name "adlib" (the default), "beeper" or "mt32"
 * @param roms the MT-32 ROM directory (required for "mt32")
 * @return 0 on success, -1 on an unknown name, -2 when mt32 has no ROM dir
 */
int32_t sdl3_audio_select(const char *name, const char *roms);

/**
 * @brief The backend's [0x5AFE] sound_driver_id (osystem.h).
 *
 * @return 0 for the Beeper, 1 for AdLib, 3 for the MT-32; -1 before a
 * selection is made
 */
int32_t sdl3_audio_driver_id(void);

#endif /* HUMANSRE_SDL3_H */
