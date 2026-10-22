/* res.c — resource I/O (game_structure.md §1 subsystem 2, re_formats.md §8).
 * The volume file (named chunks [tag][payload]), the 16K-block flag words,
 * the LZW decoder, the offset-based pool arena.
 * Reference decoder: tools/ani_lzw.py (byte-for-byte validated). */
#include "game.h"

int res_init(const OSystem *osys) { (void)osys; return 0; }
