/* The headless entry: run the core with the null backend, --ticks N to stop.
 *
 * Port-only: there is no input source, so only --ticks ends the loop.
 */
#include "game_main.h"
#include "null/osys_null.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief The headless executable: run the core with the null backend.
 *
 * @param argc the argument count
 * @param argv the argument vector; --ticks N stops after N flow ticks,
 *             --version prints the version and exits
 * @return 0 on success, 2 on a bad command line
 */
int main(int argc, char **argv)
{
    uint32_t ticks = 0;
    int32_t i;

    for (i = 1; i < argc; i++) {
        char *end;
        unsigned long n; // strtoul's own type

        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s\n", HUMANSRE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--ticks") != 0 || i + 1 >= argc) {
            fprintf(stderr, "USAGE: %s [--ticks N] [--version]\n", argv[0]);
            return 2;
        }
        n = strtoul(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0') {
            fprintf(stderr, "--ticks needs a number\n");
            return 2;
        }
        ticks = (uint32_t)n;
    }
    printf("%s %s\n", HUMANSRE_PROJECT_TITLE, HUMANSRE_VERSION);
    osys_null_set_tick_limit(ticks);
    return game_main(&g_osys_null);
}
