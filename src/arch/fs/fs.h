/* The filesystem module contract: the game core's only file I/O.
 *
 *   volume   openVolume / volumeRead / volumeSize / closeVolume
 *   save     openSave / saveRead / createSave / saveWrite / closeSave
 *   named    openNamed / createNamed        (HISCORES.HUM)
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
} FSModule;

/* Each module (fs/<name>/fs_<name>.c) exports its own `g_fs_<name>` and
 * declares it in fs/<name>/fs_<name>.h; only the code that builds an OSystem
 * includes one. */

#endif /* HUMANSRE_FS_H */
