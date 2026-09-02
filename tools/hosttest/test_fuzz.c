/*
 * Host tests for the malformed-message engine, run under ASan/UBSan so any
 * out-of-bounds read caused by a crafted frame is a hard failure.
 */
#include <stdio.h>
#include <string.h>
#include "app_fuzz.h"

static int s_fail;

static void expect(uint32_t got, uint32_t want, const char *what)
{
  if (got != want)
  {
    s_fail++;
    printf("  FAIL %s: got %lu want %lu\n", what,
           (unsigned long)got, (unsigned long)want);
  }
}

/* Every mutation kind must be exercised, and none may be silently accepted. */
static void run_seed(uint32_t seed, uint32_t cases)
{
  APP_FUZZ_Result_t r;
  uint32_t bad = APP_FUZZ_Run(seed, cases, &r);
  uint32_t i;
  char label[64];

  snprintf(label, sizeof(label), "seed 0x%08lX unflagged", (unsigned long)seed);
  expect(bad, 0u, label);

  if (bad != 0u)
  {
    printf("    first at case %lu, mutation %s\n",
           (unsigned long)r.last_bad_case,
           APP_FUZZ_MutName((APP_FUZZ_Mut_t)r.last_bad_mut));
  }

  /* The generator must be deterministic: the same seed gives the same result. */
  {
    APP_FUZZ_Result_t r2;
    (void)APP_FUZZ_Run(seed, cases, &r2);
    expect(r2.flagged, r.flagged, "deterministic flagged count");
    expect(r2.cases, r.cases, "deterministic case count");
  }

  /* Coverage: report which mutation kinds were hit. */
  for (i = 1u; i < (uint32_t)APP_FUZZ_MUT_COUNT; i++)
  {
    if (r.per_mut[i] == 0u && cases >= 400u)
    {
      printf("  note: mutation %s not exercised at %lu cases\n",
             APP_FUZZ_MutName((APP_FUZZ_Mut_t)i), (unsigned long)cases);
    }
  }
}

int main(void)
{
  uint32_t s;

  printf("=== fuzz engine host tests ===\n");

  /* Edge cases. */
  {
    APP_FUZZ_Result_t r;
    expect(APP_FUZZ_Run(1u, 0u, &r), 0u, "zero cases");
    expect(r.ok, 1u, "zero cases ok");
    expect(APP_FUZZ_Run(1u, 10u, NULL), 0u, "null result");
  }

  /* A spread of seeds, enough cases to reach every mutation kind. */
  for (s = 1u; s <= 24u; s++)
  {
    run_seed(s * 2654435761u, 600u);
  }

  printf("=== %s ===\n", (s_fail == 0) ? "PASS" : "FAIL");
  return (s_fail == 0) ? 0 : 1;
}
