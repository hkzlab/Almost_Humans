/* ver_detect.c — identify the shipped language from HUMANS.001 (ver_detect.h).
 *
 * The digests are the FNV-1a 32-bit values of the first 2048 bytes of each
 * shipped language's HUMANS.001 (binaries/volumes/<LANG>/), measured
 * 2026-11-08 and pinned by tests/test_ver_detect.c against the real files.
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

/**
 * @brief FNV-1a 32-bit digest of a byte buffer.
 *
 * @param buf the buffer (NULL is accepted when n is 0)
 * @param n the byte count
 * @return the digest, the offset basis for an empty buffer
 */
uint32_t ver_detect_hash(const uint8_t *buf, uint32_t n)
{
    uint32_t h = 0x811C9DC5u; /* the FNV-1a 32-bit offset basis */
    for (uint32_t i = 0; i < n; i++) {
        h ^= buf[i];
        h *= 0x01000193u; /* the FNV-1a 32-bit prime */
    }
    return h;
}

/**
 * @brief Map a HUMANS.001 digest to a game_strs column.
 *
 * @param hash a digest from ver_detect_hash over the VER_DETECT_LEN window
 * @return the enum GameLang index, GAME_LANG_EN for an unknown digest
 */
int32_t ver_detect_lang(uint32_t hash)
{
    switch (hash) {
    case VER_DETECT_EN_HASH: return GAME_LANG_EN;
    case 0xD7713AFAu: return GAME_LANG_FRE;
    case 0xA6F63422u: return GAME_LANG_GER;
    case 0x5910D3A2u: return GAME_LANG_ITA;
    case 0xD9AB8E78u: return GAME_LANG_SPA;
    default:
        /* An unknown volume set (a dump of another release): keep the
         * shipped text rather than index a column that may not exist. */
        return GAME_LANG_EN;
    }
}

/**
 * @brief Identify the installed language from HUMANS.001.
 *
 * @param osys the backend interface (the warning's destination)
 * @return the enum GameLang index, GAME_LANG_EN on a missing, short or
 *         unrecognized volume (with a warning)
 */
int32_t ver_detect_run(const OSystem *osys)
{
    /* static: a 2 KiB stack frame is a lot on the constrained targets, and
     * the port calls this once at boot. */
    static uint8_t buf[VER_DETECT_LEN];
    char msg[128];
    FSVolume *v = g_fs.openVolume(VER_DETECT_VOLUME);
    uint32_t got, hash;
    int32_t lang;

    if (!v) {
        snprintf(msg, sizeof msg,
                 "ver_detect: HUMANS.%03u is missing; using the English text",
                 (unsigned)VER_DETECT_VOLUME);
        osys->warn(msg);
        return GAME_LANG_EN;
    }
    got = g_fs.read(v, 0, buf, VER_DETECT_LEN);
    g_fs.closeVolume(v);
    if (got != VER_DETECT_LEN) {
        snprintf(msg, sizeof msg,
                 "ver_detect: HUMANS.%03u is short (%u of %u bytes); using "
                 "the English text",
                 (unsigned)VER_DETECT_VOLUME, (unsigned)got,
                 (unsigned)VER_DETECT_LEN);
        osys->warn(msg);
        return GAME_LANG_EN;
    }

    hash = ver_detect_hash(buf, VER_DETECT_LEN);
    lang = ver_detect_lang(hash);
    /* The map falls back to English for an unknown digest too; the real ENG
     * set is the one case where that answer is not a fallback. */
    if (lang == GAME_LANG_EN && hash != VER_DETECT_EN_HASH) {
        snprintf(msg, sizeof msg,
                 "ver_detect: unrecognized volume set (HUMANS.%03u digest "
                 "0x%08X); using the English text",
                 (unsigned)VER_DETECT_VOLUME, (unsigned)hash);
        osys->warn(msg);
    }
    return lang;
}
