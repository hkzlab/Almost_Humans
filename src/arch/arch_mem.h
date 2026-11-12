/* The process heap, shared by the arch modules and the core.
 *
 * This is the only non-thirdparty place that calls the CRT's allocator: the
 * OSystem memAlloc/memFree pair points at the forwarders below, so installing
 * a pair here covers every core allocation as well as the arch modules that
 * call arch_mem_alloc/arch_mem_free directly.  Thirdparty modules (the
 * vendored OPL3 and the impulse speaker model, and the system mt32emu) keep
 * their own allocator.
 */

#ifndef HUMANSRE_ARCH_MEM_H
#define HUMANSRE_ARCH_MEM_H

#include <stddef.h>

/**
 * @brief Install the heap pair used by the whole process.
 *
 * Process-wide and unsynchronised: install it once at startup, before the
 * first allocation (the tests do it at the top of main).  A NULL function
 * restores the libc default for that side, so arch_mem_set(NULL, NULL) is
 * the teardown.
 *
 * @param alloc the allocation function (NULL: libc malloc)
 * @param free_fn the release function (NULL: libc free)
 */
void arch_mem_set(void *(*alloc)(size_t size), void (*free_fn)(void *p));

/**
 * @brief Allocate one block from the installed heap.
 *
 * @param size the byte count
 * @return the block, or NULL out of memory
 */
void *arch_mem_alloc(size_t size);

/**
 * @brief Release a block from arch_mem_alloc.
 *
 * @param p the block to free
 */
void arch_mem_free(void *p);

#endif /* HUMANSRE_ARCH_MEM_H */
