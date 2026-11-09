/* width.h — checked narrowing for the explicit casts in the width work.
 *
 * The build rejects implicit narrowing (-Wconversion) and the sanitizer gate
 * (HUMANSRE_SANITIZE_IMPLICIT) aborts on implicit conversions at run time.
 * These helpers cover the remaining explicit casts: in a test/debug build an
 * out-of-range value aborts at the cast instead of silently wrapping, while a
 * release build (NDEBUG) compiles them to the plain cast.  Intentional 8/16-bit
 * wraps (the DEC-then-test counters) keep a plain cast and are documented as
 * such; only value-range casts use these.
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
