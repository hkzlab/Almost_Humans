#include "arch_mem.h"

#include <stdlib.h>

static void *_libc_alloc(size_t size) { return malloc(size); }

static void _libc_free(void *p) { free(p); }

static void *(*_alloc)(size_t) = _libc_alloc;
static void (*_free)(void *) = _libc_free;

void arch_mem_set(void *(*alloc)(size_t size), void (*free_fn)(void *p))
{
    _alloc = alloc ? alloc : _libc_alloc;
    _free = free_fn ? free_fn : _libc_free;
}

void *arch_mem_alloc(size_t size) { return _alloc(size); }

void arch_mem_free(void *p)
{
    if (p) _free(p);
}
