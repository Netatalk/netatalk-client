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

#endif
