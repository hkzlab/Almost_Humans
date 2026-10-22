/* script.c — the interpreter skeleton (re_scripts.md §2): one cursor +
 * dispatch-table + per-tick machine, shared by the entity bytecode, the
 * music-event scripts and the frame-keyed cue lists. */
#include "game.h"

int script_init(const OSystem *osys) { (void)osys; return 0; }
