/* The all-fail filesystem module: every open returns NULL.
 *
 * Port-only reference for a no-data build; no current target links it.
 * Callers treat NULL as "the volume is not available" and own the error path.
 * Only an OSystem initializer would name the module.
 */
#ifndef HUMANSRE_FS_NULL_H
#define HUMANSRE_FS_NULL_H

#include "fs/fs.h"

extern const FSModule g_fs_null;

#endif /* HUMANSRE_FS_NULL_H */
