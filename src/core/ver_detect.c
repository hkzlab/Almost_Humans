#include "ver_detect.h"
#include "game_strings.h"
#include "fs/fs.h"
#include "osystem.h"

#include <stdint.h>
#include <stdio.h>

#define VER_DETECT_EN_HASH 0xCFDBFBFFu // GOG's CD English version volume digest

#define FNV32_OFFSET 0x811C9DC5u // the FNV-1a 32-bit offset basis
#define FNV32_PRIME  0x01000193u // the FNV-1a 32-bit prime

/**
 * @brief Extend one FNV-1a 32-bit digest with n bytes.
 *
 * @param h the running digest (FNV32_OFFSET for a fresh one)
 * @param buf the bytes
 * @param n the byte count
 * @return the extended digest
 */
static uint32_t _hash_update(uint32_t h, const uint8_t *buf, uint32_t n);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint32_t ver_detect_hash(const uint8_t *buf, uint32_t n)
{ return _hash_update(FNV32_OFFSET, buf, n); }
#endif /* HUMANSRE_TEST_PROBES */

static uint32_t _hash_update(uint32_t h, const uint8_t *buf, uint32_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        h ^= buf[i];
        h *= FNV32_PRIME;
    }
    return h;
}

int32_t ver_detect_lang(uint32_t hash)
{
    switch (hash) {
        case VER_DETECT_EN_HASH: return GAME_LANGUAGE_EN;
        case 0xD7713AFAu: return GAME_LANGUAGE_FRE;
        case 0xA6F63422u: return GAME_LANGUAGE_GER;
        case 0x5910D3A2u: return GAME_LANGUAGE_ITA;
        case 0xD9AB8E78u: return GAME_LANGUAGE_SPA;
        default:
            /* An unknown volume set (a dump of another release?) */
            return GAME_LANGUAGE_EN;
    }
}

int32_t ver_detect_run(const OSystem *osys)
{
    /* use chunked reading to save memory */
    uint8_t chunk[256];
    char msg[128];
    FSVolume *v = osys->fs->openVolume((uint16_t)VER_DETECT_VOLUME);
    uint16_t got = 0;
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
    if (lang == GAME_LANGUAGE_EN && hash != VER_DETECT_EN_HASH) {
        snprintf(msg, sizeof msg,
                 "ver_detect: unrecognized volume set (digest "
                 "0x%08X); using the English text",
                 hash);
        osys->warn(msg);
    }

    return lang;
}
