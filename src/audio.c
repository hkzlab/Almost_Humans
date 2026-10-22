/* audio.c — the tune/sfx state machine (game_structure.md §1 subsystem 7)
 * over osys->audio*; the INT 60h ABI is re_map.md's audio section. */
#include "game.h"

int audio_init(const OSystem *osys) { (void)osys; return 0; }
