/* fs/fs.h — the common filesystem-module interface.
 *
 * The game core never touches the OS's files or types: it asks the selected
 * module for random-access reads over the volume files (HUMANS  .000/.001/.002)
 * and the save files, through opaque handles.  FILE and the host paths never
 * cross this header — the exotic platforms implement the module on their own
 * storage primitives.
 *
 * Contract:
 *   openVolume(disc)   opens HUMANS  .00N for the disc number (0-based);
 *                      NULL = the volume is not available.
 *   volumeRead(v, off, dst, n)  reads n bytes at the byte offset `off` of
 *                      the volume's data; returns the bytes read (< n = EOF
 *                      or the error); the caller owns the error handling.
 *   volumeSize(v)      the volume's size in bytes (0 = unknown/unavailable).
 *   closeVolume(v)     releases the handle; NULL is accepted.
 */
#ifndef HUMANSRE_FS_H
#define HUMANSRE_FS_H

#include <stdint.h>

typedef struct FSVolume FSVolume;

typedef struct FSModule {
    FSVolume *(*openVolume)(unsigned disc);
    void      (*closeVolume)(FSVolume *v);
    uint32_t  (*volumeSize)(FSVolume *v);
    uint32_t  (*read)(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n);
} FSModule;

/* The selected module, defined by exactly one fs_*.c that the build links. */
extern const FSModule fs;

#endif /* HUMANSRE_FS_H */
