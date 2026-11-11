/* The POSIX filesystem module's one exported symbol.
 *
 * Only the code that builds an OSystem names the module: the arch backends'
 * initializers and the tests that hand-build one.  The core reaches the module
 * through OSystem::fs and never includes this header.
 * Deviation: D-05 (POSIX files, no DOS drive or disc swap).
 */
#ifndef HUMANSRE_FS_POSIX_H
#define HUMANSRE_FS_POSIX_H

#include "fs/fs.h"

extern const FSModule g_fs_posix;

#endif /* HUMANSRE_FS_POSIX_H */
