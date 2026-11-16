/* The POSIX filesystem module: stdio over the extracted volume and save files.
 *
 * HUMANS.00N and SAVEA..SAVET in the game directory.  Format knowledge stays
 * in the core; this module only opens, sizes, reads and writes bytes.  Only
 * the code that builds an OSystem names the module - the arch backends'
 * initializers and the tests that hand-build one; the core reaches it through
 * OSystem::fs and never includes this header.
 * Deviation: D-05 (POSIX files, no DOS drive or disc swap).
 */
#ifndef HUMANSRE_FS_POSIX_H
#define HUMANSRE_FS_POSIX_H

#include "fs/fs.h"

extern const FSModule g_fs_posix;

#endif /* HUMANSRE_FS_POSIX_H */
