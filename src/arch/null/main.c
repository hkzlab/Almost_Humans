/* arch/null/main.c — the arch layer owns main() (port_structure.md §2). */
#include "osystem.h"

#include <stdio.h>

extern const OSystem g_osys_null;

/**
 * @brief The headless executable: run the core with the null backend.
 *
 * @return 0 on success, 1 on failure
 */
int main(void)
{
    fprintf(stderr, "HumansRE %s\n", HUMANSRE_VERSION);
    return game_main(&g_osys_null);
}
