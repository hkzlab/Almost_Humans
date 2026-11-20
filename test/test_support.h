/* test_support.h - the suite's shared Unity glue.
 *
 * Unity test functions take no arguments, so main stores the ctest command
 * line here and the test reads it back through _test_argc/_test_argv.  The
 * two macros keep the formatted skip/failure diagnostics the suite has always
 * printed while routing them through Unity's report. */
#ifndef HUMANSRE_TEST_SUPPORT_H
#define HUMANSRE_TEST_SUPPORT_H

#include "unity.h"

#include <stdio.h>

/* The ctest command line, stored by test_args_set. */
extern int _test_argc;
extern char **_test_argv;

/**
 * @brief Store the ctest command line for the test functions.
 *
 * @param argc the argument count
 * @param argv the argument vector
 */
void test_args_set(int argc, char **argv);

/* A formatted failure that aborts the current test. */
#define TEST_FAILF(...)                                                        \
    do {                                                                       \
        char msg[512];                                                         \
        snprintf(msg, sizeof msg, __VA_ARGS__);                                \
        TEST_FAIL_MESSAGE(msg);                                                \
    } while (0)

/* A formatted skip that ends the current test. */
#define TEST_SKIPF(...)                                                        \
    do {                                                                       \
        char msg[512];                                                         \
        snprintf(msg, sizeof msg, __VA_ARGS__);                                \
        TEST_IGNORE_MESSAGE(msg);                                              \
    } while (0)

#endif /* HUMANSRE_TEST_SUPPORT_H */
