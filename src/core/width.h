/* Port-only: checked narrowing for the explicit casts.
 *
 * -Wconversion rejects implicit narrowing and HUMANSRE_SANITIZE_IMPLICIT aborts
 * on implicit conversions; these helpers abort at the cast in a debug build and
 * compile to the plain cast under NDEBUG.  Intentional 8/16-bit wraps keep a
 * plain cast and are documented at the call site.
 */
#ifndef HUMANSRE_WIDTH_H
#define HUMANSRE_WIDTH_H

#include <assert.h>
#include <stdint.h>

static inline uint16_t u16(uint32_t v)
{
    assert(v <= 0xFFFFu);
    return (uint16_t)v;
}

static inline uint8_t u8(uint32_t v)
{
    assert(v <= 0xFFu);
    return (uint8_t)v;
}

static inline int16_t i16(int32_t v)
{
    assert(v >= -32768 && v <= 32767);
    return (int16_t)v;
}

static inline int8_t i8(int32_t v)
{
    assert(v >= -128 && v <= 127);
    return (int8_t)v;
}

#endif /* HUMANSRE_WIDTH_H */
