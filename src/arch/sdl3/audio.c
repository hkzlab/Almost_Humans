/* arch/sdl3/audio.c — a stub for now: the OPL-emulation-vs-driver-port
 * decision is M5's (port_structure.md §7).  The interface is stable either
 * way, so the core never changes. */
#include "osystem.h"
#include <SDL3/SDL.h>

int sdl3_audioStart(void) { SDL_Log("audio: stub (M5)"); return 0; }
int sdl3_audioPlayTune(unsigned id) { (void)id; return 0; }
void sdl3_audioStop(void) {}
