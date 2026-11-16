/* The behaviour walk: the per-behaviour movers, the projectile and the
 * stack-8 states.
 */

#ifndef HUMANSRE_OBJECT_WALK_H
#define HUMANSRE_OBJECT_WALK_H

#include <stdint.h>


/**
 * @brief The placed-object update list.
 *
 * 1000:C295 update_list: the placed records' behaviour walk (torch, stepper,
 * sequence, wanderer).
 */
void object_walk_update_list(void);

#endif /* HUMANSRE_OBJECT_WALK_H */
