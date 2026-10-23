/* fs/posix/fs_posix.c — the POSIX filesystem module: the byte reader over
 * the volume files (HUMANS  .00N).  The volume's internal format (the chunk
 * directory, the LZW payloads) is the core's business (re_formats.md §8); the
 * module only opens, sizes and reads bytes through standard stdio. */
#include "fs/fs.h"
#include <stdio.h>
#include <stdio.h>

static FSVolume *posix_openVolume(unsigned disc)
{
    char name[32];
    snprintf(name, sizeof name, "HUMANS  .%03u", disc % 1000u);
    return (FSVolume *)fopen(name, "rb");
}

static void posix_closeVolume(FSVolume *v) { if (v) fclose((FILE *)v); }

static uint32_t posix_volumeSize(FSVolume *v)
{
    if (fseek((FILE *)v, 0, SEEK_END)) return 0;
    return (uint32_t)ftell((FILE *)v);
}

static uint32_t posix_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{
    if (fseek((FILE *)v, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, n, (FILE *)v);
}

const FSModule fs = { posix_openVolume, posix_closeVolume, posix_volumeSize, posix_read };
