/* test_ver_detect.c - the language detector (ver_detect.h).
 *
 * Unit part: the FNV-1a known vectors and the digest -> game_strs column map.
 * Data part: the real volume sets, argv[1] being a directory that holds one
 * subdirectory per language (ENG, FRE, GER, ITA, SPA), each with its
 * HUMANS.001.  Missing data = skip; a wrong detection = fail.
 *
 * The expected digests are the 2026-11-08 measurement over the first 2048
 * bytes of each shipped HUMANS.001; this test is what pins them, so a
 * replaced volume set fails here instead of silently showing the wrong text.
 */
#include "compat.h"
#include "game_strs.h"
#include "ver_detect.h"

#include <stdio.h>

static int32_t _bad;

extern const OSystem g_osys_null;

/**
 * @brief Assert one condition and count a failure.
 *
 * @param ok the condition
 * @param what the check description
 */
static void _check(int32_t ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        _bad = 1;
    }
}

/**
 * @brief Run the unit vectors, the map and the real-volume detection.
 *
 * @param argc the argument count
 * @param argv argv[1] = the volume-set root (optional)
 * @return 0 on success (or a data skip), 1 on failure
 */
int main(int argc, char **argv)
{
    static const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o' };
    static const struct {
        const char *dir;
        int32_t lang;
    } langs[] = {
        { "ENG", GAME_LANG_EN },  { "FRE", GAME_LANG_FRE },
        { "GER", GAME_LANG_GER }, { "ITA", GAME_LANG_ITA },
        { "SPA", GAME_LANG_SPA },
    };

    /* the algorithm: the standard FNV-1a 32-bit vectors */
    _check(ver_detect_hash(NULL, 0) == 0x811C9DC5u, "empty digest");
    _check(ver_detect_hash(hello, 5) == 0x4F9F2CABu, "hello digest");

    /* the map: every shipped digest, and an unknown one */
    _check(ver_detect_lang(0xCFDBFBFFu) == GAME_LANG_EN, "map ENG");
    _check(ver_detect_lang(0xD7713AFAu) == GAME_LANG_FRE, "map FRE");
    _check(ver_detect_lang(0xA6F63422u) == GAME_LANG_GER, "map GER");
    _check(ver_detect_lang(0x5910D3A2u) == GAME_LANG_ITA, "map ITA");
    _check(ver_detect_lang(0xD9AB8E78u) == GAME_LANG_SPA, "map SPA");
    _check(ver_detect_lang(0x12345678u) == GAME_LANG_EN,
           "an unknown digest keeps English");

    if (_bad) return 1;

    /* the chdir into the scratch directory below breaks a relative root, so
     * resolve the fixture root before it */
    char root[PATH_MAX];
    int32_t have_root = argc >= 2 && compat_realpath(argv[1], root);

    /* A volume that is absent or shorter than the window is a failure to
     * detect the installed data, not an English fallback: the boot must
     * abort instead (ver_detect.h). */
    {
        static const char nodata[] = "humans_ver_detect_nodata";

        (void)compat_mkdir(nodata); /* already present is fine */
        if (compat_chdir(nodata)) {
            perror(nodata);
            return 1;
        }
        _check(ver_detect_run(&g_osys_null) < 0, "missing volume is an error");
        FILE *v = fopen("HUMANS.001", "wb");
        if (v) {
            fputs("short", v);
            fclose(v);
        }
        _check(ver_detect_run(&g_osys_null) < 0, "short volume is an error");
        remove("HUMANS.001");
        if (_bad) return 1;
    }

    if (argc < 2) {
        printf("skip: no volume-set root given\n");
        return 0;
    }
    if (!have_root) {
        printf("skip: no volume-set root at %s\n", argv[1]);
        return 0;
    }

    /* the real sets: the digest must pick each language's own column */
    int32_t checked = 0;
    for (uint32_t i = 0; i < sizeof langs / sizeof langs[0]; i++) {
        char path[512];
        char dir[512];

        snprintf(path, sizeof path, "%s/%s/HUMANS.001", root, langs[i].dir);
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("skip: no %s (fixture data absent)\n", path);
            continue;
        }
        fclose(f);

        snprintf(dir, sizeof dir, "%s/%s", root, langs[i].dir);
        if (compat_chdir(dir)) {
            perror(dir);
            return 1;
        }
        _check(ver_detect_run(&g_osys_null) == langs[i].lang, langs[i].dir);
        checked++;
    }

    if (_bad) return 1;
    if (!checked) {
        printf("skip: no volume sets under %s\n", root);
        return 0;
    }
    printf("ok ver_detect: the FNV-1a vectors and %d language digest(s)\n",
           (int)checked);
    return 0;
}
