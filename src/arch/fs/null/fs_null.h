/* The all-fail filesystem module's one exported symbol.
 *
 * Kept for the day a target links it; no current target does, so nothing
 * includes this header yet.  Only an OSystem initializer would name it.
 */
#ifndef HUMANSRE_FS_NULL_H
#define HUMANSRE_FS_NULL_H

#include "fs/fs.h"

extern const FSModule g_fs_null;

#endif /* HUMANSRE_FS_NULL_H */
