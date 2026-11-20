/* test_support.c - the suite's shared Unity glue (see test_support.h). */
#include "test_support.h"

int _test_argc;
char **_test_argv;

void test_args_set(int argc, char **argv)
{
    _test_argc = argc;
    _test_argv = argv;
}

/* Unity's per-test fixture hooks; this suite has no per-test state. */
void setUp(void) {}

void tearDown(void) {}
