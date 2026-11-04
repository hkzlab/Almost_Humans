#ifndef HUMANSRE_OBJECT_DS_H
#define HUMANSRE_OBJECT_DS_H

#include <stdint.h>

/* The original data segment (202F:0x0000..0xA4AF), copied from the image.
 * object.c uses it as the backing store for every address the unguarded
 * class/record arithmetic can reach, and for the throw's word read. */
#define OBJ_DS_LEN 0xA4B0u
extern uint8_t objDsImage[OBJ_DS_LEN];

#endif
