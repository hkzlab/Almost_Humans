/* arch/null/main.c — the arch layer owns main() (port_structure.md §2). */
#include "osystem.h"

extern const OSystem g_osys_null;

int main(void) { return game_main(&g_osys_null); }
