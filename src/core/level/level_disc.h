/* draw_status_indicator (1000:1FEF): the insert-disc prompt.
 */

#ifndef HUMANSRE_LEVEL_DISC_H
#define HUMANSRE_LEVEL_DISC_H

#include "osystem.h"
#include <stdint.h>

/**
 * @brief Start the disc-presence indicator for one disc.
 *
 * draw_status_indicator (1000:1FEF): the disc probe + the "PUT THE DISK INTO
 * DRIVE n / PRESS FIRE WHEN READY" screen.
 * Begin returns 0 when the volume is present (the normal, no-screen path);
 * Tick returns 1 while the screen is up.
 *
 * @param disc the disc number
 * @return 1 when the prompt is up, 0 when the disc is present
 */
uint8_t level_disk_status_begin(uint16_t disc);

/**
 * @brief Tick the disc indicator; 1 = still waiting.
 *
 * @param in the input state
 * @return 1 while the prompt is still up
 */
uint8_t level_disk_status_tick(const InputState *in);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Clear the disc indicator's state (test/lifecycle: drop the screen).
 */
void level_disk_status_reset(void);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_LEVEL_DISC_H */
