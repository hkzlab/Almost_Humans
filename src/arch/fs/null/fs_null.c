/* The all-fail filesystem module: every open returns NULL.
 *
 * Port-only reference for a no-data build; no current target links it.
 * Callers treat NULL as "the volume is not available" and own the error path.
 */
#include "fs/fs.h"

#include <stdio.h>

/**
 * @brief The null filesystem: open always fails.
 *
 * @param disc the disc number
 * @return the pointer, or NULL when unavailable
 */
static FSVolume *_null_open_volume(uint16_t disc);

/**
 * @brief The null filesystem: nothing to close.
 *
 * @param v the volume handle
 */
static void _null_close_volume(FSVolume *v);

/**
 * @brief The null filesystem: no size.
 *
 * @param v the volume handle
 * @return 0, always (no size)
 */
static uint32_t _null_volume_size(FSVolume *v);

/**
 * @brief The null filesystem: read fails.
 *
 * @param v the volume handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return 0, always (read fails)
 */
static uint32_t _null_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n);

/**
 * @brief The null filesystem: open always fails.
 *
 * @param letter the slot letter
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_open_save(char letter);

/**
 * @brief The null filesystem: save read fails.
 *
 * @param s the save handle
 * @param off the offset
 * @param dst the destination buffer
 * @param n the count
 * @return 0, always (read fails)
 */
static uint32_t _null_save_read(FSSave *s, uint32_t off, uint8_t *dst,
                                uint32_t n);

/**
 * @brief The null filesystem: creation always fails.
 *
 * @param letter the slot letter
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_create_save(char letter);

/**
 * @brief The null filesystem: save write fails.
 *
 * @param s the save handle
 * @param off the offset
 * @param src the source buffer
 * @param n the count
 * @return 0, always (write fails)
 */
static uint32_t _null_save_write(FSSave *s, uint32_t off, const uint8_t *src,
                                 uint32_t n);

/**
 * @brief The null filesystem: nothing to close.
 *
 * @param s the save handle
 */
static void _null_close_save(FSSave *s);

/**
 * @brief The null filesystem: open always fails.
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_open_named(const char *name);

/**
 * @brief The null filesystem: creation always fails.
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static FSSave *_null_create_named(const char *name);

const FSModule g_fs_null = {
    _null_open_volume, _null_close_volume, _null_volume_size, _null_read,
    _null_open_save,   _null_save_read,    _null_create_save, _null_save_write,
    _null_close_save,  _null_open_named,   _null_create_named
};

/* The null filesystem: no volume opens. */
static FSVolume *_null_open_volume(uint16_t disc)
{
    (void)disc;
    return NULL;
}
static void _null_close_volume(FSVolume *v) { (void)v; }
/* The null filesystem: a missing volume has size 0. */
static uint32_t _null_volume_size(FSVolume *v)
{
    (void)v;
    return 0;
}
/* The null filesystem: a read returns no bytes. */
static uint32_t _null_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{
    (void)v;
    (void)off;
    (void)dst;
    (void)n;
    return 0;
}

/* The null filesystem: no save opens. */
static FSSave *_null_open_save(char letter)
{
    (void)letter;
    return NULL;
}
/* The null filesystem: a save read returns no bytes. */
static uint32_t _null_save_read(FSSave *s, uint32_t off, uint8_t *dst,
                                uint32_t n)
{
    (void)s;
    (void)off;
    (void)dst;
    (void)n;
    return 0;
}
/* The null filesystem: no save is created. */
static FSSave *_null_create_save(char letter)
{
    (void)letter;
    return NULL;
}
/* The null filesystem: a save write stores no bytes. */
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
/* The null filesystem: no named file opens. */
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
