#include "osystem.h"

/* The single backend: written once by osys_bind, read everywhere else.
 * Boot binds it before any core allocation or vtable call. */
const OSystem *g_osys;

void osys_bind(const OSystem *osys) { g_osys = osys; }
