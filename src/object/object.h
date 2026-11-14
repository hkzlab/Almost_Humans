/* The placed-object array (0x3E75) and its machines.
 *
 *   object_core_test_dispatch   1000:35D6   the goal predicates
 *   object_sequence_claim_begin     1000:3A6A   the adjacent-object op
 *   script_op_dispatch     1000:3979   the ACTION program
 *   object_command_run     1000:E3F6   the acting record's command
 *   object_walk_update_list     1000:C295   the behaviour walk
 *   collision_scan         1000:D860   the movers' last pass
 *
 * Deviations: D-101/D-137/D-177..D-182 (bounded reads, malformed-save edge
 * only), D-227 (TER attach: original bug fixed), D-270 (map edges: air, hard
 * sides and a bottom floor), D-271 (the unreferenced class-0 write).
 *
 * The declarations are split into one header per theme; this header re-exports
 * them for the rest of the port.
 */

#ifndef HUMANSRE_OBJECT_H
#define HUMANSRE_OBJECT_H

#include "object_core.h" // IWYU pragma: export
#include "object_render.h" // IWYU pragma: export
#include "object_spawn.h" // IWYU pragma: export
#include "object_sequence.h" // IWYU pragma: export
#include "object_command.h" // IWYU pragma: export
#include "object_walk.h" // IWYU pragma: export
#include "object_units.h" // IWYU pragma: export

#endif /* HUMANSRE_OBJECT_H */
