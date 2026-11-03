/* fs/posix/fs_posix.c — the POSIX filesystem module: the byte reader over
 * the volume files (HUMANS  .00N) and the save files (SAVEA..SAVET in the
 * game directory).  The volume's internal format (the chunk directory, the
 * LZW payloads) and the save format are the core's business (re_formats.md
 * §8/§10); the module only opens, sizes and reads bytes through stdio. */
#include "fs/fs.h"

#include <stdio.h>

static FSVolume *posix_openVolume(uint32_t disc);
static void posix_closeVolume(FSVolume *v);
static uint32_t posix_volumeSize(FSVolume *v);
static uint32_t posix_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n);
static FSSave *posix_openSave(char letter);
static uint32_t posix_saveRead(FSSave *s, uint32_t off, uint8_t *dst,
                               uint32_t n);
static FSSave *posix_createSave(char letter);
static uint32_t posix_saveWrite(FSSave *s, uint32_t off, const uint8_t *src,
                                uint32_t n);
static void posix_closeSave(FSSave *s);
static FSSave *posix_openNamed(const char *name);
static FSSave *posix_createNamed(const char *name);

const FSModule fs = { posix_openVolume, posix_closeVolume, posix_volumeSize,
                      posix_read,       posix_openSave,    posix_saveRead,
                      posix_createSave, posix_saveWrite,   posix_closeSave,
                      posix_openNamed,  posix_createNamed };

static FSVolume *posix_openVolume(uint32_t disc)
{
    char name[32];
    snprintf(name, sizeof name, "HUMANS.%03u", disc % 1000u);
    return (FSVolume *)fopen(name, "rb");
}

static void posix_closeVolume(FSVolume *v)
{
    if (v) fclose((FILE *)v);
}

static uint32_t posix_volumeSize(FSVolume *v)
{
    if (fseek((FILE *)v, 0, SEEK_END)) return 0;
    return (uint32_t)ftell((FILE *)v);
}

static uint32_t posix_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{
    /* long is fseek's parameter type, not a host-dependent integer here. */
    if (fseek((FILE *)v, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, n, (FILE *)v);
}

/* The save files: the original's `\HUMANS\CDROM\saveX` mapped onto the game
 * directory as SAVEA..SAVET (the slot letter is the game's law, the
 * filename is this module's business). */
static FSSave *posix_openSave(char letter)
{
    char name[16];
    if (letter < 'A' || letter > 'T') return NULL;
    snprintf(name, sizeof name, "SAVE%c", letter);
    return (FSSave *)fopen(name, "rb");
}

static uint32_t posix_saveRead(FSSave *s, uint32_t off, uint8_t *dst,
                               uint32_t n)
{
    if (fseek((FILE *)s, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, n, (FILE *)s);
}

static FSSave *posix_createSave(char letter)
{
    char name[16];
    if (letter < 'A' || letter > 'T') return NULL;
    snprintf(name, sizeof name, "SAVE%c", letter);
    return (FSSave *)fopen(name, "wb");
}

static uint32_t posix_saveWrite(FSSave *s, uint32_t off, const uint8_t *src,
                                uint32_t n)
{
    if (fseek((FILE *)s, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fwrite(src, 1, n, (FILE *)s);
}

static void posix_closeSave(FSSave *s)
{
    if (s) fclose((FILE *)s);
}

/* the game-directory runtime files (HISCORES.HUM): the name is the game's */
static FSSave *posix_openNamed(const char *name)
{ return (FSSave *)fopen(name, "rb"); }

static FSSave *posix_createNamed(const char *name)
{ return (FSSave *)fopen(name, "wb"); }
