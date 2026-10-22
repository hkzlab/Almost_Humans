/* entity.c — the entity table and the movement
 * (game_structure.md §1 subsystem 10, re_formats.md §12): the table seeded
 * from the map bytes, the entity bytecode, the waypoint ping-pong, REX.DAT. */
#include "game.h"

int entity_init(const OSystem *osys) { (void)osys; return 0; }
void entity_tick(void) {}
