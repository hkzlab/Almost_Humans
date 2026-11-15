#include "arch_mem.h"

#include <stdlib.h>

static void *(*_alloc)(size_t) = malloc;
static void (*_free)(void *) = free;

void arch_mem_set(void *(*alloc)(size_t size), void (*free_fn)(void *p))
{
    _alloc = alloc ? alloc : malloc;
    _free = free_fn ? free_fn : free;
}

void *arch_mem_alloc(size_t size) { return _alloc(size); }

void arch_mem_free(void *p) { _free(p); }
