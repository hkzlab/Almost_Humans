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
 *
 *   openSave(letter)   opens the save file for slot letter 'A'..'T' — the
 *                      game's `'A' + (n-1)` law (re_formats.md §10/§10.0).
 *                      The module owns the filename/path; the POSIX module
 *                      uses SAVEA..SAVET in the game directory (the
 *                      original's `\HUMANS\CDROM\saveX`).  NULL = absent;
 *                      the caller owns the error (the original's JC path).
 *   saveRead(s, off, dst, n)  the same read contract as volumeRead.
 *   createSave(letter) creates/truncates the same file for writing (the
 *                      save writer); NULL = cannot create.
 *   saveWrite(s, off, src, n) writes n bytes at `off`; returns the bytes
 *                      written (< n = the error).
 *   closeSave(s)       releases the handle; NULL is accepted.
 *
 *   openNamed(name)    opens a runtime file of the game directory for reading
 *                      — the original's non-disc files (`HISCORES.HUM`,
 *                      re_formats.md §11).  NULL = absent.
 *   createNamed(name)  creates/truncates it for writing.
 *
 * The modules never abort.  Every byte the game I/O needs goes through here —
 * the core (and its tests' game-side code) never includes a stdio path.
 */
#ifndef HUMANSRE_FS_H
#define HUMANSRE_FS_H

#include <stdint.h>

typedef struct FSVolume FSVolume;
typedef struct FSSave FSSave;

typedef struct FSModule {
    FSVolume *(*openVolume)(uint32_t disc);
    void (*closeVolume)(FSVolume *v);
    uint32_t (*volumeSize)(FSVolume *v);
    uint32_t (*read)(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n);
    FSSave *(*openSave)(char letter);
    uint32_t (*saveRead)(FSSave *s, uint32_t off, uint8_t *dst, uint32_t n);
    FSSave *(*createSave)(char letter);
    uint32_t (*saveWrite)(FSSave *s, uint32_t off, const uint8_t *src,
                          uint32_t n);
    void (*closeSave)(FSSave *s);
    FSSave *(*openNamed)(const char *name);
    FSSave *(*createNamed)(const char *name);
} FSModule;

/* The one module the build links; the arch layer puts it in OSystem::fs and
 * the core never names it. */
extern const FSModule g_fs_posix;
extern const FSModule g_fs_null;

#endif /* HUMANSRE_FS_H */
