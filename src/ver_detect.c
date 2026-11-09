/* ver_detect.c - identify the resource language
 *
 * The digests are the FNV-1a 32-bit values of the first 2048 bytes of each
 * GOG's shipped language's HUMANS.001 .
 *
 * HUMANS.000 cannot tell ENG, GER and ITA apart (those three are byte-
 * identical) and HUMANS.002 is identical in every language, so .001 is the
 * only volume that carries the distinction.
 */
#include "ver_detect.h"
#include "game_strs.h"
#include "fs/fs.h"

#include <stdio.h>

/* The English set's digest.  ver_detect_lang's default arm returns the same
 * column, so telling "the real English set" from "an unknown set" needs the
 * value itself (both in the map and in ver_detect_run's warning). */
#define VER_DETECT_EN_HASH 0xCFDBFBFFu

#define FNV32_OFFSET 0x811C9DC5u /* the FNV-1a 32-bit offset basis */
#define FNV32_PRIME  0x01000193u /* the FNV-1a 32-bit prime       */

/**
 * @brief Extend one FNV-1a 32-bit digest with n bytes.
 *
 * @param h the running digest (FNV32_OFFSET for a fresh one)
 * @param buf the bytes
 * @param n the byte count
 * @return the extended digest
 */
static uint32_t _hash_update(uint32_t h, const uint8_t *buf, uint32_t n);

uint32_t ver_detect_hash(const uint8_t *buf, uint32_t n)
{ return _hash_update(FNV32_OFFSET, buf, n); }

static uint32_t _hash_update(uint32_t h, const uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        h ^= buf[i];
        h *= FNV32_PRIME;
    }
    return h;
}

int32_t ver_detect_lang(uint32_t hash)
{
    switch (hash) {
    case VER_DETECT_EN_HASH: return GAME_LANG_EN;
    case 0xD7713AFAu: return GAME_LANG_FRE;
    case 0xA6F63422u: return GAME_LANG_GER;
    case 0x5910D3A2u: return GAME_LANG_ITA;
    case 0xD9AB8E78u: return GAME_LANG_SPA;
    default:
        /* An unknown volume set (a dump of another release?) */
        return GAME_LANG_EN;
    }
}

int32_t ver_detect_run(const OSystem *osys)
{
    /* use chunked reading to save memory */
    uint8_t chunk[256];
    char msg[128];
    FSVolume *v = osys->fs->openVolume(VER_DETECT_VOLUME);
    uint32_t got = 0;
    uint32_t hash = FNV32_OFFSET;
    int32_t lang;

    /* A missing or short identifying volume is not a language fallback: the
     * caller must abort the boot (ver_detect.h). */
    if (!v) return -1;

    while (got < VER_DETECT_LEN) {
        uint32_t want = VER_DETECT_LEN - got;
        uint32_t n;

        if (want > sizeof chunk) want = (uint32_t)sizeof chunk;
        n = osys->fs->read(v, got, chunk, want);
        if (n == 0) break;
        hash = _hash_update(hash, chunk, n);
        got += n;
    }
    osys->fs->closeVolume(v);

    if (got != VER_DETECT_LEN) return -1;

    lang = ver_detect_lang(hash);
    /* The map falls back to English for an unknown digest; the real ENG
     * set is the one case where that answer is not a fallback. */
    if (lang == GAME_LANG_EN && hash != VER_DETECT_EN_HASH) {
        snprintf(msg, sizeof msg,
                 "ver_detect: unrecognized volume set (digest "
                 "0x%08X); using the English text",
                 (unsigned)hash);
        osys->warn(msg);
    }

    return lang;
}
