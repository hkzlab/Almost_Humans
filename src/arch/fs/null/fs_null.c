#include "fs/null/fs_null.h"

#include <stdint.h>
#include <stdio.h>

/**
 * @brief Open always fails.
 *
 * @param disc the disc number
 * @return the pointer, or NULL when unavailable
 */
static FSVolume *_null_open_volume(uint16_t disc);

/**
 * @brief Nothing to close.
 *
 * @param v the volume handle
 */
static void _null_close_volume(FSVolume *v);

/**
 * @brief No size.
 *
 * @param v the volume handle
 * @return 0
 */
static uint32_t _null_volume_size(FSVolume *v);

/**
 * @brief Read fails.
 *
 * @param v the volume handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return 0
 */
static uint32_t _null_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n);

/**
 * @brief Open always fails.
 *
 * @param letter the slot letter
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_open_save(char letter);

/**
 * @brief Save read fails.
 *
 * @param s the save handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return 0
 */
static uint32_t _null_save_read(FSSave *s, uint32_t off, uint8_t *dst,
                                uint32_t n);

/**
 * @brief Creation always fails.
 *
 * @param letter the slot letter
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_create_save(char letter);

/**
 * @brief Save write fails.
 *
 * @param s the save handle
 * @param off the offset
 * @param src the source buffer
 * @param n the count
 * @return 0
 */
static uint32_t _null_save_write(FSSave *s, uint32_t off, const uint8_t *src,
                                 uint32_t n);

/**
 * @brief Nothing to close.
 *
 * @param s the save handle
 */
static void _null_close_save(FSSave *s);

/**
 * @brief Open always fails.
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_open_named(const char *name);

/**
 * @brief Creation always fails.
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_create_named(const char *name);

/**
 * @brief Open always fails.
 *
 * @param path the path
 * @return NULL
 */
static FSFile *_null_open_file(const char *path);

/**
 * @brief No size.
 *
 * @param f the asset handle
 * @return 0
 */
static uint32_t _null_file_size(FSFile *f);

/**
 * @brief Read fails.
 *
 * @param f the asset handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return 0
 */
static uint32_t _null_file_read(FSFile *f, uint32_t off, uint8_t *dst,
                                uint32_t n);

/**
 * @brief Nothing to close.
 *
 * @param f the asset handle
 */
static void _null_close_file(FSFile *f);

/**
 * @brief No directory.
 *
 * @param dir the directory
 * @param out the name buffer
 * @param cap out's capacity
 * @return -1
 */
static int32_t _null_list_dir(const char *dir, char *out, uint32_t cap);

const FSModule g_fs_null = {
    _null_open_volume, _null_close_volume, _null_volume_size,  _null_read,
    _null_open_save,   _null_save_read,    _null_create_save,  _null_save_write,
    _null_close_save,  _null_open_named,   _null_create_named, _null_open_file,
    _null_file_size,   _null_file_read,    _null_close_file,   _null_list_dir
};

static FSVolume *_null_open_volume(uint16_t disc)
{
    (void)disc;
    return NULL;
}

static void _null_close_volume(FSVolume *v) { (void)v; }

static uint32_t _null_volume_size(FSVolume *v)
{
    (void)v;
    return 0;
}

static uint32_t _null_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{
    (void)v;
    (void)off;
    (void)dst;
    (void)n;
    return 0;
}

static FSSave *_null_open_save(char letter)
{
    (void)letter;
    return NULL;
}

static uint32_t _null_save_read(FSSave *s, uint32_t off, uint8_t *dst,
                                uint32_t n)
{
    (void)s;
    (void)off;
    (void)dst;
    (void)n;
    return 0;
}

static FSSave *_null_create_save(char letter)
{
    (void)letter;
    return NULL;
}

static uint32_t _null_save_write(FSSave *s, uint32_t off, const uint8_t *src,
                                 uint32_t n)
{
    (void)s;
    (void)off;
    (void)src;
    (void)n;
    return 0;
}

static void _null_close_save(FSSave *s) { (void)s; }

static FSSave *_null_open_named(const char *name)
{
    (void)name;
    return NULL;
}

static FSSave *_null_create_named(const char *name)
{
    (void)name;
    return NULL;
}

static FSFile *_null_open_file(const char *path)
{
    (void)path;
    return NULL;
}

static uint32_t _null_file_size(FSFile *f)
{
    (void)f;
    return 0;
}

static uint32_t _null_file_read(FSFile *f, uint32_t off, uint8_t *dst,
                                uint32_t n)
{
    (void)f;
    (void)off;
    (void)dst;
    (void)n;
    return 0;
}

static void _null_close_file(FSFile *f) { (void)f; }

static int32_t _null_list_dir(const char *dir, char *out, uint32_t cap)
{
    (void)dir;
    (void)out;
    (void)cap;
    return -1;
}
