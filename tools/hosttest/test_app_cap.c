/**
 * @file    test_app_cap.c
 * @brief   Host-side tests for the capture ring: capacity, wrap behaviour,
 *          drop accounting, clipping and snapshot reads.
 */
#include "app_cap.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
static int s_pass;

#define CHECK(cond, ...)                                                    \
  do {                                                                      \
    if (cond) { s_pass++; }                                                 \
    else { s_fail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__);          \
           printf(__VA_ARGS__); printf("\n"); }                             \
  } while (0)

#define CHECK_U(got, want, what)                                             \
  do {                                                                       \
    unsigned long _g = (unsigned long)(got), _w = (unsigned long)(want);     \
    if (_g == _w) { s_pass++; }                                              \
    else { s_fail++; printf("  FAIL %s:%d  %s: got %lu want %lu\n",          \
                            __FILE__, __LINE__, (what), _g, _w); }           \
  } while (0)

static void fill(uint32_t n, uint32_t ts_base)
{
  uint8_t payload[4] = { 0xA1u, 0x61u, 0x2Cu, 0x91u };
  uint32_t i;
  for (i = 0; i < n; i++)
  {
    payload[3] = (uint8_t)i;
    APP_CAP_Record(APP_CAP_T_MSG_IN, 0u, 0u, ts_base + i, payload, sizeof(payload));
  }
}

static void test_empty(void)
{
  APP_CAP_Rec_t r;
  APP_CAP_Stats_t st;

  printf("test_empty\n");
  APP_CAP_Init();
  CHECK_U(APP_CAP_Count(), 0u, "count");
  CHECK(APP_CAP_Get(0u, &r) == -2, "get on empty ring fails");
  CHECK(APP_CAP_Get(0u, NULL) == -1, "NULL out rejected");
  APP_CAP_GetStats(&st);
  CHECK_U(st.capacity, APP_CAP_RING_RECORDS, "capacity");
  CHECK_U(st.count, 0u, "stats count");
  CHECK_U(st.dropped, 0u, "no drops");
  CHECK_U(st.enabled, 1u, "enabled by default");
}

static void test_basic(void)
{
  APP_CAP_Rec_t r;
  APP_CAP_Stats_t st;

  printf("test_basic\n");
  APP_CAP_Init();
  fill(5u, 1000u);

  CHECK_U(APP_CAP_Count(), 5u, "count after 5");
  CHECK(APP_CAP_Get(0u, &r) == 0, "get oldest");
  CHECK_U(r.ts, 1000u, "oldest ts");
  CHECK_U(r.data[3], 0u, "oldest payload");
  CHECK_U(r.len, 4u, "len");
  CHECK_U(r.type, APP_CAP_T_MSG_IN, "type");

  CHECK(APP_CAP_Get(4u, &r) == 0, "get newest");
  CHECK_U(r.ts, 1004u, "newest ts");
  CHECK_U(r.data[3], 4u, "newest payload");

  CHECK(APP_CAP_Get(5u, &r) == -2, "index past the end");

  APP_CAP_GetStats(&st);
  CHECK_U(st.total, 5u, "total");
  CHECK_U(st.msg_in, 5u, "msg_in");
  CHECK_U(st.msg_out, 0u, "msg_out");
  CHECK_U(st.oldest_ts, 1000u, "oldest_ts");
  CHECK_U(st.newest_ts, 1004u, "newest_ts");
}

static void test_wrap_and_drops(void)
{
  APP_CAP_Rec_t r;
  APP_CAP_Stats_t st;

  printf("test_wrap_and_drops\n");
  APP_CAP_Init();

  /* exactly fill the ring: nothing may be dropped yet */
  fill(APP_CAP_RING_RECORDS, 0u);
  CHECK_U(APP_CAP_Count(), APP_CAP_RING_RECORDS, "ring full");
  APP_CAP_GetStats(&st);
  CHECK_U(st.dropped, 0u, "no drops at exactly full");
  CHECK_U(st.wrapped, 0u, "not wrapped yet");

  /* one more record evicts the oldest */
  fill(1u, 0xFFFFFFFFu);
  CHECK_U(APP_CAP_Count(), APP_CAP_RING_RECORDS, "still full");
  APP_CAP_GetStats(&st);
  CHECK_U(st.total, APP_CAP_RING_RECORDS + 1u, "total keeps counting");
  CHECK_U(st.dropped, 1u, "one drop");
  CHECK_U(st.wrapped, 1u, "wrapped");

  /* the oldest readable record is now the second one written */
  CHECK(APP_CAP_Get(0u, &r) == 0, "get after wrap");
  CHECK_U(r.ts, 1u, "oldest shifted by one");
  /* and the newest is the record we just added */
  CHECK(APP_CAP_Get(APP_CAP_RING_RECORDS - 1u, &r) == 0, "get newest after wrap");
  CHECK_U(r.ts, 0xFFFFFFFFu, "newest ts after wrap");

  /* many more writes: drop count tracks exactly the evictions */
  APP_CAP_Init();
  fill(APP_CAP_RING_RECORDS + 100u, 0u);
  APP_CAP_GetStats(&st);
  CHECK_U(st.count, APP_CAP_RING_RECORDS, "count clamped to capacity");
  CHECK_U(st.dropped, 100u, "100 drops");
  CHECK(APP_CAP_Get(0u, &r) == 0, "readable");
  CHECK_U(r.ts, 100u, "oldest after 100 evictions");
}

static void test_clipping(void)
{
  uint8_t big[APP_CAP_PAYLOAD + 40];
  APP_CAP_Rec_t r;
  APP_CAP_Stats_t st;

  printf("test_clipping\n");
  APP_CAP_Init();
  memset(big, 0x5A, sizeof(big));

  APP_CAP_Record(APP_CAP_T_MSG_IN, 0u, 0u, 7u, big, sizeof(big));
  CHECK(APP_CAP_Get(0u, &r) == 0, "get clipped record");
  CHECK_U(r.len, APP_CAP_PAYLOAD, "clipped length");
  CHECK_U(r.flags & APP_CAP_F_CLIPPED, APP_CAP_F_CLIPPED, "clipped flag");
  APP_CAP_GetStats(&st);
  CHECK_U(st.clipped, 1u, "clipped counter");

  /* zero-length records are legal (state notifications) */
  APP_CAP_Record(APP_CAP_T_PE_STATE, 0u, 0xFFu, 8u, NULL, 0u);
  CHECK(APP_CAP_Get(1u, &r) == 0, "get zero-length");
  CHECK_U(r.len, 0u, "zero length");
  CHECK_U(r.sop, 0xFFu, "sop passthrough");
  CHECK_U(r.type, APP_CAP_T_PE_STATE, "type passthrough");
}

static void test_enable_and_clear(void)
{
  APP_CAP_Stats_t st;

  printf("test_enable_and_clear\n");
  APP_CAP_Init();
  APP_CAP_SetEnabled(0u);
  CHECK_U(APP_CAP_IsEnabled(), 0u, "disabled");
  fill(4u, 0u);
  CHECK_U(APP_CAP_Count(), 0u, "nothing recorded while disabled");
  APP_CAP_SetEnabled(1u);
  fill(2u, 0u);
  CHECK_U(APP_CAP_Count(), 2u, "recording resumed");

  APP_CAP_Clear();
  CHECK_U(APP_CAP_Count(), 0u, "cleared");
  APP_CAP_GetStats(&st);
  CHECK_U(st.total, 0u, "total reset");
  CHECK_U(st.dropped, 0u, "drops reset");
  CHECK_U(st.msg_in, 0u, "msg_in reset");
  CHECK_U(st.enabled, 1u, "still enabled after clear");
}

static void test_elapsed(void)
{
  printf("test_elapsed\n");
  /* 400 MHz core: 400 cycles == 1 us */
  CHECK_U(APP_CAP_ElapsedUs(0u, 400u, 400000000u), 1u, "1 us");
  CHECK_U(APP_CAP_ElapsedUs(0u, 400000000u, 400000000u), 1000000u, "1 s");
  /* wrap: 0xFFFFFF00 -> 0x00000090 is 0x190 = 400 cycles across the
   * 2^32 boundary, i.e. exactly 1 us at 400 MHz. */
  CHECK_U(APP_CAP_ElapsedUs(0xFFFFFF00u, 0x00000090u, 400000000u), 1u,
          "wrap-safe interval");
  /* 0xFFFFFFF0 -> 0x000000F0 is 0x100 = 256 cycles = 0.64 us, which the
   * integer division truncates to 0.  Documents the resolution limit. */
  CHECK_U(APP_CAP_ElapsedUs(0xFFFFFFF0u, 0x000000F0u, 400000000u), 0u,
          "sub-microsecond interval truncates");
  CHECK_U(APP_CAP_ElapsedUs(0u, 100u, 0u), 0u, "zero clock guarded");
}

int main(void)
{
  printf("=== app_cap host tests ===\n");
  test_empty();
  test_basic();
  test_wrap_and_drops();
  test_clipping();
  test_enable_and_clear();
  test_elapsed();
  printf("=== %d passed, %d failed ===\n", s_pass, s_fail);
  return (s_fail == 0) ? 0 : 1;
}
