/* byteorder.h — the one place the game's data bytes become integers.
 *
 * Every on-disk format is little-endian (port_structure.md §5), but a few
 * asset tables are big-endian (REX.DAT's frame words, the terrain bank), so
 * both orders live here and nothing else in `src/` composes a multi-byte
 * integer out of individual bytes.
 *
 * The host order is decided at compile time.  On a little-endian host the
 * little-endian functions are a plain unaligned load (memcpy, which every
 * compiler folds into one mov); the big-endian ones become the same load plus
 * a byte swap (`bswap`/`movbe`).  On a big-endian host the roles are mirrored.
 * That is the whole point of reading through this module instead of shifting
 * bytes by hand.
 *
 * The conversion helpers `bo_*` are named for the order of their input; the
 * swap is its own inverse, so the same helper serves reading and writing.
 * 8-bit values have no byte order, so they carry no le/be in their names.
 *
 * Copyright: same as the rest of the port.
 */
#ifndef HUMANSRE_BYTEORDER_H
#define HUMANSRE_BYTEORDER_H

#include <stdint.h>
#include <string.h>

/* --- compile-time host byte order ------------------------------------ */

/* May be forced on the command line (-DBYTEORDER_HOST_LITTLE=0) to compile
 * the other branch; left alone, it is detected. */
#ifndef BYTEORDER_HOST_LITTLE
#    if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&         \
        defined(__ORDER_BIG_ENDIAN__)
#        if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#            define BYTEORDER_HOST_LITTLE 1
#        elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#            define BYTEORDER_HOST_LITTLE 0
#        else
#            error "byteorder: __BYTE_ORDER__ is neither little nor big endian"
#        endif
#    elif defined(_WIN32) || defined(_MSC_VER) || defined(__i386__) ||         \
        defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) ||          \
        defined(_M_ARM) || defined(_M_ARM64) || defined(__LITTLE_ENDIAN__)
/* Windows is little-endian on every architecture it targets; these are the
 * remaining compilers that do not publish __BYTE_ORDER__. */
#        define BYTEORDER_HOST_LITTLE 1
#    else
#        error                                                                 \
            "byteorder: unknown host byte order; add its predefined macro here"
#    endif
#endif /* BYTEORDER_HOST_LITTLE */

/* --- the host-order load/store primitives ----------------------------
 *
 * memcpy is deliberate.  It is the C99-safe way to say "read a native u16
 * out of an arbitrary byte buffer": a cast would be UB on an unaligned
 * address.  The size is constant, so gcc and clang lower it to one
 * unaligned load/store (gcc even at -O0, clang from -O1 up) -- the direct
 * mapping this module exists for.  Spelling the bytes out instead
 * (`p[0] | p[1] << 8`) reaches the same instruction at -O2, but then the
 * native and foreign paths would each carry their own hand-written byte
 * order; here the foreign order is just the bswap below.
 *
 * In an unoptimised clang build nothing is inlined, so these become calls;
 * that is a debug-build cost, not a property of the generated code.
 */
/**
 * @brief Load a 16-bit word from a byte pointer in host order.
 *
 * @param p the source bytes
 * @return the loaded word
 */
static inline uint16_t bo_ld16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/**
 * @brief Load a 32-bit word from a byte pointer in host order.
 *
 * @param p the source bytes
 * @return the loaded word
 */
static inline uint32_t bo_ld32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/**
 * @brief Store a 16-bit word in host order.
 *
 * @param p the destination bytes
 * @param v the word
 */
static inline void bo_st16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof v); }
/**
 * @brief Store a 32-bit word in host order.
 *
 * @param p the destination bytes
 * @param v the word
 */
static inline void bo_st32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

/**
 * @brief Byte-swap a 16-bit value.
 *
 * @param v the value to swap
 * @return the byte-swapped value
 */
static inline uint16_t bo_bswap16(uint16_t v)
{ return (uint16_t)((uint16_t)(v >> 8) | (uint16_t)(v << 8)); }

/**
 * @brief Byte-swap a 32-bit value.
 *
 * @param v the value to swap
 * @return the byte-swapped value
 */
static inline uint32_t bo_bswap32(uint32_t v)
{
    return (v >> 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
           (v << 24);
}

/* Map a value in the named order to/from the host's order. */
#if BYTEORDER_HOST_LITTLE
/**
 * @brief Convert a little-endian 16-bit value to host order.
 *
 * @param v the little-endian value
 * @return the host-order value
 */
static inline uint16_t bo_le16(uint16_t v) { return v; }
/**
 * @brief Convert a little-endian 32-bit value to host order.
 *
 * @param v the little-endian value
 * @return the host-order value
 */
static inline uint32_t bo_le32(uint32_t v) { return v; }
/**
 * @brief Convert a big-endian 16-bit value to host order.
 *
 * @param v the big-endian value
 * @return the host-order value
 */
static inline uint16_t bo_be16(uint16_t v) { return bo_bswap16(v); }
/**
 * @brief Convert a big-endian 32-bit value to host order.
 *
 * @param v the big-endian value
 * @return the host-order value
 */
static inline uint32_t bo_be32(uint32_t v) { return bo_bswap32(v); }
#else
/**
 * @brief Convert a little-endian 16-bit value to host order.
 *
 * @param v the little-endian value
 * @return the host-order value
 */
static inline uint16_t bo_le16(uint16_t v) { return bo_bswap16(v); }
/**
 * @brief Convert a little-endian 32-bit value to host order.
 *
 * @param v the little-endian value
 * @return the host-order value
 */
static inline uint32_t bo_le32(uint32_t v) { return bo_bswap32(v); }
/**
 * @brief Convert a big-endian 16-bit value to host order.
 *
 * @param v the big-endian value
 * @return the host-order value
 */
static inline uint16_t bo_be16(uint16_t v) { return v; }
/**
 * @brief Convert a big-endian 32-bit value to host order.
 *
 * @param v the big-endian value
 * @return the host-order value
 */
static inline uint32_t bo_be32(uint32_t v) { return v; }
#endif

/* --- reads ----------------------------------------------------------- */
/**
 * @brief Read one byte.
 *
 * @param p the source bytes
 * @return the byte
 */
static inline uint8_t rd_u8(const uint8_t *p) { return *p; }
/**
 * @brief Read one signed byte.
 *
 * @param p the source bytes
 * @return the signed byte
 */
static inline int8_t rd_i8(const uint8_t *p) { return (int8_t)*p; }

/**
 * @brief Read a little-endian 16-bit word.
 *
 * @param p the source bytes
 * @return the word
 */
static inline uint16_t rd_le_u16(const uint8_t *p)
{ return bo_le16(bo_ld16(p)); }
/**
 * @brief Read a little-endian signed 16-bit word.
 *
 * @param p the source bytes
 * @return the signed word
 */
static inline int16_t rd_le_i16(const uint8_t *p)
{ return (int16_t)rd_le_u16(p); }
/**
 * @brief Read a big-endian 16-bit word.
 *
 * @param p the source bytes
 * @return the word
 */
static inline uint16_t rd_be_u16(const uint8_t *p)
{ return bo_be16(bo_ld16(p)); }
/**
 * @brief Read a big-endian signed 16-bit word.
 *
 * @param p the source bytes
 * @return the signed word
 */
static inline int16_t rd_be_i16(const uint8_t *p)
{ return (int16_t)rd_be_u16(p); }

/**
 * @brief Read a little-endian 32-bit word.
 *
 * @param p the source bytes
 * @return the word
 */
static inline uint32_t rd_le_u32(const uint8_t *p)
{ return bo_le32(bo_ld32(p)); }
/**
 * @brief Read a little-endian signed 32-bit word.
 *
 * @param p the source bytes
 * @return the signed word
 */
static inline int32_t rd_le_i32(const uint8_t *p)
{ return (int32_t)rd_le_u32(p); }
/**
 * @brief Read a big-endian 32-bit word.
 *
 * @param p the source bytes
 * @return the word
 */
static inline uint32_t rd_be_u32(const uint8_t *p)
{ return bo_be32(bo_ld32(p)); }
/**
 * @brief Read a big-endian signed 32-bit word.
 *
 * @param p the source bytes
 * @return the signed word
 */
static inline int32_t rd_be_i32(const uint8_t *p)
{ return (int32_t)rd_be_u32(p); }

/* --- writes ---------------------------------------------------------- */
/**
 * @brief Write one byte.
 *
 * @param p the destination bytes
 * @param v the byte
 */
static inline void wr_u8(uint8_t *p, uint8_t v) { *p = v; }
/**
 * @brief Write one signed byte.
 *
 * @param p the destination bytes
 * @param v the signed byte
 */
static inline void wr_i8(uint8_t *p, int8_t v) { *p = (uint8_t)v; }

/**
 * @brief Write a little-endian 16-bit word.
 *
 * @param p the destination bytes
 * @param v the word
 */
static inline void wr_le_u16(uint8_t *p, uint16_t v) { bo_st16(p, bo_le16(v)); }
/**
 * @brief Write a little-endian signed 16-bit word.
 *
 * @param p the destination bytes
 * @param v the signed word
 */
static inline void wr_le_i16(uint8_t *p, int16_t v)
{ wr_le_u16(p, (uint16_t)v); }
/**
 * @brief Write a big-endian 16-bit word.
 *
 * @param p the destination bytes
 * @param v the word
 */
static inline void wr_be_u16(uint8_t *p, uint16_t v) { bo_st16(p, bo_be16(v)); }
/**
 * @brief Write a big-endian signed 16-bit word.
 *
 * @param p the destination bytes
 * @param v the signed word
 */
static inline void wr_be_i16(uint8_t *p, int16_t v)
{ wr_be_u16(p, (uint16_t)v); }

/**
 * @brief Write a little-endian 32-bit word.
 *
 * @param p the destination bytes
 * @param v the word
 */
static inline void wr_le_u32(uint8_t *p, uint32_t v) { bo_st32(p, bo_le32(v)); }
/**
 * @brief Write a little-endian signed 32-bit word.
 *
 * @param p the destination bytes
 * @param v the signed word
 */
static inline void wr_le_i32(uint8_t *p, int32_t v)
{ wr_le_u32(p, (uint32_t)v); }
/**
 * @brief Write a big-endian 32-bit word.
 *
 * @param p the destination bytes
 * @param v the word
 */
static inline void wr_be_u32(uint8_t *p, uint32_t v) { bo_st32(p, bo_be32(v)); }
/**
 * @brief Write a big-endian signed 32-bit word.
 *
 * @param p the destination bytes
 * @param v the signed word
 */
static inline void wr_be_i32(uint8_t *p, int32_t v)
{ wr_be_u32(p, (uint32_t)v); }

#endif /* HUMANSRE_BYTEORDER_H */
