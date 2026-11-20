/* test_res.c - res_load_chunk against the real volume files.
 *
 * Byte-exact fixture test: the expected bytes are the extracted resource
 * files themselves (documented byte-identical to the game's loaded image).
 * argv[1] = the game data directory holding
 * HUMANS.000/001/002, argv[2] = the extracted-tree expectation root.
 * Missing volume files = skip (exit 0 with a notice); a wrong decode with
 * data present = fail.
 *
 * The disc follows the original: the level-intro index picks the disc
 * through the ported table res_movie_disc (202F:0x6287) and the front end
 * runs on disc 0 ([0xF7E] = 0 in attract mode, live-verified).
 *
 * The resource-level checksum values (chkOut) are asserted against the
 * corpus measurement (the 2026-10-23 sweep in the decode's own model -
 * the last-block reset semantics of resource_load's BP): ANI03 = 0x3c3f,
 * ITB = 0x2adb. */
#include "null/osys_null.h"
#include "osystem.h"
#include "res.h"
#include "compat.h"
#include "test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/**
 * @brief Report whether the fixture volume HUMANS.000 is absent.
 *
 * @param dir the direction
 * @return 1 when the volume is absent, 0 otherwise
 */
static int32_t _data_missing(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fclose(f);
    return 0;
}

/**
 * @brief Read a whole file into a malloc'd buffer; NULL on error.
 *
 * @param path the path
 * @param size the size
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

/**
 * @brief Decode one resource and compare it with the fixture and its checksum.
 *
 * @param name the name
 * @param disc the disc number
 * @param expect_path the expected fixture path
 * @param want_chk the wanted checksum
 * @return 0 on success, 1 on a mismatch
 */
static int32_t _check_chunk(const char *name, uint32_t disc,
                            const char *expect_path, uint16_t want_chk)
{
    uint32_t expect_size = 0;
    uint8_t *expect = _read_file(expect_path, &expect_size);
    if (!expect) {
        fprintf(stderr, "FAIL fixture unreadable: %s\n", expect_path);
        return 1;
    }
    res_set_disc(disc);
    uint32_t got_size = 0;
    uint16_t got_chk = 0;
    uint8_t *got = res_load_chunk(name, &got_size, &got_chk);
    int32_t bad = 1;
    if (!got) {
        fprintf(stderr, "FAIL %s: res_load_chunk returned NULL\n", name);
    } else if (got_size != expect_size) {
        fprintf(stderr, "FAIL %s: size %u, want %u\n", name, got_size,
                expect_size);
    } else if (memcmp(got, expect, expect_size) != 0) {
        size_t at = 0;
        while (at < expect_size && got[at] == expect[at]) at++;
        fprintf(stderr, "FAIL %s: first diff at byte %zu\n", name, at);
    } else if (got_chk != want_chk) {
        fprintf(stderr, "FAIL %s: checksum %04x, want %04x\n", name, got_chk,
                want_chk);
    } else {
        bad = 0;
    }
    free(got);
    free(expect);
    return bad;
}

/**
 * @brief Run the resource decoder tests.
 */
static void test_res(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : "."; // HUMANS.00N
    /* argv[2] = the extracted tree; it defaults to the volume dir */
    const char *fix_dir = _test_argc > 2 ? _test_argv[2] : vol_dir;
    if (_data_missing(vol_dir)) {
        TEST_SKIPF("skip: no volume files at %s (fixture data absent)",
                   vol_dir);
    }
    /* fs opens its volumes relative to the process cwd; the fixture paths
     * are resolved against the caller's cwd first. */
    char fix_abs[PATH_MAX];
    if (!compat_realpath(fix_dir, fix_abs)) {
        TEST_FAILF("fixture tree %s unreadable", fix_dir);
    }
    if (compat_chdir(vol_dir)) { TEST_FAILF("cannot enter %s", vol_dir); }

    char path[512];

    osys_bind(&g_osys_null);
    res_init();

    /* The ANI disc table, corpus-verified 10/10 (see res.c): ANI03 -> 1. */
    if (res_movie_disc(0) != 2 || res_movie_disc(1) != 0 ||
        res_movie_disc(3) != 1 || res_movie_disc(9) != 2 ||
        res_movie_disc(10) != 0xFF) {
        TEST_FAILF("res_movie_disc table");
    }
    snprintf(path, sizeof path, "%s/HUMANS.001/ANM/ANI03.ANI", fix_abs);
    TEST_ASSERT_FALSE(
        _check_chunk("ANM\\ANI03.ANI", res_movie_disc(3), path, 0x3c3f));

    /* The front end loads the intro assets from disc 0. */
    snprintf(path, sizeof path, "%s/HUMANS.000/INTRO/ITB.BIT", fix_abs);
    TEST_ASSERT_FALSE(_check_chunk("INTRO\\ITB.BIT", 0, path, 0x2adb));

    /* Case-insensitive lookup mirrors the original's name comparison. */
    TEST_ASSERT_FALSE(_check_chunk("intro\\itb.bit", 0, path, 0x2adb));

    /* A resource that is not on the CURRENT disc must fail - the scan is
     * gone, the original never crosses discs ([0xF7E]). */
    res_set_disc(1);
    uint32_t sz = 0;
    TEST_ASSERT_NULL_MESSAGE(res_load_chunk("INTRO\\ITB.BIT", &sz, NULL),
                             "wrong-disc load must return NULL");

    TEST_ASSERT_FALSE_MESSAGE(
        res_load_chunk("ANM\\NOPE.ANI", &sz, NULL) != NULL || sz != 0,
        "missing name should return NULL");

    /* D-145 (closed 2026-11-05): the stored arm writes WORDS (1000:919D
     * SHR CX,1), so an odd take's tail byte is never written; the stream
     * still advances by the full take, which is where the next flag is read.
     * A synthetic volume whose stored payload is TRUNCATED by that odd byte
     * must still decode: the old code read `take` bytes and failed here. */
    {
        char cwd[PATH_MAX];
        static const char scratch[] = "humans_res_d145";

        if (!compat_realpath(".", cwd)) {
            TEST_FAILF("cwd unreadable");
        } else {
            (void)compat_mkdir(scratch); // already present is fine
            snprintf(path, sizeof path, "%s/HUMANS.000", scratch);
            uint8_t vol[0x48];
            memset(vol, 0, sizeof vol);
            vol[0] = 1; // one record
            memcpy(vol + 2, "TEST\\ODD.BIN      ", 18);
            vol[2 + 0x36] = 5; // the block size (odd)
            vol[2 + 0x38] = 5; // the chunk length (odd)
            vol[2 + 0x3C] = 0x42; // the data offset
            vol[0x42] = 0; // the flag word: stored
            vol[0x44] = 0x11; // the four even payload
            vol[0x45] = 0x22;
            vol[0x46] = 0x33;
            vol[0x47] = 0x44;
            FILE *f = fopen(path, "wb");
            if (!f || fwrite(vol, 1, sizeof vol, f) != sizeof vol) {
                TEST_FAILF("cannot write the D-145 volume");
            } else {
                fclose(f);
                if (compat_chdir(scratch)) {
                    TEST_FAILF("cannot enter %s", scratch);
                } else {
                    uint32_t n = 0;
                    uint8_t *got;

                    res_set_disc(0); // the synthetic volume
                    got = res_load_chunk("TEST\\ODD.BIN", &n, NULL);

                    if (!got) {
                        TEST_FAILF("D-145: the truncated stored "
                                   "block must decode");
                    } else {
                        if (n != 5u || got[0] != 0x11u || got[1] != 0x22u ||
                            got[2] != 0x33u || got[3] != 0x44u) {
                            TEST_FAILF("D-145: the stored bytes");
                        }
                        free(got);
                    }
                    if (compat_chdir(cwd)) perror(cwd);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_res);
    return UNITY_END();
}
