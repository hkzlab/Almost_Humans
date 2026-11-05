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
#    include <direct.h>
#    include <stdlib.h>
#    ifndef PATH_MAX
#        define PATH_MAX 260 /* the Windows MAX_PATH, _fullpath's own limit */
#    endif
/**
 * @brief Change the process directory.
 *
 * @param dir the target directory
 * @return 0 on success, -1 on failure
 */
static inline int32_t compat_chdir(const char *dir) { return _chdir(dir); }
/**
 * @brief Create a directory.
 *
 * @param dir the directory to create
 * @return 0 on success, -1 on failure
 */
static inline int32_t compat_mkdir(const char *dir) { return _mkdir(dir); }
/**
 * @brief Remove a directory.
 *
 * @param dir the directory to remove
 * @return 0 on success, -1 on failure
 */
static inline int32_t compat_rmdir(const char *dir) { return _rmdir(dir); }
/**
 * @brief Resolve an absolute path.
 *
 * @param path the path to resolve
 * @param resolved the output buffer
 * @return resolved, or NULL on failure
 */
static inline char *compat_realpath(const char *path, char *resolved)
{ return _fullpath(resolved, path, PATH_MAX); }
#else
#    include <limits.h>
#    include <stdlib.h>      /* realpath */
#    include <sys/stat.h>
#    include <unistd.h>
/**
 * @brief Change the process directory.
 *
 * @param dir the target directory
 * @return 0 on success, -1 on failure
 */
static inline int32_t compat_chdir(const char *dir) { return chdir(dir); }
/**
 * @brief Create a directory.
 *
 * @param dir the directory to create
 * @return 0 on success, -1 on failure
 */
static inline int32_t compat_mkdir(const char *dir) { return mkdir(dir, 0777); }
/**
 * @brief Remove a directory.
 *
 * @param dir the directory to remove
 * @return 0 on success, -1 on failure
 */
static inline int32_t compat_rmdir(const char *dir) { return rmdir(dir); }
/**
 * @brief Resolve an absolute path.
 *
 * @param path the path to resolve
 * @param resolved the output buffer
 * @return resolved, or NULL on failure
 */
static inline char *compat_realpath(const char *path, char *resolved)
{ return realpath(path, resolved); }
#endif

#endif /* HUMANSRE_TEST_COMPAT_H */
