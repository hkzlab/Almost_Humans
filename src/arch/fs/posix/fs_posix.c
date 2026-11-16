#include "fs/posix/fs_posix.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Open the volume directory for one disc.
 *
 * @param disc the disc number
 * @return the pointer, or NULL when unavailable
 */
static FSVolume *_posix_open_volume(uint16_t disc);

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
 * @return the size in bytes
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
 * @brief Open a game-directory runtime file (HISCORES.HUM).
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

/**
 * @brief Open an asset file at an arbitrary path.
 *
 * @param path the path
 * @return the handle, or NULL when unavailable
 */
static FSFile *_posix_open_file(const char *path);

/**
 * @brief The asset file's byte size.
 *
 * @param f the asset handle
 * @return the size in bytes, 0 when unavailable
 */
static uint32_t _posix_file_size(FSFile *f);

/**
 * @brief Read n bytes from an asset file at an absolute offset.
 *
 * @param f the asset handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return the bytes read
 */
static uint32_t _posix_file_read(FSFile *f, uint32_t off, uint8_t *dst,
                                 uint32_t n);

/**
 * @brief Close an asset file.
 *
 * @param f the asset handle
 */
static void _posix_close_file(FSFile *f);

/**
 * @brief List a directory's files as NUL-separated names.
 *
 * `readdir` order; the dot entries are skipped.  A name that does not fit the
 * buffer ends the listing.
 *
 * @param dir the directory
 * @param out the name buffer
 * @param cap out's capacity
 * @return the name count, -1 when the directory cannot be read
 */
static int32_t _posix_list_dir(const char *dir, char *out, uint32_t cap);

const FSModule g_fs_posix = { _posix_open_volume,  _posix_close_volume,
                              _posix_volume_size,  _posix_read,
                              _posix_open_save,    _posix_save_read,
                              _posix_create_save,  _posix_save_write,
                              _posix_close_save,   _posix_open_named,
                              _posix_create_named, _posix_open_file,
                              _posix_file_size,    _posix_file_read,
                              _posix_close_file,   _posix_list_dir };

static FSVolume *_posix_open_volume(uint16_t disc)
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

static FSFile *_posix_open_file(const char *path)
{ return (FSFile *)fopen(path, "rb"); }

static uint32_t _posix_file_size(FSFile *f)
{
    if (fseek((FILE *)f, 0, SEEK_END)) return 0;
    return (uint32_t)ftell((FILE *)f);
}

static uint32_t _posix_file_read(FSFile *f, uint32_t off, uint8_t *dst,
                                 uint32_t n)
{
    if (fseek((FILE *)f, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, n, (FILE *)f);
}

static void _posix_close_file(FSFile *f)
{
    if (f) fclose((FILE *)f);
}

static int32_t _posix_list_dir(const char *dir, char *out, uint32_t cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    uint32_t used = 0;
    int32_t n = 0;

    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        uint32_t len = (uint32_t)strlen(e->d_name) + 1u;

        if (e->d_name[0] == '.') continue;
        if (used + len > cap) break;
        memcpy(out + used, e->d_name, len);
        used += len;
        n++;
    }
    closedir(d);
    return n;
}
