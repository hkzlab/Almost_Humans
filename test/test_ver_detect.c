/* test_ver_detect.c - the language detector (ver_detect.h).
 *
 * Unit part: the FNV-1a known vectors and the digest -> game_strings column map.
 * Data part: the real volume sets, argv[1] being a directory that holds one
 * subdirectory per language (ENG, FRE, GER, ITA, SPA), each with its
 * HUMANS.001.  Missing data = skip; a wrong detection = fail.
 *
 * The expected digests are the 2026-11-08 measurement over the first 2048
 * bytes of each shipped HUMANS.001; this test is what pins them, so a
 * replaced volume set fails here instead of silently showing the wrong text.
 */
#include "compat.h"
#include "game_strings.h"
#include "null/osys_null.h"
#include "ver_detect.h"
#include "test_support.h"

#include <stdio.h>

/**
 * @brief Run the unit vectors, the map and the real-volume detection.
 */
static void test_ver_detect(void)
{
    static const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o' };
    static const struct {
        const char *dir;
        int32_t lang;
    } langs[] = {
        { "ENG", GAME_LANGUAGE_EN },  { "FRE", GAME_LANGUAGE_FRE },
        { "GER", GAME_LANGUAGE_GER }, { "ITA", GAME_LANGUAGE_ITA },
        { "SPA", GAME_LANGUAGE_SPA },
    };

    /* the algorithm: the standard FNV-1a 32-bit vectors */
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_hash(NULL, 0) == 0x811C9DC5u,
                             "empty digest");
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_hash(hello, 5) == 0x4F9F2CABu,
                             "hello digest");

    /* the map: every shipped digest, and an unknown one */
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_lang(0xCFDBFBFFu) == GAME_LANGUAGE_EN,
                             "map ENG");
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_lang(0xD7713AFAu) == GAME_LANGUAGE_FRE,
                             "map FRE");
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_lang(0xA6F63422u) == GAME_LANGUAGE_GER,
                             "map GER");
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_lang(0x5910D3A2u) == GAME_LANGUAGE_ITA,
                             "map ITA");
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_lang(0xD9AB8E78u) == GAME_LANGUAGE_SPA,
                             "map SPA");
    TEST_ASSERT_TRUE_MESSAGE(ver_detect_lang(0x12345678u) == GAME_LANGUAGE_EN,
                             "an unknown digest keeps English");

    /* the chdir into the scratch directory below breaks a relative root, so
     * resolve the fixture root before it */
    char root[PATH_MAX];
    int32_t have_root = _test_argc >= 2 && compat_realpath(_test_argv[1], root);

    /* A volume that is absent or shorter than the window is a failure to
     * detect the installed data, not an English fallback: the boot must
     * abort instead (ver_detect.h). */
    {
        static const char nodata[] = "humans_ver_detect_nodata";

        (void)compat_mkdir(nodata); // already present is fine
        if (compat_chdir(nodata)) TEST_FAILF("cannot enter %s", nodata);
        TEST_ASSERT_TRUE_MESSAGE(ver_detect_run(&g_osys_null) < 0,
                                 "missing volume is an error");
        FILE *v = fopen("HUMANS.001", "wb");
        if (v) {
            fputs("short", v);
            fclose(v);
        }
        TEST_ASSERT_TRUE_MESSAGE(ver_detect_run(&g_osys_null) < 0,
                                 "short volume is an error");
        remove("HUMANS.001");
    }

    if (_test_argc < 2) TEST_SKIPF("skip: no volume-set root given");
    if (!have_root) TEST_SKIPF("skip: no volume-set root at %s", _test_argv[1]);

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
        if (compat_chdir(dir)) TEST_FAILF("cannot enter %s", dir);
        TEST_ASSERT_TRUE_MESSAGE(ver_detect_run(&g_osys_null) == langs[i].lang,
                                 langs[i].dir);
        checked++;
    }

    if (!checked) TEST_SKIPF("skip: no volume sets under %s", root);
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_ver_detect);
    return UNITY_END();
}
