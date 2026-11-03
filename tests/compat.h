/* compat.h — the host-OS shims for the test mains.
 *
 * The core stays pure C99 with no host #ifdefs (port_structure.md §5); only
 * these host-facing mains need the POSIX calls: chdir (fs opens its volumes
 * relative to the cwd) and realpath (the fixture paths resolve against the
 * caller's cwd before the chdir).  Windows spells them _chdir/_fullpath
 * under <direct.h>/<stdlib.h>, and its limits.h has no PATH_MAX. */
#ifndef HUMANSRE_TEST_COMPAT_H
#define HUMANSRE_TEST_COMPAT_H

#ifdef _WIN32
#include <direct.h>
#include <stdlib.h>
#ifndef PATH_MAX
#define PATH_MAX 260 /* the Windows MAX_PATH, _fullpath's own limit */
#endif
static inline int32_t compat_chdir(const char *dir) { return _chdir(dir); }
static inline int32_t compat_mkdir(const char *dir) { return _mkdir(dir); }
static inline int32_t compat_rmdir(const char *dir) { return _rmdir(dir); }
static inline char *compat_realpath(const char *path, char *resolved)
{
    return _fullpath(resolved, path, PATH_MAX);
}
#else
#include <limits.h>
#include <stdlib.h>      /* realpath */
#include <sys/stat.h>
#include <unistd.h>
static inline int32_t compat_chdir(const char *dir) { return chdir(dir); }
static inline int32_t compat_mkdir(const char *dir)
{
    return mkdir(dir, 0777);
}
static inline int32_t compat_rmdir(const char *dir) { return rmdir(dir); }
static inline char *compat_realpath(const char *path, char *resolved)
{
    return realpath(path, resolved);
}
#endif

#endif /* HUMANSRE_TEST_COMPAT_H */
