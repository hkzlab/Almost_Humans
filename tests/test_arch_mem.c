/* test_arch_mem.c - the settable arch heap.
 *
 * The whole process takes its memory from src/arch/arch_mem.c: OSystem's
 * memAlloc/memFree point at the forwarders and the arch modules call them
 * directly.  Installing a counting pair must therefore see both, and a
 * driver's create/destroy must leave nothing behind. */
#include "arch_mem.h"
#include "audio/adlib/sound_driver_adlib.h"
#include "audio/sound_driver.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t _allocs;
static uint32_t _frees;
static int32_t _live;

/**
 * @brief The counting allocator's allocation side.
 *
 * @param size the byte count
 * @return the block, or NULL out of memory
 */
static void *_count_alloc(size_t size)
{
    void *p = malloc(size);

    if (p) {
        _allocs++;
        _live++;
    }
    return p;
}

/**
 * @brief The counting allocator's release side.
 *
 * Counts every call, including a NULL block: arch_mem_free forwards NULL to
 * the installed free, so a call with NULL must be visible here.
 *
 * @param p the block
 */
static void _count_free(void *p)
{
    _frees++;
    if (p) _live--;
    free(p);
}

/**
 * @brief Run the arch heap's checks.
 *
 * @return 0 on success, 1 on failure
 */
int main(void)
{
    int32_t bad = 0;

    /* 1. the forwarders use the installed pair, and a NULL free is forwarded
     *    to it (free(NULL) is a no-op, but the call is visible). */
    arch_mem_set(_count_alloc, _count_free);
    {
        void *b = arch_mem_alloc(64);

        bad |= b == NULL;
        bad |= _allocs != 1u || _live != 1;
        arch_mem_free(b);
        bad |= _frees != 1u || _live != 0;
        arch_mem_free(NULL);
        bad |= _frees != 2u || _live != 0;
    }

    /* 2. a driver's create/destroy goes through the same heap. */
    {
        SoundDriver *drv = g_sound_driver_adlib.create(48000);

        bad |= drv == NULL;
        bad |= _allocs < 2u; // the driver struct at least
        bad |= _live < 1;
        if (drv) g_sound_driver_adlib.destroy(drv);
        bad |= _live != 0; // destroy released everything it took
    }

    /* 3. NULL restores the libc pair, so the counters stay put. */
    arch_mem_set(NULL, NULL);
    {
        uint32_t allocs0 = _allocs;
        uint32_t frees0 = _frees;
        void *b = arch_mem_alloc(16);

        bad |= b == NULL;
        arch_mem_free(b);
        bad |= _allocs != allocs0 || _frees != frees0;
    }

    if (bad) {
        fprintf(stderr, "test_arch_mem: FAIL\n");
        return 1;
    }
    printf("test_arch_mem: ok (%u allocs, %u frees)\n", _allocs, _frees);
    return 0;
}
