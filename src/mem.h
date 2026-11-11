/* The core's heap: the buffers the original took from DOS (INT 21h AH=48h).
 *
 * The core never calls malloc/free: every dynamic buffer goes through the
 * backend's memAlloc/memFree.  A buffer that models one of the original's
 * runtime blocks is allocated when its module accesses it and released when
 * the module unloads; a block the original kept for the whole session (the
 * boot heap [0x48AC]'s shared arenas, its VGA pages aside) is allocated at
 * boot and released at exit.
 */

#ifndef HUMANSRE_MEM_H
#define HUMANSRE_MEM_H

#include "osystem.h"

#include <stddef.h>

/**
 * @brief Bind the heap to the backend.  Idempotent; every module init calls it.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t mem_bind(const OSystem *osys);

/**
 * @brief Allocate one block from the backend's heap.
 *
 * @param size the byte count
 * @return the block, or NULL when unbound or out of memory
 */
void *mem_alloc(size_t size);

/**
 * @brief Release a block from mem_alloc.
 *
 * @param p the block (NULL is a no-op)
 */
void mem_free(void *p);

#endif /* HUMANSRE_MEM_H */
