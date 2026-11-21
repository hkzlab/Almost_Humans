/* test_arch_mem.c - the settable arch heap.
 *
 * The whole process takes its memory from src/arch/arch_mem.c: OSystem's
 * memAlloc/memFree point at the forwarders and the arch modules call them
 * directly.  Installing a counting pair must therefore see both, and a
 * driver's create/destroy must leave nothing behind. */
#include "arch_mem.h"
#include "audio/adlib/sound_driver_adlib.h"
#include "audio/sound_driver.h"
#include "test_support.h"

#include <stdint.h>
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
 */
static void test_arch_mem(void)
{
    /* 1. the forwarders use the installed pair, and a NULL free is forwarded
     *    to it (free(NULL) is a no-op, but the call is visible). */
    arch_mem_set(_count_alloc, _count_free);
    {
        void *b = arch_mem_alloc(64);

        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_TRUE(_allocs == 1u && _live == 1);
        arch_mem_free(b);
        TEST_ASSERT_TRUE(_frees == 1u && _live == 0);
        arch_mem_free(NULL);
        TEST_ASSERT_TRUE(_frees == 2u && _live == 0);
    }

    /* 2. a driver's create/destroy goes through the same heap. */
    {
        SoundDriver *drv = g_sound_driver_adlib.create(48000);

        TEST_ASSERT_NOT_NULL(drv);
        TEST_ASSERT_TRUE(_allocs >= 2u); // the driver struct at least
        TEST_ASSERT_TRUE(_live >= 1);
        if (drv) g_sound_driver_adlib.destroy(drv);
        TEST_ASSERT_TRUE(_live == 0); // destroy released everything it took
    }

    /* 3. NULL restores the libc pair, so the counters stay put. */
    arch_mem_set(NULL, NULL);
    {
        uint32_t allocs0 = _allocs;
        uint32_t frees0 = _frees;
        void *b = arch_mem_alloc(16);

        TEST_ASSERT_NOT_NULL(b);
        arch_mem_free(b);
        TEST_ASSERT_TRUE(_allocs == allocs0 && _frees == frees0);
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_arch_mem);
    return UNITY_END();
}
