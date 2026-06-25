#ifndef GH_TEST_H
#define GH_TEST_H
#include <stdio.h>
#include <string.h>
static int gh_tests_run = 0, gh_tests_failed = 0;
#define CHECK(cond) do { \
    gh_tests_run++; \
    if (!(cond)) { gh_tests_failed++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    gh_tests_run++; \
    if (_a != _b) { gh_tests_failed++; \
        printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
               __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)
#define RUN_TEST(fn) do { printf("TEST %s\n", #fn); fn(); } while (0)
#define TEST_SUMMARY() ( \
    printf("\n%d checks, %d failed\n", gh_tests_run, gh_tests_failed), \
    gh_tests_failed == 0 ? 0 : 1)
#endif
