/* orouteragent - minimal test harness
 *
 * Deliberately dependency-free: the decoders under test only need libc,
 * so the tests build and run anywhere without pulling in a framework.
 */
#ifndef ORA_TEST_HARNESS_H
#define ORA_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ora_tests_run;
static int ora_tests_failed;
static const char *ora_current_test = "";

#define TEST(name) static void name(void)

#define RUN_TEST(fn)                                                         \
    do {                                                                     \
        ora_current_test = #fn;                                              \
        ora_tests_run++;                                                     \
        fn();                                                                \
    } while (0)

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ora_tests_failed++;                                              \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__,         \
                    ora_current_test);                                       \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n  condition: %s\n", #cond);                   \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(got, want, ...)                                         \
    do {                                                                     \
        long long g_ = (long long)(got), w_ = (long long)(want);             \
        if (g_ != w_) {                                                      \
            ora_tests_failed++;                                              \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__,         \
                    ora_current_test);                                       \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n  got %lld, want %lld\n", g_, w_);            \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_EQ_MEM(got, want, n, ...)                                      \
    do {                                                                     \
        if (memcmp((got), (want), (n)) != 0) {                               \
            ora_tests_failed++;                                              \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__,         \
                    ora_current_test);                                       \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n  memory differs over %zu bytes\n",           \
                    (size_t)(n));                                            \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_EQ_STR(got, want, ...)                                         \
    do {                                                                     \
        const char *g_ = (got), *w_ = (want);                                \
        if (!g_ || !w_ || strcmp(g_, w_) != 0) {                             \
            ora_tests_failed++;                                              \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__,         \
                    ora_current_test);                                       \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n  got \"%s\", want \"%s\"\n",                 \
                    g_ ? g_ : "(null)", w_ ? w_ : "(null)");                 \
            return;                                                          \
        }                                                                    \
    } while (0)

static int ora_test_report(const char *suite)
{
    if (ora_tests_failed) {
        fprintf(stderr, "\n%s: %d/%d checks failed\n", suite,
                ora_tests_failed, ora_tests_run);
        return 1;
    }
    printf("%s: %d tests passed\n", suite, ora_tests_run);
    return 0;
}

#endif