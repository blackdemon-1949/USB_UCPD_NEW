/**
 * @file    test_engines.c
 * @brief   Host tests for the cable, transaction and power engines.
 *
 * Every expectation below is hand-computed from the USB PD 3.1 bit layouts as
 * transcribed into app_cable.h / app_dec.h, or from exact integer arithmetic.
 */
#include "app_cable.h"
#include "app_txn.h"
#include "app_pwr.h"
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

#define CHECK_I(got, want, what)                                             \
  do {                                                                       \
    long _g = (long)(got), _w = (long)(want);                                \
    if (_g == _w) { s_pass++; }                                              \
    else { s_fail++; printf("  FAIL %s:%d  %s: got %ld want %ld\n",          \
                            __FILE__, __LINE__, (what), _g, _w); }           \
  } while (0)

#define CHECK_S(got, want, what)                                             \
  do {                                                                       \
    if (strcmp((got), (want)) == 0) { s_pass++; }                            \
    else { s_fail++; printf("  FAIL %s:%d  %s: got \"%s\" want \"%s\"\n",    \
                            __FILE__, __LINE__, (what), (got), (want)); }    \
  } while (0)

#define CORE_HZ 400000000u
#define MS(x) ((uint32_t)((x) * 400000u))   /* milliseconds -> cycles @400MHz */

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Build a standard PD header: type, data role, spec rev, power role, id, ndo */
static uint16_t hdr(unsigned type, unsigned dro, unsigned rev, unsigned pro,
                    unsigned id, unsigned ndo)
{
  return (uint16_t)(type | (dro << 5) | (rev << 6) | (pro << 8) |
                    (id << 9) | (ndo << 12));
}

/* ================================================================== */
/* Cable / E-marker                                                   */
/* ================================================================== */
static void test_cable_vdo(void)
{
  /* Passive cable: Gen1 SS, 5 A, 50 V, latency code 4, EPR capable,
   * Type-C far end, fw 1, hw 2, passive mode. */
  const uint32_t vdo = (1u << 0) | (2u << 5) | (3u << 9) | (0u << 11) |
                       (4u << 13) | (1u << 17) | (2u << 18) |
                       (1u << 22) | (2u << 26);
  APP_CBL_Info_t i;
  char buf[192];

  printf("test_cable_vdo\n");
  CHECK_U(vdo, 0x084A8641u, "hand-built cable VDO");

  APP_CBL_DecodeVdo(vdo, 0u, &i);
  CHECK_U(i.valid, 1u, "valid");
  CHECK_U(i.active, 0u, "passive");
  CHECK_U(i.ss_support, APP_CBL_SS_GEN1, "ss support");
  CHECK_U(i.current_cap, APP_CBL_CUR_5A, "current cap");
  CHECK_U(i.max_vbus, APP_CBL_VBUS_50V, "max vbus");
  CHECK_U(i.latency, 4u, "latency");
  CHECK_U(i.epr_capable, 1u, "epr capable");
  CHECK_U(i.to_type, APP_CBL_TO_C, "to type");
  CHECK_U(i.fw_ver, 1u, "fw");
  CHECK_U(i.hw_ver, 2u, "hw");

  CHECK_U(APP_CBL_MaxVoltageMv(APP_CBL_VBUS_20V), 20000u, "20V");
  CHECK_U(APP_CBL_MaxVoltageMv(APP_CBL_VBUS_50V), 50000u, "50V");
  CHECK_U(APP_CBL_MaxCurrentMa(APP_CBL_CUR_3A), 3000u, "3A");
  CHECK_U(APP_CBL_MaxCurrentMa(APP_CBL_CUR_5A), 5000u, "5A");
  CHECK_U(APP_CBL_MaxCurrentMa(APP_CBL_CUR_DEFAULT), 0u, "default -> no PD limit");

  CHECK_S(APP_CBL_SsName(APP_CBL_SS_GEN1), "USB3.2 Gen1", "ss name");
  CHECK_S(APP_CBL_TermName(APP_CBL_TERM_PASSIVE_NOVCONN),
          "passive, no VCONN", "term name");
  CHECK_S(APP_CBL_ProductTypeName(APP_CBL_PT_PASSIVE_CABLE),
          "passive cable", "product type name");

  APP_CBL_FormatInfo(&i, buf, sizeof(buf));
  CHECK(strstr(buf, "passive") != NULL, "format says passive");
  CHECK(strstr(buf, "50V") != NULL, "format shows 50V");
  CHECK(strstr(buf, "5A") != NULL, "format shows 5A");
  CHECK(strstr(buf, "EPR") != NULL, "format shows EPR");
}

static void test_cable_discover_identity(void)
{
  /* Discover Identity ACK: VDM header + ID header + cert stat + product +
   * cable VDO.  ID header product type 6 (passive cable) in B29..27. */
  uint8_t pay[4 + 16];
  APP_CBL_Info_t i;
  int n;

  printf("test_cable_discover_identity\n");
  memset(pay, 0, sizeof(pay));
  put32(&pay[0], 0xFF000021u);                  /* SVID FF00, cmd 1, ACK */
  put32(&pay[4], 0x30001234u);                  /* VID 1234, pt 6        */
  put32(&pay[8], 0x00000000u);                  /* cert stat             */
  put32(&pay[12], 0xABCD0001u);                 /* pid ABCD, version 1   */
  put32(&pay[16], 0x084A8641u);                 /* cable VDO             */

  n = APP_CBL_DecodeDiscoverIdentityAck(pay, sizeof(pay), &i);
  CHECK_I(n, 4, "four VDOs parsed");
  CHECK_U(i.vid, 0xABCDu, "product VDO pid field");
  CHECK_U(i.pid, 0x0001u, "product VDO version field");
  CHECK_U(i.current_cap, APP_CBL_CUR_5A, "current via identity");
  CHECK_U(i.epr_capable, 1u, "epr via identity");

  /* not a Discover Identity response */
  put32(&pay[0], 0xFF00000Eu);                  /* command 14 */
  CHECK_I(APP_CBL_DecodeDiscoverIdentityAck(pay, sizeof(pay), &i), -3,
          "wrong command rejected");

  /* truncated payload */
  put32(&pay[0], 0xFF000021u);
  CHECK_I(APP_CBL_DecodeDiscoverIdentityAck(pay, 8u, &i), -4,
          "missing VDO4 rejected");

  CHECK_I(APP_CBL_DecodeDiscoverIdentityAck(NULL, 0u, &i), -1, "NULL payload");
}

static void test_cable_check(void)
{
  APP_CBL_Info_t i;

  printf("test_cable_check\n");
  memset(&i, 0, sizeof(i));
  CHECK_U(APP_CBL_Check(&i, 9000u, 3000u, 0u), APP_CBL_NO_CABLE,
          "no identity yet");

  i.valid = 1u;
  i.max_vbus = APP_CBL_VBUS_20V;
  i.current_cap = APP_CBL_CUR_3A;
  i.epr_capable = 0u;

  CHECK_U(APP_CBL_Check(&i, 20000u, 3000u, 0u), APP_CBL_OK, "20V/3A ok");
  CHECK_U(APP_CBL_Check(&i, 48000u, 3000u, 0u), APP_CBL_VOLT_LIMIT,
          "48V exceeds a 20V cable");
  CHECK_U(APP_CBL_Check(&i, 9000u, 5000u, 0u), APP_CBL_CURR_LIMIT,
          "5A exceeds a 3A cable");
  CHECK_U(APP_CBL_Check(&i, 9000u, 3000u, 1u), APP_CBL_NOT_EPR,
          "EPR on a non-EPR cable");

  i.max_vbus = APP_CBL_VBUS_50V;
  i.current_cap = APP_CBL_CUR_5A;
  i.epr_capable = 1u;
  CHECK_U(APP_CBL_Check(&i, 48000u, 5000u, 1u), APP_CBL_OK, "48V/5A EPR ok");

  CHECK_S(APP_CBL_VerdictName(APP_CBL_NOT_EPR), "cable not EPR capable",
          "verdict name");
}

/* ================================================================== */
/* Transaction reconstruction                                         */
/* ================================================================== */
static void test_txn_happy_path(void)
{
  APP_TXN_Port_t p;
  uint8_t m[2 + 28];

  printf("test_txn_happy_path\n");
  APP_TXN_SetClock(CORE_HZ);
  APP_TXN_Init(&p);
  CHECK_U(p.state, APP_TXN_DETACHED, "starts detached");
  CHECK_U(p.sop, 0xFFu, "no sop yet");

  /* Source_Capabilities, 6 objects, MessageID 0 */
  memset(m, 0, sizeof(m));
  put32(&m[0], 0u);
  m[0] = (uint8_t)hdr(1u, 1u, 2u, 1u, 0u, 6u);
  m[1] = (uint8_t)(hdr(1u, 1u, 2u, 1u, 0u, 6u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(0), m, 2u + 24u);
  CHECK_U(p.state, APP_TXN_NEGOTIATING, "caps -> negotiating");
  CHECK_U(p.n_caps, 1u, "caps counted");

  /* our Request for object 2, MessageID 0 */
  memset(m, 0, sizeof(m));
  m[0] = (uint8_t)hdr(2u, 0u, 2u, 0u, 0u, 1u);
  m[1] = (uint8_t)(hdr(2u, 0u, 2u, 0u, 0u, 1u) >> 8);
  put32(&m[2], 0x2104B12Cu);
  APP_TXN_NoteRequest(&p, 9000u, 3000u, 0u);
  APP_TXN_Feed(&p, 1u, 0u, MS(1), m, 6u);
  CHECK_U(p.n_req, 1u, "request counted");
  CHECK_U(p.req_pos, 2u, "requested object position");

  /* Accept from the source, MessageID 1 */
  memset(m, 0, sizeof(m));
  m[0] = (uint8_t)hdr(3u, 1u, 2u, 1u, 1u, 0u);
  m[1] = (uint8_t)(hdr(3u, 1u, 2u, 1u, 1u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(3), m, 2u);
  CHECK_U(p.state, APP_TXN_TRANSITION, "accept -> transition");
  CHECK_U(p.accept_us, 2000u, "request->accept is 2 ms");

  /* PS_RDY, MessageID 2 */
  memset(m, 0, sizeof(m));
  m[0] = (uint8_t)hdr(6u, 1u, 2u, 1u, 2u, 0u);
  m[1] = (uint8_t)(hdr(6u, 1u, 2u, 1u, 2u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(30), m, 2u);
  CHECK_U(p.state, APP_TXN_CONTRACT, "ps_rdy -> contract");
  CHECK_U(p.psrdy_us, 27000u, "accept->ps_rdy is 27 ms");
  CHECK_U(p.contract_us, 29000u, "request->ps_rdy is 29 ms");
  CHECK_U(p.n_contracts, 1u, "one contract");
  CHECK_U(p.contract_mv, 9000u, "negotiated voltage kept");

  CHECK_S(APP_TXN_StateName(p.state), "contract", "state name");
}

static void test_txn_failure_paths(void)
{
  APP_TXN_Port_t p;
  uint8_t m[6];

  printf("test_txn_failure_paths\n");
  APP_TXN_SetClock(CORE_HZ);

  /* --- Reject ----------------------------------------------------- */
  APP_TXN_Init(&p);
  memset(m, 0, sizeof(m));
  m[0] = (uint8_t)hdr(2u, 0u, 2u, 0u, 0u, 1u);
  m[1] = (uint8_t)(hdr(2u, 0u, 2u, 0u, 0u, 1u) >> 8);
  put32(&m[2], 0x1104B12Cu);
  APP_TXN_Feed(&p, 1u, 0u, MS(0), m, 6u);

  m[0] = (uint8_t)hdr(4u, 1u, 2u, 1u, 1u, 0u);      /* Reject, wire 0x04 */
  m[1] = (uint8_t)(hdr(4u, 1u, 2u, 1u, 1u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(2), m, 2u);
  CHECK_U(p.state, APP_TXN_FAILED, "reject -> failed");
  CHECK_U(p.n_reject, 1u, "reject counted");
  CHECK_U(p.flags & APP_TXN_F_REJECTED, APP_TXN_F_REJECTED, "rejected flag");

  /* --- Wait keeps negotiating -------------------------------------- */
  APP_TXN_Init(&p);
  m[0] = (uint8_t)hdr(2u, 0u, 2u, 0u, 0u, 1u);
  m[1] = (uint8_t)(hdr(2u, 0u, 2u, 0u, 0u, 1u) >> 8);
  put32(&m[2], 0x1104B12Cu);
  APP_TXN_Feed(&p, 1u, 0u, MS(0), m, 6u);
  m[0] = (uint8_t)hdr(12u, 1u, 2u, 1u, 1u, 0u);     /* Wait, wire 0x0C */
  m[1] = (uint8_t)(hdr(12u, 1u, 2u, 1u, 1u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(2), m, 2u);
  CHECK_U(p.state, APP_TXN_NEGOTIATING, "wait stays negotiating");
  CHECK_U(p.n_wait, 1u, "wait counted");

  /* --- unmatched Accept ------------------------------------------- */
  APP_TXN_Init(&p);
  m[0] = (uint8_t)hdr(3u, 1u, 2u, 1u, 0u, 0u);
  m[1] = (uint8_t)(hdr(3u, 1u, 2u, 1u, 0u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(0), m, 2u);
  CHECK_U(p.n_unmatched, 1u, "unmatched accept detected");
  CHECK_U(p.flags & APP_TXN_F_UNMATCHED, APP_TXN_F_UNMATCHED, "unmatched flag");

  /* --- duplicate MessageID ---------------------------------------- */
  APP_TXN_Init(&p);
  m[0] = (uint8_t)hdr(2u, 1u, 2u, 1u, 4u, 0u);
  m[1] = (uint8_t)(hdr(2u, 1u, 2u, 1u, 4u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(0), m, 2u);
  APP_TXN_Feed(&p, 0u, 0u, MS(1), m, 2u);   /* same MessageID again */
  CHECK_U(p.n_dups, 1u, "duplicate MessageID detected");
  CHECK_U(p.flags & APP_TXN_F_DUP_MSGID, APP_TXN_F_DUP_MSGID, "dup flag");

  /* --- GoodCRC does not count as a duplicate ---------------------- */
  APP_TXN_Init(&p);
  m[0] = (uint8_t)hdr(1u, 1u, 2u, 1u, 4u, 0u);
  m[1] = (uint8_t)(hdr(1u, 1u, 2u, 1u, 4u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(0), m, 2u);
  APP_TXN_Feed(&p, 0u, 0u, MS(1), m, 2u);
  CHECK_U(p.n_goodcrc, 2u, "two GoodCRCs");
  CHECK_U(p.n_dups, 0u, "GoodCRC is not a duplicate");

  /* --- soft reset clears MessageID tracking ----------------------- */
  APP_TXN_Init(&p);
  APP_TXN_Feed(&p, 0u, 0u, MS(0), m, 2u);
  m[0] = (uint8_t)hdr(13u, 1u, 2u, 1u, 0u, 0u);
  m[1] = (uint8_t)(hdr(12u, 1u, 2u, 1u, 0u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(1), m, 2u);
  CHECK_U(p.n_soft_reset, 1u, "soft reset counted");
  CHECK_U(p.msg_id_valid, 0u, "MessageID tracking cleared");

  /* --- hard reset -------------------------------------------------- */
  APP_TXN_Init(&p);
  APP_TXN_NoteHardReset(&p);
  CHECK_U(p.n_hard_reset, 1u, "hard reset counted");
  CHECK_U(p.state, APP_TXN_HARD_RESET, "hard reset state");
}

static void test_txn_timeout(void)
{
  APP_TXN_Port_t p;
  uint8_t m[6];

  printf("test_txn_timeout\n");
  APP_TXN_SetClock(CORE_HZ);
  APP_TXN_Init(&p);

  /* Request with no answer at all: tSenderResponse is 30 ms */
  memset(m, 0, sizeof(m));
  m[0] = (uint8_t)hdr(2u, 0u, 2u, 0u, 0u, 1u);
  m[1] = (uint8_t)(hdr(2u, 0u, 2u, 0u, 0u, 1u) >> 8);
  put32(&m[2], 0x1104B12Cu);
  APP_TXN_Feed(&p, 1u, 0u, MS(0), m, 6u);

  APP_TXN_Poll(&p, MS(10), CORE_HZ);
  CHECK_U(p.state, APP_TXN_NEGOTIATING, "still negotiating at 10 ms");

  APP_TXN_Poll(&p, MS(50), CORE_HZ);
  CHECK_U(p.state, APP_TXN_FAILED, "no answer by 50 ms -> failed");
  CHECK_U(p.n_timeouts, 1u, "timeout counted");
  CHECK_U(p.flags & APP_TXN_F_TIMEOUT, APP_TXN_F_TIMEOUT, "timeout flag");

  /* Accept but no PS_RDY */
  APP_TXN_Init(&p);
  APP_TXN_Feed(&p, 1u, 0u, MS(0), m, 6u);
  m[0] = (uint8_t)hdr(3u, 1u, 2u, 1u, 1u, 0u);
  m[1] = (uint8_t)(hdr(2u, 1u, 2u, 1u, 1u, 0u) >> 8);
  APP_TXN_Feed(&p, 0u, 0u, MS(2), m, 2u);
  APP_TXN_Poll(&p, MS(200), CORE_HZ);
  CHECK_U(p.state, APP_TXN_TRANSITION, "still in transition at 200 ms");
  APP_TXN_Poll(&p, MS(1500), CORE_HZ);
  CHECK_U(p.state, APP_TXN_FAILED, "no PS_RDY by 1.5 s -> failed");
  CHECK_U(p.n_timeouts, 1u, "one timeout");
}

/* ================================================================== */
/* Power / energy                                                     */
/* ================================================================== */
static void test_power(void)
{
  APP_PWR_Stat_t s;
  char buf[400];
  uint32_t i;

  printf("test_power\n");
  APP_PWR_Init(&s);
  CHECK_I(APP_PWR_AvgMv(&s), 0, "no samples");

  /* 5 V at 2 A, sampled every 10 ms for exactly 1 s (100 samples). */
  for (i = 0; i < 100u; i++)
  {
    APP_PWR_Sample(&s, 10000u, 5000, 2000000);
  }
  CHECK_U(s.n, 100u, "sample count");
  CHECK_I(s.mw_last, 10000, "instantaneous power 10 W");
  CHECK_I(s.mv_min, 5000, "v min");
  CHECK_I(s.ua_max, 2000000, "current max");
  CHECK_I(APP_PWR_AvgMw(&s), 10000, "average power");

  /* E = 10 W * 1 s = 10/3600 Wh = 2777.78 uWh -> 2777 uWh truncated */
  CHECK_I(s.uwh, 2777, "energy in uWh");
  /* Q = 2 A * 1 s = 2/3600 Ah = 555.56 uAh -> 555 uAh truncated */
  CHECK_I(s.uah, 555, "charge in uAh");
  CHECK_U(s.span_us, 1000000u, "integration window");

  /* min/max across a varying load */
  APP_PWR_Init(&s);
  APP_PWR_Sample(&s, 1000u, 5000, 1000000);
  APP_PWR_Sample(&s, 1000u, 9000, 3000000);
  APP_PWR_Sample(&s, 1000u, 20000, 500000);
  CHECK_I(s.mv_min, 5000, "v min across samples");
  CHECK_I(s.mv_max, 20000, "v max across samples");
  CHECK_I(s.mw_max, 27000, "p max is 9V*3A = 27 W");
  CHECK_I(APP_PWR_AvgMv(&s), 11333, "average voltage (34000/3)");

  /* reverse flow must not corrupt the accumulator */
  APP_PWR_Init(&s);
  APP_PWR_Sample(&s, 1000000u, 5000, -1000000);
  CHECK_I(s.mw_last, -5000, "negative power");
  CHECK_I(s.uwh, -1388, "negative energy");

  /* correlation with the contracted operating point */
  APP_PWR_Init(&s);
  APP_PWR_SetContract(&s, 9000u, 3000u, 0u);
  APP_PWR_Sample(&s, 1000u, 8950, 3000000);
  APP_PWR_Sample(&s, 1000u, 9120, 3000000);
  CHECK_I(s.worst_dev_mv, 120, "worst deviation from contract");
  APP_PWR_NoteEvent(&s);
  APP_PWR_NoteEvent(&s);
  CHECK_U(s.n_events, 2u, "event count");

  /* formatting must not overrun */
  APP_PWR_Format(&s, buf, sizeof(buf));
  CHECK(strlen(buf) > 0u, "format produced text");
  CHECK(strstr(buf, "samples") != NULL, "format mentions samples");
  APP_PWR_Format(&s, buf, 8u);
  CHECK_U(buf[7], '\0', "small buffer NUL terminated");
}

int main(void)
{
  printf("=== engine host tests ===\n");
  test_cable_vdo();
  test_cable_discover_identity();
  test_cable_check();
  test_txn_happy_path();
  test_txn_failure_paths();
  test_txn_timeout();
  test_power();
  printf("=== %d passed, %d failed ===\n", s_pass, s_fail);
  return (s_fail == 0) ? 0 : 1;
}
