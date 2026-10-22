/* arch/sdl3/fs.c — the game-dir/save-dir paths (port_structure.md §2). */
#include "osystem.h"
#include <stdio.h>

FILE *sdl3_openGameFile(const char *name) { return fopen(name, "rb"); }
FILE *sdl3_openSaveFile(unsigned slot, const char *mode)
{
    char n[64];
    snprintf(n, sizeof n, "save%u.sav", slot);
    return fopen(n, mode);
}
