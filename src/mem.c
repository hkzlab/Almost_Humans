#include "mem.h"

static const OSystem *_mem_osys;

int32_t mem_bind(const OSystem *osys)
{
    _mem_osys = osys;
    return 0;
}

void *mem_alloc(size_t size)
{
    if (!_mem_osys || !_mem_osys->memAlloc) return NULL;
    return _mem_osys->memAlloc(size);
}

void mem_free(void *p)
{
    if (!p || !_mem_osys || !_mem_osys->memFree) return;
    _mem_osys->memFree(p);
}
