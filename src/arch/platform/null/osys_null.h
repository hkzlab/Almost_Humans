/* The null backend's test surface: the OSystem instance and the accessors the
 * headless tests read their evidence from.  Linked into humans-null and the
 * core_test executables only.
 */
#ifndef HUMANSRE_OSYS_NULL_H
#define HUMANSRE_OSYS_NULL_H

#include "osystem.h"

#include <stdint.h>

/* The no-video/no-audio OSystem the tests bind through game_main(). */
extern const OSystem g_osys_null;

/**
 * @brief Stop the loop after a tick count (the headless executable's --ticks).
 *
 * @param n the limit in ticks; 0 runs until the input asks to quit
 */
extern void osys_null_set_tick_limit(uint32_t n);

/**
 * @brief Drive N ticks by hand, for the content tests.
 *
 * @param n the tick count
 */
extern void osys_null_run_ticks(uint32_t n);

/**
 * @brief Inject the input state the next poll returns.
 *
 * @param in the input state
 */
extern void osys_null_inject(const InputState *in);

/**
 * @brief Accessor: the music-resume (cmd 5, CX = 0) count.
 *
 * @return the audio-start count
 */
extern uint32_t osys_null_audio_starts(void);

/**
 * @brief Accessor: the music-stop (cmd 6) count.
 *
 * @return the audio-stop count
 */
extern uint32_t osys_null_audio_stops(void);

/**
 * @brief Accessor: the install count.
 *
 * @return the install count
 */
extern uint32_t osys_null_audio_installs(void);

/**
 * @brief Accessor: the tune-load count.
 *
 * @return the tune-load count
 */
extern uint32_t osys_null_audio_loads(void);

/**
 * @brief Accessor: the last loaded tune's byte size.
 *
 * @return the size
 */
extern uint32_t osys_null_audio_tune_size(void);

/**
 * @brief Accessor: the byte size of the i-th tune load.
 *
 * @param i the load index
 * @return the size, 0 when out of range
 */
extern uint32_t osys_null_audio_load_size(uint32_t i);

/**
 * @brief Accessor: the sound-effect command count.
 *
 * @return the count
 */
extern uint32_t osys_null_audio_sfx_count(void);

/**
 * @brief Accessor: the last sound-effect event id.
 *
 * @return the event id, 0xFFFF when none
 */
extern uint16_t osys_null_audio_last_sfx(void);

/**
 * @brief Accessor: the last master-volume byte (cmd 0x0A).
 *
 * @return the byte
 */
extern uint8_t osys_null_audio_tempo(void);

/**
 * @brief Accessor: the music-tick count (cmd 2).
 *
 * @return the count
 */
extern uint32_t osys_null_audio_ticks(void);

/**
 * @brief Accessor: the last video mode set.
 *
 * @return the last video mode set
 */
extern VideoMode osys_null_video_mode(void);

/**
 * @brief Accessor: the eventsPoll count, the smoke test's frame-dump clock.
 *
 * @return the eventsPoll call count
 */
extern uint32_t osys_null_poll_count(void);

/**
 * @brief Install the present hook.
 *
 * @param fn the callback, called with the poll count
 */
extern void osys_null_on_present(void (*fn)(uint32_t poll));

/**
 * @brief Install a scripted input sequence.
 *
 * @param states the input sequence
 * @param n the count
 */
extern void osys_null_input_script(const InputState *states, uint32_t n);

/**
 * @brief Accessor: the i-th null video page.
 *
 * @param which the page selector
 * @return the page, masked to 0 or 1
 */
extern const uint8_t *osys_null_page(int32_t which);

/**
 * @brief Accessor: the last palette written.
 *
 * @return the last palette written
 */
extern const uint8_t *osys_null_palette(void);

#endif /* HUMANSRE_OSYS_NULL_H */
