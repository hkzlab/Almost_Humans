/* The application entry the arch layer calls: boot, then the foreground flow
 * loop.
 */
#ifndef HUMANSRE_GAME_MAIN_H
#define HUMANSRE_GAME_MAIN_H

#include "osystem.h"

/**
 * @brief The application entry: boot, then the foreground flow loop.
 *
 * Implemented by the core; the arch layer's main() picks a backend and hands
 * it here.
 *
 * @param osys the backend interface
 * @return 0 when the quit request ends the loop
 */
int32_t game_main(const OSystem *osys);

#endif /* HUMANSRE_GAME_MAIN_H */
