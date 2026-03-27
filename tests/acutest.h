/*
 * Minimal acutest-compatible harness for nanopdf.
 * API-compatible subset used by this repository.
 */
#ifndef ACUTEST_H
#define ACUTEST_H

#include <stdio.h>

typedef void (*TestFunc)(void);

struct TEST_ENTRY {
  const char* name;
  TestFunc func;
};

extern struct TEST_ENTRY TEST_LIST[];

static int acutest_current_failures = 0;
static int acutest_total_failures = 0;
static int acutest_total_tests = 0;

#define TEST_MSG(...)                         \
  do {                                        \
    fprintf(stderr, __VA_ARGS__);             \
    fprintf(stderr, "\n");                    \
  } while (0)

#define TEST_CHECK(cond)                                                          \
  do {                                                                            \
    if (!(cond)) {                                                                \
      acutest_current_failures++;                                                 \
      TEST_MSG("%s:%d: check failed: %s", __FILE__, __LINE__, #cond);            \
    }                                                                             \
  } while (0)

#define TEST_CHECK_(cond, ...)                                                    \
  do {                                                                            \
    if (!(cond)) {                                                                \
      acutest_current_failures++;                                                 \
      TEST_MSG("%s:%d: check failed: %s", __FILE__, __LINE__, #cond);            \
      TEST_MSG(__VA_ARGS__);                                                      \
    }                                                                             \
  } while (0)

#define TEST_ASSERT(cond)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      acutest_current_failures++;                                                 \
      TEST_MSG("%s:%d: assert failed: %s", __FILE__, __LINE__, #cond);           \
      return;                                                                     \
    }                                                                             \
  } while (0)

#ifndef TEST_NO_MAIN
int main(void) {
  int failed_tests = 0;
  int i = 0;
  for (i = 0; TEST_LIST[i].name != NULL; ++i) {
    acutest_total_tests++;
    acutest_current_failures = 0;
    fprintf(stdout, "[acutest] RUN %s\n", TEST_LIST[i].name);
    TEST_LIST[i].func();
    if (acutest_current_failures == 0) {
      fprintf(stdout, "[acutest] OK  %s\n", TEST_LIST[i].name);
    } else {
      fprintf(stdout, "[acutest] FAIL %s (%d)\n", TEST_LIST[i].name,
              acutest_current_failures);
      failed_tests++;
      acutest_total_failures += acutest_current_failures;
    }
  }

  fprintf(stdout, "[acutest] tests: %d, failed: %d, assertions failed: %d\n",
          acutest_total_tests, failed_tests, acutest_total_failures);
  return failed_tests == 0 ? 0 : 1;
}
#endif

#endif  // ACUTEST_H
