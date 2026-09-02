/* Host entry point that runs the firmware's own on-target vector suite.
 * The suite reports through APP_LOG_*, which the stub captures, so the buffer
 * has to be dumped explicitly or the failures are invisible. */
#include <stdio.h>
#include "app_test.h"

extern const char *log_stub_text(void);

int main(void)
{
  APP_TEST_Result_t r;
  uint32_t failed;

  printf("=== on-target vector suite, run on host ===\n");
  failed = APP_TEST_RunSuite();
  printf("%s", log_stub_text());
  APP_TEST_GetResult(&r);
  printf("vectors %lu  passed %lu  failed %lu\n",
         (unsigned long)r.vectors, (unsigned long)r.passed,
         (unsigned long)r.failed);
  printf("=== %s ===\n", (failed == 0u) ? "PASS" : "FAIL");
  return (failed == 0u) ? 0 : 1;
}
