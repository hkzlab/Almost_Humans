/* arch/sdl3/audio.c — a stub for now: the OPL-emulation-vs-driver-port
 * decision is M5's (port_structure.md §7).  The interface is stable either
 * way, so the core never changes. */
#include "osystem.h"

#include <SDL3/SDL.h>

/** @brief Start the audio device. */
int32_t sdl3_audio_start(void)
{
    SDL_Log("audio: stub (M5)");
    return 0;
}
/** @brief Start one of the game's tunes. */
int32_t sdl3_audio_play_tune(uint32_t id)
{
    (void)id;
    return 0;
}
/** @brief Stop the audio device (no mixer yet). */
void sdl3_audio_stop(void) {}
