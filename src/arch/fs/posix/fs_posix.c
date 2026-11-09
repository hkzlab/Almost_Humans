/* fs/posix/fs_posix.c — the POSIX filesystem module: the byte reader over
 * the volume files (HUMANS  .00N) and the save files (SAVEA..SAVET in the
 * game directory).  The volume's internal format (the chunk directory, the
 * LZW payloads) and the save format are the core's business (re_formats.md
 * §8/§10); the module only opens, sizes and reads bytes through stdio. */
#include "fs/fs.h"

#include <stdio.h>

/**
 * @brief Open the volume directory for one disc.
 *
 * @param disc the disc number
 * @return the pointer, or NULL when unavailable
 */
static FSVolume *_posix_open_volume(uint32_t disc);

/**
 * @brief Close a volume stream.
 *
 * @param v the volume handle
 */
static void _posix_close_volume(FSVolume *v);

/**
 * @brief The volume's byte size.
 *
 * @param v the volume handle
 * @return the volume's byte size
 */
static uint32_t _posix_volume_size(FSVolume *v);

/**
 * @brief Read n bytes from a volume at an absolute offset.
 *
 * @param v the volume handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return the bytes read
 */
static uint32_t _posix_read(FSVolume *v, uint32_t off, uint8_t *dst,
                            uint32_t n);

/** @brief The save files: the original's `\HUMANS\CDROM\saveX` mapped onto the game
 * directory as SAVEA..SAVET (the slot letter is the game's law, the
 * filename is this module's business).
 *
 * @param letter the slot letter
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_posix_open_save(char letter);

/**
 * @brief Read n bytes from a save.
 *
 * @param s the save handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return the bytes read
 */
static uint32_t _posix_save_read(FSSave *s, uint32_t off, uint8_t *dst,
                                 uint32_t n);

/**
 * @brief Create the save file for one slot letter.
 *
 * @param letter the slot letter
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_posix_create_save(char letter);

/**
 * @brief Write n bytes to a save.
 *
 * @param s the save handle
 * @param off the offset
 * @param src the source buffer
 * @param n the count
 * @return the bytes written
 */
static uint32_t _posix_save_write(FSSave *s, uint32_t off, const uint8_t *src,
                                  uint32_t n);

/**
 * @brief Close a save stream.
 *
 * @param s the save handle
 */
static void _posix_close_save(FSSave *s);

/**
 * @brief the game-directory runtime files (HISCORES.HUM): the name is the game's
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_posix_open_named(const char *name);

/**
 * @brief Create a named save file.
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_posix_create_named(const char *name);

const FSModule g_fs_posix = { _posix_open_volume, _posix_close_volume,
                              _posix_volume_size, _posix_read,
                              _posix_open_save,   _posix_save_read,
                              _posix_create_save, _posix_save_write,
                              _posix_close_save,  _posix_open_named,
                              _posix_create_named };

static FSVolume *_posix_open_volume(uint32_t disc)
{
    char name[32];
    snprintf(name, sizeof name, "HUMANS.%03u", disc % 1000u);
    return (FSVolume *)fopen(name, "rb");
}

static void _posix_close_volume(FSVolume *v)
{
    if (v) fclose((FILE *)v);
}

static uint32_t _posix_volume_size(FSVolume *v)
{
    if (fseek((FILE *)v, 0, SEEK_END)) return 0;
    return (uint32_t)ftell((FILE *)v);
}

static uint32_t _posix_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{
    /* long is fseek's parameter type, not a host-dependent integer here. */
    if (fseek((FILE *)v, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, n, (FILE *)v);
}

static FSSave *_posix_open_save(char letter)
{
    char name[16];
    if (letter < 'A' || letter > 'T') return NULL;
    snprintf(name, sizeof name, "SAVE%c", letter);
    return (FSSave *)fopen(name, "rb");
}

static uint32_t _posix_save_read(FSSave *s, uint32_t off, uint8_t *dst,
                                 uint32_t n)
{
    if (fseek((FILE *)s, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, n, (FILE *)s);
}

static FSSave *_posix_create_save(char letter)
{
    char name[16];
    if (letter < 'A' || letter > 'T') return NULL;
    snprintf(name, sizeof name, "SAVE%c", letter);
    return (FSSave *)fopen(name, "wb");
}

static uint32_t _posix_save_write(FSSave *s, uint32_t off, const uint8_t *src,
                                  uint32_t n)
{
    if (fseek((FILE *)s, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fwrite(src, 1, n, (FILE *)s);
}

static void _posix_close_save(FSSave *s)
{
    if (s) fclose((FILE *)s);
}

static FSSave *_posix_open_named(const char *name)
{ return (FSSave *)fopen(name, "rb"); }

static FSSave *_posix_create_named(const char *name)
{ return (FSSave *)fopen(name, "wb"); }
