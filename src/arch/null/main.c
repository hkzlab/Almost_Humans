/* arch/null/main.c - the arch layer owns main() (port_structure.md §2). */
#include "osystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const OSystem g_osys_null;

/**
 * @brief Stop the loop after a tick count (the headless executable's --ticks).
 *
 * @param n the limit in ticks; 0 runs until the input asks to quit
 */
extern void osys_null_set_tick_limit(uint32_t n);

/**
 * @brief The headless executable: run the core with the null backend.
 *
 * There is no input source, so the loop only ends at --ticks.
 *
 * @param argc the argument count
 * @param argv the argument vector; --ticks N stops after N flow ticks
 * @return 0 on success, 2 on a bad command line
 */
int main(int argc, char **argv)
{
    uint32_t ticks = 0;
    int i;

    fprintf(stderr, "%s %s\n", HUMANSRE_PROJECT_TITLE, HUMANSRE_VERSION);
    for (i = 1; i < argc; i++) {
        char *end;
        unsigned long n;

        if (strcmp(argv[i], "--ticks") != 0 || i + 1 >= argc) {
            fprintf(stderr, "USAGE: %s [--ticks N]\n", argv[0]);
            return 2;
        }
        n = strtoul(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0') {
            fprintf(stderr, "--ticks needs a number\n");
            return 2;
        }
        ticks = (uint32_t)n;
    }
    osys_null_set_tick_limit(ticks);
    return game_main(&g_osys_null);
}
