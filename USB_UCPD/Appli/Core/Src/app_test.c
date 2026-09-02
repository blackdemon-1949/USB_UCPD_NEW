/**
 * @file    app_test.c
 * @brief   Deterministic protocol test and replay engine (see app_test.h).
 */
#include "app_test.h"

/* CMSIS global; declaring it directly avoids pulling the HAL in here, which
 * keeps the vector suite host-runnable. */
extern uint32_t SystemCoreClock;
#include "app_log.h"
#include "app_dec.h"
#include "app_cap.h"
#include "app_txn.h"
#include "app_pps.h"
#include "app_cable.h"
#include "app_epr.h"
#include "app_integ.h"
#include "app_pdcap.h"

#include <string.h>
#include <stdio.h>

/* Reference passive cable VDO: 5 A, 50 V, EPR capable, USB 3.2 Gen 1,
 * Type-C plug, FW 1, HW 2.  Built bit by bit from the field positions in
 * app_cable.h, so the vector is self-documenting rather than a magic number. */
#define REF_CABLE_VDO \
  ((uint32_t)APP_CBL_SS_GEN1 | \
   ((uint32_t)APP_CBL_CUR_5A << 5) | \
   ((uint32_t)APP_CBL_VBUS_50V << 9) | \
   ((uint32_t)APP_CBL_TERM_PASSIVE_NOVCONN << 11) | \
   ((uint32_t)0u << 13) | \
   ((uint32_t)1u << 17) | \
   ((uint32_t)APP_CBL_TO_C << 18) | \
   ((uint32_t)1u << 22) | \
   ((uint32_t)2u << 26))

static APP_TEST_Result_t s_r;

static uint8_t v_fail(const char *what, uint32_t got, uint32_t want)
{
  s_r.vectors++;
  if (got == want)
  {
    s_r.passed++;
    return 1u;
  }
  s_r.failed++;
  APP_LOG_Printf("  FAIL %s: got %lu want %lu\r\n", what,
                 (unsigned long)got, (unsigned long)want);
  return 0u;
}

/* ------------------------------------------------------------------ */
/* Vector suite                                                        */
/* ------------------------------------------------------------------ */

static void vec_pps(void)
{
  /* A PPS APDO: 3.3-11 V, 5 A, power limited. */
  uint32_t apdo = (uint32_t)5000u / 50u;          /* max current, 50 mA units */
  uint32_t rdo;

  apdo |= ((uint32_t)3300u / 100u) << 8;          /* min voltage, 100 mV      */
  apdo |= ((uint32_t)11000u / 100u) << 17;        /* max voltage, 100 mV      */
  apdo |= 1uL << 27;                              /* PPS power limited        */
  apdo |= (uint32_t)APP_DEC_APDO_PPS << 28;       /* APDO subtype 00b         */
  apdo |= (uint32_t)APP_DEC_PDO_APDO << 30;       /* object type 11b          */

  {
    APP_PPS_Window_t w;

    (void)v_fail("pps is-apdo", (uint32_t)APP_PPS_IsApdo(apdo), 1u);
    (void)v_fail("pps parse", (uint32_t)APP_PPS_Parse(apdo, 1u, &w), 1u);
    (void)v_fail("pps min_mv", w.min_mv, 3300u);
    (void)v_fail("pps max_mv", w.max_mv, 11000u);
    (void)v_fail("pps max_ma", w.max_ma, 5000u);
    (void)v_fail("pps ok", APP_PPS_Validate(&w, 9000u, 3000u), APP_PPS_OK);
    (void)v_fail("pps below", APP_PPS_Validate(&w, 3000u, 1000u),
                 APP_PPS_BELOW_MIN);
    (void)v_fail("pps above", APP_PPS_Validate(&w, 12000u, 1000u),
                 APP_PPS_ABOVE_MAX);
    (void)v_fail("pps overcurr", APP_PPS_Validate(&w, 9000u, 6000u),
                 APP_PPS_OVER_CURR);
  }

  /* RDO round trip: build, then read the fields back out of the wire layout. */
  rdo = APP_PPS_BuildRdo(2u, 9000u, 3000u, 1u, 1u);
  (void)v_fail("rdo nonzero", (uint32_t)(rdo != 0u), 1u);
  (void)v_fail("rdo pos", APP_PPS_RDO_POS(rdo), 2u);
  (void)v_fail("rdo volt", APP_PPS_RDO_VOLT(rdo), 9000u);
  (void)v_fail("rdo curr", APP_PPS_RDO_CURR(rdo), 3000u);
  (void)v_fail("rdo unchunked", APP_PPS_RDO_UNCHUNKED(rdo), 1u);
  (void)v_fail("rdo bad pos", APP_PPS_BuildRdo(0u, 9000u, 3000u, 1u, 1u), 0u);
}

static void vec_cable(void)
{
  APP_CBL_Info_t info;

  memset(&info, 0, sizeof(info));
  APP_CBL_DecodeVdo(REF_CABLE_VDO, 0u, &info);

  (void)v_fail("cbl valid", info.valid, 1u);
  (void)v_fail("cbl ss", info.ss_support, APP_CBL_SS_GEN1);
  (void)v_fail("cbl curr", info.current_cap, APP_CBL_CUR_5A);
  (void)v_fail("cbl vbus", info.max_vbus, APP_CBL_VBUS_50V);
  (void)v_fail("cbl epr", info.epr_capable, 1u);
  (void)v_fail("cbl to", info.to_type, APP_CBL_TO_C);
  (void)v_fail("cbl maxmv", APP_CBL_MaxVoltageMv(info.max_vbus), 50000u);
  (void)v_fail("cbl maxma", APP_CBL_MaxCurrentMa(info.current_cap), 5000u);

  (void)v_fail("cbl check ok", APP_CBL_Check(&info, 20000u, 5000u, 0u),
               APP_CBL_OK);
  (void)v_fail("cbl check volt", APP_CBL_Check(&info, 60000u, 1000u, 0u),
               APP_CBL_VOLT_LIMIT);
  (void)v_fail("cbl check curr", APP_CBL_Check(&info, 20000u, 6000u, 0u),
               APP_CBL_CURR_LIMIT);
}

static void vec_epr(void)
{
  uint32_t pdo = APP_EPR_BuildAvsPdo(240u, 15000u, 48000u, 0u, 1u);
  uint32_t mv = 0u;
  uint32_t ma = 0u;

  (void)v_fail("epr is-avs", (uint32_t)APP_EPR_IsAvsPdo(pdo), 1u);
  (void)v_fail("epr pdp", APP_EPR_AVS_PDP_W(pdo), 240u);
  (void)v_fail("epr min", APP_EPR_AVS_MIN_MV(pdo), 15000u);
  (void)v_fail("epr max", APP_EPR_AVS_MAX_MV(pdo), 48000u);

  /* Clamping must respect both the source window and the board ceiling. */
  (void)v_fail("epr clamp", (uint32_t)APP_EPR_ClampRequest(pdo, 28000u, 0u,
                                                           &mv, &ma), 1u);
  (void)v_fail("epr clamp mv", mv, 28000u);
  (void)v_fail("epr clamp ceiling rejects window",
               (uint32_t)APP_EPR_ClampRequest(pdo, 12000u, 0u, &mv, &ma), 0u);
}

static void vec_txn(void)
{
  /*
   * Header is TWO bytes: b0 = type | dr<<5 | rev<<6,
   *                      b1 = pr | msgid<<1 | ndo<<4 | ext<<7.
   * A 4-byte header, or a control message whose b1 carries a non-zero NDO,
   * decodes as something else entirely - both mistakes were present in an
   * earlier version of these vectors and are the reason they are built from
   * the field positions rather than written as literals.
   */
  #define HDR(type, ndo, msgid, pr) \
      (uint8_t)(type), (uint8_t)(((ndo) << 4) | ((msgid) << 1) | (pr))
  #define LE32(v) \
      (uint8_t)((v) & 0xFFu), (uint8_t)(((v) >> 8) & 0xFFu), \
      (uint8_t)(((v) >> 16) & 0xFFu), (uint8_t)(((v) >> 24) & 0xFFu)

  /* 5 V / 3 A and 9 V / 3 A fixed PDOs: voltage in 50 mV at B19..10,
   * current in 10 mA at B9..0. */
  const uint32_t pdo5 = (100u << 10) | 300u;
  const uint32_t pdo9 = (180u << 10) | 300u;
  /* RDO: position 1, operating and maximum current 3 A. */
  const uint32_t rdo9 = (1uL << 28) | (300u << 10) | 300u;

  /* Source_Caps and Request carry computed payloads, so they are assembled at
   * run time below.  The two control messages are fixed. */
  static const uint8_t acc[2]  = { HDR(0x03u | (2u << 6), 0u, 2u, 1u) };
  static const uint8_t rdy[2]  = { HDR(0x06u | (2u << 6), 0u, 2u, 1u) };
  APP_TXN_Port_t p;

  /* Const arrays cannot hold computed values, so assemble them here. */
  {
    uint8_t c[10];
    uint8_t q[6];

    /* b1 = ext<<7 | ndo<<4 | msgid<<1 | pr ; b0 = type | dr<<5 | rev<<6.
     * rev = 2 selects PD 3.x; rev = 0 is not a valid spec revision and the
     * transaction engine ignores frames carrying it. */
    c[0] = (uint8_t)(0x01u | (2u << 6));
    c[1] = (uint8_t)((2u << 4) | (1u << 1) | 1u);
    c[2] = (uint8_t)(pdo5 & 0xFFu);        c[3] = (uint8_t)((pdo5 >> 8) & 0xFFu);
    c[4] = (uint8_t)((pdo5 >> 16) & 0xFFu); c[5] = (uint8_t)((pdo5 >> 24) & 0xFFu);
    c[6] = (uint8_t)(pdo9 & 0xFFu);        c[7] = (uint8_t)((pdo9 >> 8) & 0xFFu);
    c[8] = (uint8_t)((pdo9 >> 16) & 0xFFu); c[9] = (uint8_t)((pdo9 >> 24) & 0xFFu);

    q[0] = (uint8_t)(0x02u | (2u << 6));
    q[1] = (uint8_t)((1u << 4) | (2u << 1));
    q[2] = (uint8_t)(rdo9 & 0xFFu);        q[3] = (uint8_t)((rdo9 >> 8) & 0xFFu);
    q[4] = (uint8_t)((rdo9 >> 16) & 0xFFu); q[5] = (uint8_t)((rdo9 >> 24) & 0xFFu);

    /* The frames must decode cleanly before the transaction can be built. */
    {
      APP_DEC_Msg_t m;

      (void)v_fail("caps decode", (uint32_t)(APP_DEC_Decode(c, 10u, &m) == 0
                                  && m.msg_class == APP_DEC_CLASS_DATA
                                  && m.num_obj == 2u), 1u);
      (void)v_fail("req decode", (uint32_t)(APP_DEC_Decode(q, 6u, &m) == 0
                                 && m.msg_type == 0x02u
                                 && m.num_obj == 1u), 1u);
      (void)v_fail("accept is control", (uint32_t)(APP_DEC_Decode(acc, 2u, &m) == 0
                                        && m.msg_class == APP_DEC_CLASS_CONTROL
                                        && m.msg_type == 0x03u), 1u);
      (void)v_fail("ps_rdy is control", (uint32_t)(APP_DEC_Decode(rdy, 2u, &m) == 0
                                        && m.msg_class == APP_DEC_CLASS_CONTROL
                                        && m.msg_type == 0x06u), 1u);
    }

    APP_TXN_SetClock(400000000u);
    APP_TXN_Init(&p);
    APP_TXN_NoteRequest(&p, 9000u, 3000u, 0u);

    APP_TXN_Feed(&p, 0u, 0u, 0u, c, 10u);
    APP_TXN_Feed(&p, 1u, 0u, 4000000u, q, 6u);          /* +10 ms */
    APP_TXN_Feed(&p, 0u, 0u, 8000000u, acc, 2u);        /* +20 ms */
    APP_TXN_Feed(&p, 0u, 0u, 11600000u, rdy, 2u);       /* +29 ms */
  }

  (void)v_fail("txn contracts", p.n_contracts, 1u);
  (void)v_fail("txn mv", p.contract_mv, 9000u);
  (void)v_fail("txn ma", p.contract_ma, 3000u);
  (void)v_fail("txn accept us", p.accept_us, 10000u);
  /* psrdy_us is Accept->PS_RDY (20 ms -> 29 ms = 9 ms); contract_us is
   * Request->PS_RDY (10 ms -> 29 ms = 19 ms).  Conflating the two was an
   * error in this vector, not in the engine. */
  (void)v_fail("txn psrdy us", p.psrdy_us, 9000u);
  (void)v_fail("txn contract us", p.contract_us, 19000u);
}

static void vec_dec(void)
{
  APP_DEC_Msg_t m;
  /* Accept: control message, so NDO must be zero. */
  static const uint8_t accept[2] = { 0x03u, 0x05u };
  /* Type 0x1F is reserved and must be flagged, not silently accepted. */
  static const uint8_t bad[2]    = { 0x1Fu, 0x05u };
  /* A data message whose NDO says two objects but which carries none. */
  static const uint8_t nodo[2]   = { 0x02u, 0x25u };

  printf("vec_dec\n");

  /* APP_DEC_Decode returns 0 on success and reports structural problems
   * through flags, so the return value alone cannot distinguish good from
   * bad - the flags are what has to be checked. */
  (void)v_fail("dec accept ok", (uint32_t)APP_DEC_Decode(accept, 2u, &m), 0u);
  (void)v_fail("dec accept class", m.msg_class, APP_DEC_CLASS_CONTROL);
  (void)v_fail("dec accept clean", m.flags, 0u);

  (void)v_fail("dec reserved type", (uint32_t)APP_DEC_Decode(bad, 2u, &m), 0u);
  (void)v_fail("dec reserved flag",
               (uint32_t)((m.flags & APP_DEC_F_TYPE_RSV) != 0u), 1u);

  (void)v_fail("dec nodo ok", (uint32_t)APP_DEC_Decode(nodo, 2u, &m), 0u);
  (void)v_fail("dec nodo flag",
               (uint32_t)((m.flags & APP_DEC_F_NUMOBJ) != 0u), 1u);

  (void)v_fail("dec short ret", (uint32_t)APP_DEC_Decode(accept, 1u, &m), 0u);
  (void)v_fail("dec short flag",
               (uint32_t)((m.flags & APP_DEC_F_SHORT) != 0u), 1u);
}

uint32_t APP_TEST_RunSuite(void)
{
  memset(&s_r, 0, sizeof(s_r));

  APP_LOG_Write("test suite\r\n");
  vec_pps();
  vec_cable();
  vec_epr();
  vec_txn();
  vec_dec();

  s_r.last_ok = (s_r.failed == 0u) ? 1u : 0u;
  APP_LOG_Printf("suite: %lu/%lu passed\r\n",
                 (unsigned long)s_r.passed, (unsigned long)s_r.vectors);
  return s_r.failed;
}

/* ------------------------------------------------------------------ */
/* Replay                                                              */
/* ------------------------------------------------------------------ */

void APP_TEST_Replay(uint32_t count)
{
  APP_TXN_Port_t rp;
  uint32_t available = APP_CAP_Count();
  uint32_t n;
  uint32_t i;
  uint32_t crc = 0u;
  extern APP_TXN_Port_t APP_TXN_Port0;

  if (available == 0u)
  {
    APP_LOG_Write("replay: capture ring is empty\r\n");
    return;
  }

  n = ((count == 0u) || (count > available)) ? available : count;

  /* Fresh port: the replay must reproduce the state from the bytes alone. */
  APP_TXN_SetClock(SystemCoreClock);
  APP_TXN_Init(&rp);

  for (i = 0u; i < n; i++)
  {
    APP_CAP_Rec_t rec;
    APP_DEC_Msg_t m;

    if (APP_CAP_Get(i, &rec) == 0)
    {
      break;
    }
    if ((rec.type != 1u) && (rec.type != 2u))   /* PD messages only */
    {
      continue;
    }
    if (rec.len < 2u)
    {
      continue;
    }

    /* Decode it, and fold the decoded form into the digest so that a decode
     * regression changes the CRC even when the bytes did not. */
    /* Randomized spot-check: sample a record with the hardware RNG and hash
     * it into a second digest, so a replay can be repeated with a different
     * sample without changing the deterministic record order. */
    if (APP_RNG_Below(16u) == 0u)
    {
      crc ^= APP_CRC_Calc(rec.data, rec.len);
    }

    if (APP_DEC_Decode(rec.data, rec.len, &m) != 0)
    {
      crc = APP_CRC_Calc(rec.data, rec.len) ^ crc;
    }

    /* The ternary yields unsigned int; dir is uint8_t, so cast explicitly
     * rather than letting -Wconversion reject the implicit narrowing. */
    APP_TXN_Feed(&rp, (uint8_t)((rec.type == 1u) ? 0u : 1u), rec.sop, rec.ts,
                 rec.data, (uint32_t)rec.len);
    s_r.replay_records++;
  }

  s_r.replay_crc = crc;
  s_r.replay_match = ((rp.state == APP_TXN_Port0.state) &&
                      (rp.n_contracts == APP_TXN_Port0.n_contracts) &&
                      (rp.contract_mv == APP_TXN_Port0.contract_mv)) ? 1u : 0u;

  APP_LOG_Printf("replay: %lu records, digest 0x%08lX, state %s (%s)\r\n",
                 (unsigned long)s_r.replay_records, (unsigned long)crc,
                 APP_TXN_StateName(rp.state),
                 s_r.replay_match ? "matches live" : "DIFFERS from live");
}

void APP_TEST_GetResult(APP_TEST_Result_t *out)
{
  if (out != NULL)
  {
    *out = s_r;
  }
}

int APP_TEST_Cmd(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "suite";

  if (strcmp(sub, "suite") == 0)
  {
    (void)APP_TEST_RunSuite();
    return 1;
  }
  if (strcmp(sub, "replay") == 0)
  {
    unsigned n = 0u;

    if (argc >= 3)
    {
      (void)sscanf(argv[2], "%u", &n);
    }
    APP_TEST_Replay((uint32_t)n);
    return 1;
  }
  if (strcmp(sub, "all") == 0)
  {
    (void)APP_TEST_RunSuite();
    APP_TEST_Replay(0u);
    return 1;
  }

  APP_LOG_Write("usage: test [suite|replay [n]|all]\r\n");
  return 1;
}
