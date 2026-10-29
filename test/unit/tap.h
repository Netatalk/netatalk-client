#ifndef NETATALK_CLIENT_TEST_TAP_H_
#define NETATALK_CLIENT_TEST_TAP_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_output_tap;
static unsigned int test_case_num;

static inline void test_tap_init(int argc, char **argv)
{
    test_output_tap = argc == 2 && strcmp(argv[1], "--tap") == 0;

    if (test_output_tap) {
        setvbuf(stdout, NULL, _IONBF, 0);
    }
}

static inline void test_tap_check(int passed, const char *description,
                                  const char *file, int line)
{
    if (test_output_tap) {
        printf("%s %u - %s\n", passed ? "ok" : "not ok",
               ++test_case_num, description);

        if (!passed) {
            printf("# failed at %s:%d\n", file, line);
            printf("Bail out! stopping after failed assertion\n");
        }
    } else if (!passed) {
        fprintf(stderr, "check failed at %s:%d: %s\n",
                file, line, description);
    }

    if (!passed) {
        exit(EXIT_FAILURE);
    }
}

static inline int test_tap_finish(void)
{
    if (test_output_tap) {
        printf("1..%u\n", test_case_num);
    }

    return EXIT_SUCCESS;
}

#define CHECK(condition) \
    test_tap_check((condition), #condition, __FILE__, __LINE__)

/* A test of one behaviour, as netatalk's afpd test harness runs them: the
 * call returns 0, or the number of the step that failed, which the report
 * names. */
static inline void test_tap_int(int got, int expected,
                                const char *description, const char *file,
                                int line)
{
    if (test_output_tap) {
        printf("%s %u - %s\n", got == expected ? "ok" : "not ok",
               ++test_case_num, description);

        if (got != expected) {
            printf("# failed at %s:%d\n", file, line);
            printf("# got: %d\n# expected: %d\n", got, expected);
            printf("Bail out! stopping after failed assertion\n");
        }
    } else if (got != expected) {
        fprintf(stderr, "test failed at %s:%d: %s: got %d, expected %d\n",
                file, line, description, got, expected);
    }

    if (got != expected) {
        exit(EXIT_FAILURE);
    }
}

#define TEST_int(a, b, desc) \
    test_tap_int((a), (b), (desc), __FILE__, __LINE__)

#endif
