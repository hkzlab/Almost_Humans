/* The filesystem module contract: the game core's only file I/O.
 *
 *   volume   openVolume / volumeRead / volumeSize / closeVolume
 *   save     openSave / saveRead / createSave / saveWrite / closeSave
 *   named    openNamed / createNamed        (HISCORES.HUM)
 *   assets   openFile / fileSize / fileRead / closeFile / listDir
 *
 * `assets` is the port's own file discovery, not a DOS service: the MT-32 ROM
 * images live in an arbitrary directory named on the command line, so the
 * driver asks the module that owns the host's paths instead of calling stdio.
 *
 * The module owns the paths and stdio; the core owns the formats and the error
 * path.  A read returns the bytes read (< n = EOF or error), NULL = absent.
 * Replaces DOS INT 21h / INT 63h.
 * Deviation: D-05 (POSIX files, no DOS drive or disc swap).
 */
#ifndef HUMANSRE_FS_H
#define HUMANSRE_FS_H

#include <stdint.h>

typedef struct FSVolume FSVolume;
typedef struct FSSave FSSave;
typedef struct FSFile FSFile;

/* The asset directory listing's buffer: NUL-separated file names, the count
 * returned by listDir.  A ROM set is a handful of files, so one page is
 * plenty; names past the buffer are dropped. */
#define FS_DIR_LIST_BYTES 4096u

typedef struct FSModule {
    FSVolume *(*openVolume)(uint16_t disc);
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

    /* The asset files: an arbitrary path, read whole or in part. */
    FSFile *(*openFile)(const char *path);
    uint32_t (*fileSize)(FSFile *f);
    uint32_t (*fileRead)(FSFile *f, uint32_t off, uint8_t *dst, uint32_t n);
    void (*closeFile)(FSFile *f);
    /* A directory's entries, NUL-separated in `out` (the FS_DIR_LIST_BYTES
     * contract).  Returns the name count, -1 unreadable.  The caller reads
     * each one and skips the ones that fail, a subdirectory included. */
    int32_t (*listDir)(const char *dir, char *out, uint32_t cap);
} FSModule;

/* Each module (fs/<name>/fs_<name>.c) exports its own `g_fs_<name>` and
 * declares it in fs/<name>/fs_<name>.h; only the code that builds an OSystem
 * includes one. */

#endif /* HUMANSRE_FS_H */
