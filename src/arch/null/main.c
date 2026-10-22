/* arch/null/main.c — the arch layer owns main() (port_structure.md §2). */
#include "osystem.h"

extern const OSystem osys_null;

int main(void)
{
    return game_main(&osys_null);
}
