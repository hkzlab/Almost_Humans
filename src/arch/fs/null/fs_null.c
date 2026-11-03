/* fs/null/fs_null.c — the null filesystem module: every operation fails.
 *
 * For ports that have nothing to load (the headless no-data builds) and as
 * the deliberate "no disc" state.  openVolume returns NULL, which the core's
 * callers treat as "the volume is not available" and own the error path. */
#include "fs/fs.h"
#include <stdio.h>

static FSVolume *null_openVolume(uint32_t disc) { (void)disc; return NULL; }
static void      null_closeVolume(FSVolume *v) { (void)v; }
static uint32_t  null_volumeSize(FSVolume *v) { (void)v; return 0; }
static uint32_t  null_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{ (void)v; (void)off; (void)dst; (void)n; return 0; }

static FSSave   *null_openSave(char letter) { (void)letter; return NULL; }
static uint32_t  null_saveRead(FSSave *s, uint32_t off, uint8_t *dst,
                               uint32_t n)
{ (void)s; (void)off; (void)dst; (void)n; return 0; }
static FSSave   *null_createSave(char letter) { (void)letter; return NULL; }
static uint32_t  null_saveWrite(FSSave *s, uint32_t off, const uint8_t *src,
                                uint32_t n)
{ (void)s; (void)off; (void)src; (void)n; return 0; }
static void      null_closeSave(FSSave *s) { (void)s; }
static FSSave   *null_openNamed(const char *name) { (void)name; return NULL; }
static FSSave   *null_createNamed(const char *name) { (void)name; return NULL; }

const FSModule fs = {
    null_openVolume, null_closeVolume, null_volumeSize, null_read,
    null_openSave, null_saveRead, null_createSave, null_saveWrite,
    null_closeSave, null_openNamed, null_createNamed
};
