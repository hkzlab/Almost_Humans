/* fs/null/fs_null.c — the null filesystem module: every operation fails.
 *
 * For ports that have nothing to load (the headless no-data builds) and as
 * the deliberate "no disc" state.  openVolume returns NULL, which the core's
 * callers treat as "the volume is not available" and own the error path. */
#include "fs/fs.h"
#include <stdio.h>

static FSVolume *null_openVolume(unsigned disc) { (void)disc; return NULL; }
static void      null_closeVolume(FSVolume *v) { (void)v; }
static uint32_t  null_volumeSize(FSVolume *v) { (void)v; return 0; }
static uint32_t  null_read(FSVolume *v, uint32_t off, uint8_t *dst, uint32_t n)
{ (void)v; (void)off; (void)dst; (void)n; return 0; }

const FSModule fs = { null_openVolume, null_closeVolume, null_volumeSize, null_read };
