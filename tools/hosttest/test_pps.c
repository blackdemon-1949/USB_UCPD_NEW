/*
 * Host tests for the PPS engine (pure logic) and the EPR AVS helpers.
 *
 * Built by tools/hosttest/run.py under ASan/UBSan.  Only the pure parts are
 * exercised here: app_pps.c and app_epr.c have no hardware dependency in the
 * functions under test, so the same code the firmware runs is what runs here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "app_pps.h"
#include "app_epr.h"
#include "app_cable.h"
#include "app_dec.h"

/* provided by log_stub.c */
const char *log_stub_text(void);
void log_stub_reset(void);

static int s_pass;
static int s_fail;

#define CHECK(expr) do {                                              \
    if (expr) { s_pass++; }                                           \
    else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); } \
  } while (0)

#define CHECK_EQ(got, want) do {                                      \
    unsigned long _g = (unsigned long)(got), _w = (unsigned long)(want); \
    if (_g == _w) { s_pass++; }                                       \
    else { s_fail++; printf("  FAIL %s:%d  %s: got %lu want %lu\n",   \
                            __FILE__, __LINE__, #got, _g, _w); }      \
  } while (0)

/* 3.3-11 V, 5 A, power limited */
static uint32_t apdo_pps(void)
{
  uint32_t p = 5000u / 50u;             /* max current, 50 mA units  */
  p |= (3300u / 100u) << 8;             /* min voltage, 100 mV units */
  p |= (11000u / 100u) << 17;           /* max voltage, 100 mV units */
  p |= 1uL << 27;                       /* PPS power limited         */
  p |= (uint32_t)APP_DEC_APDO_PPS << 28;
  p |= (uint32_t)APP_DEC_PDO_APDO << 30;
  return p;
}

/* 5-9 V, 3 A, not power limited */
static uint32_t apdo_pps_low(void)
{
  uint32_t p = 3000u / 50u;
  p |= (5000u / 100u) << 8;
  p |= (9000u / 100u) << 17;
  p |= (uint32_t)APP_DEC_APDO_PPS << 28;
  p |= (uint32_t)APP_DEC_PDO_APDO << 30;
  return p;
}

/* A plain fixed 9 V / 3 A PDO: object type 00b. */
static uint32_t pdo_fixed_9v(void)
{
  uint32_t p = (3000u / 10u);           /* 10 mA units */
  p |= (9000u / 50u) << 10;             /* 50 mV units */
  return p;                             /* B31..30 = 00b fixed */
}

static void test_pps_parse(void)
{
  APP_PPS_Window_t w;

  printf("test_pps_parse\n");

  CHECK_EQ(APP_PPS_IsApdo(apdo_pps()), 1);
  CHECK_EQ(APP_PPS_IsApdo(apdo_pps_low()), 1);
  CHECK_EQ(APP_PPS_IsApdo(pdo_fixed_9v()), 0);

  memset(&w, 0xAA, sizeof(w));
  CHECK_EQ(APP_PPS_Parse(pdo_fixed_9v(), 1u, &w), 0);
  /* A rejected parse must not have touched the caller's buffer. */
  CHECK_EQ(w.pos, 0xAAAAu & 0xFFu);

  memset(&w, 0, sizeof(w));
  CHECK_EQ(APP_PPS_Parse(apdo_pps(), 3u, &w), 1);
  CHECK_EQ(w.pos, 3);
  CHECK_EQ(w.min_mv, 3300u);
  CHECK_EQ(w.max_mv, 11000u);
  CHECK_EQ(w.max_ma, 5000u);
  CHECK_EQ(w.power_limited, 1);

  /* Position 0 is not addressable in a PD Request. */
  CHECK_EQ(APP_PPS_Parse(apdo_pps(), 0u, &w), 0);
  CHECK_EQ(APP_PPS_Parse(apdo_pps(), 1u, NULL), 0);
}

static void test_pps_validate(void)
{
  APP_PPS_Window_t w;

  printf("test_pps_validate\n");

  CHECK_EQ(APP_PPS_Parse(apdo_pps(), 1u, &w), 1);

  CHECK_EQ(APP_PPS_Validate(&w, 9000u, 3000u), APP_PPS_OK);
  CHECK_EQ(APP_PPS_Validate(&w, 3300u, 1000u), APP_PPS_OK);   /* floor is inclusive */
  CHECK_EQ(APP_PPS_Validate(&w, 11000u, 1000u), APP_PPS_OK);  /* ceiling inclusive  */
  CHECK_EQ(APP_PPS_Validate(&w, 3200u, 1000u), APP_PPS_BELOW_MIN);
  CHECK_EQ(APP_PPS_Validate(&w, 11100u, 1000u), APP_PPS_ABOVE_MAX);
  CHECK_EQ(APP_PPS_Validate(&w, 9000u, 5100u), APP_PPS_OVER_CURR);
  CHECK_EQ(APP_PPS_Validate(NULL, 9000u, 1000u), APP_PPS_NO_WINDOW);
}

static void test_pps_analyse(void)
{
  uint32_t list[7];
  APP_PPS_Set_t s;

  printf("test_pps_analyse\n");

  list[0] = pdo_fixed_9v();
  list[1] = apdo_pps_low();
  list[2] = apdo_pps();
  APP_PPS_Analyse(list, 3u, &s);

  CHECK_EQ(s.n, 2);                        /* the fixed PDO is not a window */
  CHECK_EQ(s.w[0].pos, 2);                 /* positions are 1-based in the list */
  CHECK_EQ(s.w[1].pos, 3);
  /* Two windows: 5-9 V and 3.3-11 V.  The reachable span is the union of
   * both, so its floor is 3.3 V, not the higher window's floor. */
  CHECK_EQ(s.span_min_mv, 3300u);
  CHECK_EQ(s.span_max_mv, 11000u);
  CHECK_EQ(s.span_max_ma, 5000u);
  CHECK_EQ(s.max_pdp_mw, 11000u * 5u);     /* 11 V * 5 A = 55 W */

  /* Degenerate inputs must not crash and must yield an empty set. */
  APP_PPS_Analyse(NULL, 0u, &s);
  CHECK_EQ(s.n, 0);
  APP_PPS_Analyse(list, 0u, &s);
  CHECK_EQ(s.n, 0);

  /* A malformed APDO whose ceiling is below its floor must be dropped. */
  {
    uint32_t bad = (11000u / 100u) << 8;   /* min = 11 V */
    bad |= (3300u / 100u) << 17;           /* max = 3.3 V */
    bad |= (uint32_t)APP_DEC_APDO_PPS << 28;
    bad |= (uint32_t)APP_DEC_PDO_APDO << 30;
    list[0] = bad;
    APP_PPS_Analyse(list, 1u, &s);
    CHECK_EQ(s.n, 0);
  }
}

static void test_pps_rdo(void)
{
  uint32_t rdo;

  printf("test_pps_rdo\n");

  rdo = APP_PPS_BuildRdo(2u, 9000u, 3000u, 1u, 1u);
  CHECK(rdo != 0u);
  CHECK_EQ(APP_PPS_RDO_POS(rdo), 2);
  CHECK_EQ(APP_PPS_RDO_VOLT(rdo), 9000u);
  CHECK_EQ(APP_PPS_RDO_CURR(rdo), 3000u);
  CHECK_EQ(APP_PPS_RDO_UNCHUNKED(rdo), 1);
  CHECK_EQ(APP_PPS_RDO_USB_COMM(rdo), 1);

  /* Clearing the flags must clear the bits. */
  rdo = APP_PPS_BuildRdo(1u, 5000u, 1000u, 0u, 0u);
  CHECK_EQ(APP_PPS_RDO_UNCHUNKED(rdo), 0);
  CHECK_EQ(APP_PPS_RDO_USB_COMM(rdo), 0);
  CHECK_EQ(APP_PPS_RDO_VOLT(rdo), 5000u);

  /* Rejects out-of-range fields rather than emitting a wrong RDO. */
  CHECK_EQ(APP_PPS_BuildRdo(0u, 9000u, 3000u, 1u, 1u), 0);
  CHECK_EQ(APP_PPS_BuildRdo(16u, 9000u, 3000u, 1u, 1u), 0);
  CHECK_EQ(APP_PPS_BuildRdo(1u, 0u, 3000u, 1u, 1u), 0);
  CHECK_EQ(APP_PPS_BuildRdo(1u, 9000u, 0u, 1u, 1u), 0);
  CHECK_EQ(APP_PPS_BuildRdo(1u, 20u * 0x1000u, 3000u, 1u, 1u), 0);
  CHECK_EQ(APP_PPS_BuildRdo(1u, 9000u, 50u * 0x80u, 1u, 1u), 0);
}

static void test_epr_avs(void)
{
  uint32_t pdo;
  uint32_t mv = 0u;
  uint32_t ma = 0u;

  printf("test_epr_avs\n");

  pdo = APP_EPR_BuildAvsPdo(240u, 15000u, 48000u, 0u, 1u);
  CHECK_EQ(APP_EPR_IsAvsPdo(pdo), 1);
  CHECK_EQ(APP_EPR_AVS_PDP_W(pdo), 240u);
  CHECK_EQ(APP_EPR_AVS_MIN_MV(pdo), 15000u);
  CHECK_EQ(APP_EPR_AVS_MAX_MV(pdo), 48000u);
  CHECK_EQ(APP_EPR_AVS_OBJ(pdo), APP_EPR_AVS_OBJ_AVSPDO);
  CHECK_EQ(APP_EPR_AVS_KIND(pdo), APP_EPR_AVS_KIND_AVS);

  /* A PPS APDO is object type 11b but subtype 00b, so it is not an AVS PDO. */
  CHECK_EQ(APP_EPR_IsAvsPdo(apdo_pps()), 0);

  /* want = 0 means "as high as allowed". */
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 28000u, 0u, &mv, &ma), 1);
  CHECK_EQ(mv, 28000u);
  CHECK(ma > 0u);
  CHECK_EQ(ma % 50u, 0u);              /* 50 mA steps */
  CHECK(ma <= 5000u);

  /* An explicit target inside the window is honoured, stepped to 100 mV. */
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 48000u, 20150u, &mv, &ma), 1);
  CHECK_EQ(mv, 20100u);

  /* A target below the window floor is raised to the floor. */
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 48000u, 5000u, &mv, &ma), 1);
  CHECK_EQ(mv, 15000u);

  /* A ceiling below the window floor excludes the whole window. */
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 12000u, 0u, &mv, &ma), 0);

  CHECK_EQ(APP_EPR_ClampRequest(apdo_pps(), 28000u, 0u, &mv, &ma), 0);
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 28000u, 0u, NULL, &ma), 0);
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 28000u, 0u, &mv, NULL), 0);
}


/* ---- EPR discovery: the 5 V Fixed PDO EPR Mode Capable bit (PD3.1 B23) ----
 * This is the regression test for the bug that made EPR unreachable: the
 * sink used to learn "source is EPR capable" only from EPR AVS PDOs, which
 * arrive only AFTER EPR mode entry, which itself required the capability to
 * be known.  Discovery must come from the ordinary SPR Source_Capabilities. */
static void test_epr_discovery(void)
{
  uint32_t caps[2];

  printf("test_epr_discovery\n");

  APP_EPR_Init();

  /* Plain 5 V/3 A Fixed PDO, EPR bit clear -> SPR-only source. */
  caps[0] = (100u << 10) | (300u << 0);      /* Fixed, 5 V, 3 A */
  caps[1] = 0u;
  APP_EPR_OnSprSrcCaps(caps, 1u);
  CHECK_EQ(APP_EPR_Ctx.src_spr_epr_capable, 0u);
  CHECK_EQ(APP_EPR_ShouldRequest(), 0u);     /* RDO B21 must stay clear */

  /* Same PDO with B23 set -> EPR-capable source. */
  caps[0] |= APP_EPR_SRC_FIXED_EPR_CAPABLE;
  APP_EPR_OnSprSrcCaps(caps, 1u);
  CHECK_EQ(APP_EPR_Ctx.src_spr_epr_capable, 1u);
  CHECK_EQ(APP_EPR_ShouldRequest(), 1u);     /* RDO B21 must now be set */

  /* 'enable' controls AUTOMATIC entry only.  Turning it off must NOT clear
   * the advertised EPR capability, otherwise a manual 'epr enter' would be
   * rejected by the source with "EPR bit not set in RDO" (reason 0x03). */
  APP_EPR_Ctx.enable = 0u;
  CHECK_EQ(APP_EPR_ShouldRequest(), 1u);
  APP_EPR_Ctx.enable = 1u;

  /* 'allow' is the switch that really suppresses the advertisement. */
  APP_EPR_Ctx.allow = 0u;
  CHECK_EQ(APP_EPR_ShouldRequest(), 0u);
  APP_EPR_Ctx.allow = 1u;

  /* The bit is only defined in the FIRST PDO, and only for a Fixed Supply.
   * An APDO in position 1 must never be read as an EPR advertisement. */
  caps[0] = apdo_pps();
  APP_EPR_OnSprSrcCaps(caps, 1u);
  CHECK_EQ(APP_EPR_Ctx.src_spr_epr_capable, 0u);

  /* Defensive: empty / NULL capability lists must not assert capability. */
  APP_EPR_OnSprSrcCaps(NULL, 0u);
  CHECK_EQ(APP_EPR_Ctx.src_spr_epr_capable, 0u);
  APP_EPR_OnSprSrcCaps(caps, 0u);
  CHECK_EQ(APP_EPR_Ctx.src_spr_epr_capable, 0u);
}

/* EPR power maths must not overflow or truncate at the 240 W / 5 A limits. */
static void test_epr_power_math(void)
{
  uint32_t pdo;
  uint32_t mv = 0u, ma = 0u;

  printf("test_epr_power_math\n");

  /* 240 W, 15-48 V.  At 48 V, 240 W would be 5.0 A exactly. */
  pdo = APP_EPR_BuildAvsPdo(240u, 15000u, 48000u, 0u, 1u);
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 48000u, 48000u, &mv, &ma), 1);
  CHECK_EQ(mv, 48000u);
  CHECK_EQ(ma, 5000u);

  /* At 15 V, 240 W would be 16 A -> must clamp to the 5 A connector limit. */
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 48000u, 15000u, &mv, &ma), 1);
  CHECK_EQ(mv, 15000u);
  CHECK_EQ(ma, 5000u);

  /* 100 W at 28 V = 3.571 A -> 3550 mA after 50 mA rounding, no overflow. */
  pdo = APP_EPR_BuildAvsPdo(100u, 15000u, 28000u, 0u, 1u);
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 28000u, 28000u, &mv, &ma), 1);
  CHECK_EQ(mv, 28000u);
  CHECK_EQ(ma, 3550u);
  CHECK(ma * mv / 1000u <= 100u * 1000u);   /* never exceeds advertised PDP */

  /* A zero-PDP advertisement must yield zero current, not a divide fault. */
  pdo = APP_EPR_BuildAvsPdo(0u, 15000u, 28000u, 0u, 1u);
  CHECK_EQ(APP_EPR_ClampRequest(pdo, 28000u, 28000u, &mv, &ma), 1);
  CHECK_EQ(ma, 0u);
}


/* The CLI must never claim a protocol operation happened.  It reports the
 * real API status string returned by the stack. */
static void test_epr_cli_honesty(void)
{
  char *argv[4];
  uint32_t caps[1];

  printf("test_epr_cli_honesty\n");
  APP_EPR_Init();

  /* Status names must map to the real USBPD_StatusTypeDef enum values. */
  CHECK(strstr(APP_EPR_StatusName(0), "USBPD_OK") != NULL);
  CHECK(strstr(APP_EPR_StatusName(3), "USBPD_BUSY") != NULL);

  /* Against an SPR-only source, 'epr status' must NOT claim EPR is available. */
  caps[0] = (100u << 10) | (300u << 0);
  APP_EPR_OnSprSrcCaps(caps, 1u);
  log_stub_reset();
  argv[0] = (char *)"epr"; argv[1] = (char *)"status";
  CHECK_EQ(APP_EPR_Cmd(2, argv), 1);
  CHECK(strstr(log_stub_text(), "EPR unavailable on this session") != NULL);
  CHECK(strstr(log_stub_text(), "clear (SPR-only source)") != NULL);

  /* With an EPR-capable source the verdict changes, but must still not claim
   * an EPR contract exists before one has been negotiated. */
  caps[0] |= APP_EPR_SRC_FIXED_EPR_CAPABLE;
  APP_EPR_OnSprSrcCaps(caps, 1u);
  log_stub_reset();
  CHECK_EQ(APP_EPR_Cmd(2, argv), 1);
  CHECK(strstr(log_stub_text(), "EPR available, not entered") != NULL);
  CHECK(strstr(log_stub_text(), "EPR contract active") == NULL);
}

/* Regressions for the three defects the real bench run exposed. */
static void test_epr_hw_regressions(void)
{
  uint32_t caps[1];
  char *argv[4];

  printf("test_epr_hw_regressions\n");
  APP_EPR_Init();

  /* (1) 'epr' must not report a status for a call that never happened.
   *     USBPD_OK is 0, so an untouched field used to read as success. */
  log_stub_reset();
  argv[0] = (char *)"epr"; argv[1] = (char *)"status";
  CHECK_EQ(APP_EPR_Cmd(2, argv), 1);
  CHECK(strstr(log_stub_text(), "last Enter status  : not attempted") != NULL);
  CHECK(strstr(log_stub_text(), "last GetSrcCap st  : not attempted") != NULL);
  /* and must not claim an EPR_Mode exchange that never occurred */
  CHECK(strstr(log_stub_text(), "none (no EPR_Mode message exchanged)") != NULL);

  /* (2) Sink Operational PDP must reflect the cable, not assume 5 A.
   *     28 V x 3 A = 84 W with a non-e-marked cable, not 140 W. */
  APP_EPR_Ctx.ceiling_mv = 28000u;
  APP_EPR_Ctx.cable_5a = 0u;
  CHECK_EQ(APP_EPR_GetSinkPdpW(), 84u);
  APP_EPR_Ctx.cable_5a = 1u;
  CHECK_EQ(APP_EPR_GetSinkPdpW(), 140u);
  /* never exceed the 240 W EPR maximum */
  APP_EPR_Ctx.ceiling_mv = 48000u;
  CHECK_EQ(APP_EPR_GetSinkPdpW(), 240u);
  APP_EPR_Ctx.ceiling_mv = 28000u;

  /* (3) An "Enter Failed" reply must be decoded, with its reason code, and
   *     must not leave the engine believing EPR is active. */
  {
    /* Action 0x04 = Enter Failed, Data 0x01 = cable not EPR capable */
    uint8_t mdo[4];
    uint32_t d32 = (0x04u << 24) | (0x01u << 16);
    mdo[0] = (uint8_t)(d32 & 0xFFu);
    mdo[1] = (uint8_t)((d32 >> 8) & 0xFFu);
    mdo[2] = (uint8_t)((d32 >> 16) & 0xFFu);
    mdo[3] = (uint8_t)((d32 >> 24) & 0xFFu);

    log_stub_reset();
    APP_EPR_OnModeDo(mdo, 4u);
    CHECK_EQ(APP_EPR_Ctx.mode, 0u);
    CHECK_EQ(APP_EPR_Ctx.error_valid, 1u);
    CHECK_EQ(APP_EPR_Ctx.last_error, 0x01u);
    CHECK(strstr(log_stub_text(), "cable not EPR capable") != NULL);

    /* Enter Succeeded must set EPR mode. */
    d32 = (0x03u << 24);
    mdo[0] = (uint8_t)(d32 & 0xFFu);
    mdo[1] = (uint8_t)((d32 >> 8) & 0xFFu);
    mdo[2] = (uint8_t)((d32 >> 16) & 0xFFu);
    mdo[3] = (uint8_t)((d32 >> 24) & 0xFFu);
    APP_EPR_OnModeDo(mdo, 4u);
    CHECK_EQ(APP_EPR_Ctx.mode, 1u);
    CHECK_EQ(APP_EPR_Ctx.entered, 1u);

    /* Malformed / short objects must be ignored, not misparsed. */
    APP_EPR_OnModeDo(NULL, 4u);
    APP_EPR_OnModeDo(mdo, 2u);
    CHECK_EQ(APP_EPR_Ctx.mode, 1u);
  }

  /* Source advertising EPR in the 5 V PDO still drives RDO B21. */
  APP_EPR_Init();
  caps[0] = (100u << 10) | (300u << 0) | APP_EPR_SRC_FIXED_EPR_CAPABLE;
  APP_EPR_OnSprSrcCaps(caps, 1u);
  CHECK_EQ(APP_EPR_ShouldRequest(), 1u);
}

static void test_cable(void)
{
  APP_CBL_Info_t info;
  /* Passive, 5 A, 50 V, EPR capable, Gen 1, Type-C, FW 1, HW 2 */
  uint32_t vdo = (uint32_t)APP_CBL_SS_GEN1
               | ((uint32_t)APP_CBL_CUR_5A << 5)
               | ((uint32_t)APP_CBL_VBUS_50V << 9)
               | ((uint32_t)APP_CBL_TERM_PASSIVE_NOVCONN << 11)
               | ((uint32_t)1u << 17)
               | ((uint32_t)APP_CBL_TO_C << 18)
               | ((uint32_t)1u << 22)
               | ((uint32_t)2u << 26);

  printf("test_cable\n");

  memset(&info, 0, sizeof(info));
  APP_CBL_DecodeVdo(vdo, 0u, &info);

  CHECK_EQ(info.valid, 1);
  CHECK_EQ(info.ss_support, APP_CBL_SS_GEN1);
  CHECK_EQ(info.current_cap, APP_CBL_CUR_5A);
  CHECK_EQ(info.max_vbus, APP_CBL_VBUS_50V);
  CHECK_EQ(info.epr_capable, 1);
  CHECK_EQ(info.to_type, APP_CBL_TO_C);
  CHECK_EQ(APP_CBL_MaxVoltageMv(info.max_vbus), 50000u);
  CHECK_EQ(APP_CBL_MaxCurrentMa(info.current_cap), 5000u);

  CHECK_EQ(APP_CBL_Check(&info, 20000u, 5000u, 0u), APP_CBL_OK);
  CHECK_EQ(APP_CBL_Check(&info, 20000u, 5000u, 1u), APP_CBL_OK);
  CHECK_EQ(APP_CBL_Check(&info, 60000u, 1000u, 0u), APP_CBL_VOLT_LIMIT);
  CHECK_EQ(APP_CBL_Check(&info, 20000u, 6000u, 0u), APP_CBL_CURR_LIMIT);

  /* A 20 V / 3 A cable cannot do EPR. */
  {
    uint32_t v2 = (uint32_t)APP_CBL_SS_USB2
                | ((uint32_t)APP_CBL_CUR_3A << 5)
                | ((uint32_t)APP_CBL_VBUS_20V << 9);
    APP_CBL_Info_t i2;

    memset(&i2, 0, sizeof(i2));
    APP_CBL_DecodeVdo(v2, 0u, &i2);
    CHECK_EQ(i2.epr_capable, 0);
    CHECK_EQ(APP_CBL_Check(&i2, 20000u, 3000u, 1u), APP_CBL_NOT_EPR);
    CHECK_EQ(APP_CBL_Check(&i2, 20000u, 3000u, 0u), APP_CBL_OK);
  }
}

/* The engines' CLI entry points are covered too, through the log stub, so the
 * formatting code that only runs on the target is exercised here. */
extern const char *log_stub_text(void);
extern void log_stub_reset(void);

static void test_cli_paths(void)
{
  char *argv[6];
  APP_PPS_Set_t s;
  uint32_t list[3];

  printf("test_cli_paths\n");

  /* Seed the PPS engine through its real ingest path. */
  list[0] = pdo_fixed_9v();
  list[1] = apdo_pps_low();
  list[2] = apdo_pps();
  {
    uint8_t raw[12];
    int i;
    for (i = 0; i < 3; i++)
    {
      raw[i * 4 + 0] = (uint8_t)(list[i] & 0xFFu);
      raw[i * 4 + 1] = (uint8_t)((list[i] >> 8) & 0xFFu);
      raw[i * 4 + 2] = (uint8_t)((list[i] >> 16) & 0xFFu);
      raw[i * 4 + 3] = (uint8_t)((list[i] >> 24) & 0xFFu);
    }
    APP_PPS_OnSrcPdo(raw, 12u);
  }
  s = *APP_PPS_Get();
  CHECK_EQ(s.n, 2);

  log_stub_reset();
  argv[0] = (char *)"pps"; argv[1] = (char *)"status";
  CHECK_EQ(APP_PPS_Cmd(2, argv), 1);
  CHECK(strstr(log_stub_text(), "3.3-11.0V") != NULL);
  CHECK(strstr(log_stub_text(), "power-limited") != NULL);

  log_stub_reset();
  argv[1] = (char *)"check"; argv[2] = (char *)"9000"; argv[3] = (char *)"3000";
  CHECK_EQ(APP_PPS_Cmd(4, argv), 1);
  CHECK(strstr(log_stub_text(), "ok") != NULL);

  log_stub_reset();
  argv[2] = (char *)"99000"; argv[3] = (char *)"3000";
  CHECK_EQ(APP_PPS_Cmd(4, argv), 1);
  CHECK(strstr(log_stub_text(), "above window maximum") != NULL);

  log_stub_reset();
  argv[1] = (char *)"rdo"; argv[2] = (char *)"2"; argv[3] = (char *)"9000";
  argv[4] = (char *)"3000";
  CHECK_EQ(APP_PPS_Cmd(5, argv), 1);
  CHECK(strstr(log_stub_text(), "0x") != NULL);

  log_stub_reset();
  argv[1] = (char *)"bogus";
  CHECK_EQ(APP_PPS_Cmd(2, argv), 1);
  CHECK(strstr(log_stub_text(), "usage:") != NULL);

  /* EPR CLI */
  APP_EPR_Init();
  log_stub_reset();
  argv[0] = (char *)"epr"; argv[1] = (char *)"ceiling"; argv[2] = (char *)"36000";
  CHECK_EQ(APP_EPR_Cmd(3, argv), 1);
  CHECK_EQ(APP_EPR_Ctx.ceiling_mv, 36000u);

  log_stub_reset();
  argv[2] = (char *)"99999";       /* above APP_EPR_MAX_MV: must be rejected */
  CHECK_EQ(APP_EPR_Cmd(3, argv), 1);
  CHECK_EQ(APP_EPR_Ctx.ceiling_mv, 36000u);

  log_stub_reset();
  argv[1] = (char *)"status";
  CHECK_EQ(APP_EPR_Cmd(2, argv), 1);
  CHECK(strstr(log_stub_text(), "EPR") != NULL);

  /* EPR notifications drive the recorded mode. */
  APP_EPR_OnNotify(116u);          /* USBPD_NOTIFY_EPRMODE_INIT      */
  CHECK_EQ(APP_EPR_Ctx.n_enter, 1u);
  APP_EPR_OnNotify(114u);          /* USBPD_NOTIFY_EPRMODE_SUCCEEDED */
  CHECK_EQ(APP_EPR_Ctx.mode, 1u);
  CHECK_EQ(APP_EPR_Ctx.entered, 1u);
  APP_EPR_OnNotify(117u);          /* USBPD_NOTIFY_EPRMODE_EXIT      */
  CHECK_EQ(APP_EPR_Ctx.mode, 0u);
  CHECK_EQ(APP_EPR_Ctx.n_exit, 1u);
  APP_EPR_OnNotify(115u);          /* USBPD_NOTIFY_EPRMODE_FAILED    */
  CHECK_EQ(APP_EPR_Ctx.n_failed, 1u);
  APP_EPR_OnNotify(999u);          /* unknown: must be ignored       */
}

int main(void)
{
  printf("=== pps / epr / cable host tests ===\n");
  test_pps_parse();
  test_pps_validate();
  test_pps_analyse();
  test_pps_rdo();
  test_epr_avs();
  test_epr_discovery();
  test_epr_power_math();
  test_epr_cli_honesty();
  test_epr_hw_regressions();
  test_cable();
  test_cli_paths();
  printf("=== %d passed, %d failed ===\n", s_pass, s_fail);
  return (s_fail == 0) ? 0 : 1;
}
