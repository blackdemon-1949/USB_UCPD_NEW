/**
 * @file    test_app_dec.c
 * @brief   Host-side (x86) unit tests for the USB PD decoder.
 *
 * Compiled and run on the development machine - NOT on the target - so the
 * protocol logic is exercised against known-good vectors without flashing.
 * Build/run: python3 tools/hosttest/run.py
 *
 * Vectors come from real captures and from the USB PD 3.1 data-object
 * layouts, cross-checked against the bitfield structures in ST's usbpd_def.h.
 */
#include "app_dec.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
static int s_pass;

#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    if (cond) { s_pass++; }                                                \
    else { s_fail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__);         \
           printf(__VA_ARGS__); printf("\n"); }                            \
  } while (0)

#define CHECK_U(got, want, what)                                            \
  do {                                                                      \
    unsigned long _g = (unsigned long)(got), _w = (unsigned long)(want);    \
    if (_g == _w) { s_pass++; }                                             \
    else { s_fail++; printf("  FAIL %s:%d  %s: got %lu want %lu\n",         \
                            __FILE__, __LINE__, (what), _g, _w); }          \
  } while (0)

#define CHECK_S(got, want, what)                                            \
  do {                                                                      \
    if (strcmp((got), (want)) == 0) { s_pass++; }                           \
    else { s_fail++; printf("  FAIL %s:%d  %s: got \"%s\" want \"%s\"\n",   \
                            __FILE__, __LINE__, (what), (got), (want)); }   \
  } while (0)

static uint32_t rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* 1. Message header field extraction                                 */
/* ------------------------------------------------------------------ */
static void test_header_fields(void)
{
  /* Real captured Source_Capabilities header: wire bytes A1 61 -> 0x61A1.
   * Source, DFP, PD 3.0, MessageID 0, 6 data objects, not extended.       */
  const uint8_t hdr[2] = { 0xA1u, 0x61u };
  APP_DEC_Msg_t m;

  printf("test_header_fields\n");
  CHECK(APP_DEC_Decode(hdr, 2u, &m) == 0, "decode failed");
  CHECK_U(m.msg_type, 1u, "msg_type");            /* Source_Capabilities */
  CHECK_U(m.data_role, 1u, "data_role");          /* DFP                 */
  CHECK_U(m.spec_rev, 2u, "spec_rev");            /* 10b = PD 3.0        */
  CHECK_U(m.power_role, 1u, "power_role");        /* Source              */
  CHECK_U(m.msg_id, 0u, "msg_id");
  CHECK_U(m.num_obj, 6u, "num_obj");
  CHECK_U(m.extended, 0u, "extended");
  CHECK_U(m.msg_class, APP_DEC_CLASS_DATA, "msg_class");
  CHECK_U(m.flags & APP_DEC_F_NUMOBJ, APP_DEC_F_NUMOBJ,
          "missing payload is reported");
  CHECK_S(APP_DEC_MsgName(&m), "Source_Capabilities", "msg name");
  CHECK_S(APP_DEC_SpecRevName(m.spec_rev), "3.0", "spec rev name");

  /* the same message with its 6 objects attached is structurally clean */
  {
    uint8_t full[2 + 24];
    memset(full, 0, sizeof(full));
    full[0] = 0xA1u;
    full[1] = 0x61u;
    CHECK(APP_DEC_Decode(full, sizeof(full), &m) == 0, "decode full srccap");
    CHECK_U(m.flags, 0u, "complete frame has no flags");
    CHECK_U(m.data_len, 24u, "payload length");
  }
}

/* ------------------------------------------------------------------ */
/* 2. Fixed Supply PDO decoding                                       */
/* ------------------------------------------------------------------ */
static void test_pdo_fixed(void)
{
  /* Real captured PDO 0x0801912C == fixed 5 V / 3 A. */
  uint32_t mv = 0u, ma = 0u;
  char buf[128];

  printf("test_pdo_fixed\n");
  CHECK_U(APP_DEC_PDO_KIND(0x0801912Cu), APP_DEC_PDO_FIXED, "kind");
  CHECK(APP_DEC_PdoFixedToMvMa(0x0801912Cu, &mv, &ma) == 1, "extract failed");
  CHECK_U(mv, 5000u, "5V3A voltage");
  CHECK_U(ma, 3000u, "5V3A current");

  /* 9 V / 3 A: voltage 180 x 50 mV, current 300 x 10 mA */
  CHECK(APP_DEC_PdoFixedToMvMa(0x0002D12Cu, &mv, &ma) == 1, "9V extract");
  CHECK_U(mv, 9000u, "9V voltage");
  CHECK_U(ma, 3000u, "9V current");

  /* 20 V / 5 A */
  CHECK(APP_DEC_PdoFixedToMvMa(0x000641F4u, &mv, &ma) == 1, "20V extract");
  CHECK_U(mv, 20000u, "20V voltage");
  CHECK_U(ma, 5000u, "20V current");

  /* 0x0801912C has B27 (Unconstrained Power) set, so an "E" capability
   * letter is expected alongside the voltage and current. */
  CHECK_U(APP_DEC_PDO_FIXED_EXTPWR(0x0801912Cu), 1u, "B27 unconstrained power");
  APP_DEC_FormatPdo(0x0801912Cu, buf, sizeof(buf));
  CHECK_S(buf, "fixed 5.00V 3.00A E", "5V3A text");

  /* a PDO with no capability bits renders without letters */
  APP_DEC_FormatPdo(0x0001912Cu, buf, sizeof(buf));
  CHECK_S(buf, "fixed 5.00V 3.00A", "5V3A text, no caps");

  /* capability flags: B29 dual-role power, B28 suspend, B27 unconstrained,
   * B26 USB comms, B25 dual-role data, B24 unchunked, B23 EPR capable    */
  APP_DEC_FormatPdo(0x0801912Cu | (1u << 29) | (1u << 26) | (1u << 23),
                    buf, sizeof(buf));
  CHECK(strstr(buf, "P") != NULL, "EPR capable flag rendered");
  CHECK(strstr(buf, "C") != NULL, "USB comms flag rendered");
  CHECK(strstr(buf, "D") != NULL, "dual-role power flag rendered");

  /* a battery PDO must not be reported as fixed */
  CHECK(APP_DEC_PdoFixedToMvMa(0x40000000u, &mv, &ma) == 0,
        "battery PDO rejected by fixed extractor");
}

/* ------------------------------------------------------------------ */
/* 3. PPS APDO decoding                                               */
/* ------------------------------------------------------------------ */
static void test_pdo_pps(void)
{
  /* PPS 3.3 - 11 V / 5 A:  B31..30 = 11b (APDO), B29..28 = 00b (PPS),
   * max volt 110 x 100 mV in B24..17, min volt 33 x 100 mV in B15..8,
   * max curr 100 x 50 mA in B6..0.                                     */
  const uint32_t pps = 0xC0DC2164u;
  uint32_t minmv = 0u, maxmv = 0u, maxma = 0u;
  char buf[128];

  printf("test_pdo_pps\n");
  CHECK_U(APP_DEC_PDO_KIND(pps), APP_DEC_PDO_APDO, "kind");
  CHECK_U(APP_DEC_APDO_SUBTYPE(pps), APP_DEC_APDO_PPS, "apdo subtype");
  CHECK(APP_DEC_PdoPpsToRange(pps, &minmv, &maxmv, &maxma) == 1, "pps extract");
  CHECK_U(minmv, 3300u, "pps min voltage");
  CHECK_U(maxmv, 11000u, "pps max voltage");
  CHECK_U(maxma, 5000u, "pps max current");

  APP_DEC_FormatPdo(pps, buf, sizeof(buf));
  CHECK_S(buf, "pps 3.3-11.0V 5.00A", "pps text");

  /* PPS power-limited bit lives at B27 */
  APP_DEC_PdoPpsToRange(pps | (1u << 27), &minmv, &maxmv, &maxma);
  APP_DEC_FormatPdo(pps | (1u << 27), buf, sizeof(buf));
  CHECK(strstr(buf, "pwr-ltd") != NULL, "power limited flag");

  /* a fixed PDO must not be accepted as PPS */
  CHECK(APP_DEC_PdoPpsToRange(0x0801912Cu, &minmv, &maxmv, &maxma) == 0,
        "fixed PDO rejected by PPS extractor");
}

/* ------------------------------------------------------------------ */
/* 4. Request Data Object                                             */
/* ------------------------------------------------------------------ */
static void test_rdo(void)
{
  /* Request PDO #2, operating and max 3 A, No USB Suspend (B24). */
  const uint32_t rdo = 0x2104B12Cu;
  char buf[128];

  printf("test_rdo\n");
  CHECK_U(APP_DEC_RDO_POS(rdo), 2u, "object position");
  CHECK_U(APP_DEC_RDO_OP_CURR(rdo), 300u, "operating current raw");
  CHECK_U(APP_DEC_RDO_MAX_CURR(rdo), 300u, "max current raw");
  CHECK_U(APP_DEC_RDO_NO_SUSPEND(rdo), 1u, "no usb suspend");
  CHECK_U(APP_DEC_RDO_BATTERY(rdo), 0u, "not battery");

  APP_DEC_FormatRdo(rdo, buf, sizeof(buf));
  CHECK_S(buf, "req pos2 op 3.00A max 3.00A nosusp", "rdo text");

  /* EPR mode request bit is B21 */
  APP_DEC_FormatRdo(rdo | (1u << 21), buf, sizeof(buf));
  CHECK(strstr(buf, "epr") != NULL, "epr flag rendered");
}

/* ------------------------------------------------------------------ */
/* 5. Control messages                                                */
/* ------------------------------------------------------------------ */
static void test_control(void)
{
  /* GoodCRC (type 0) from a Source/DFP, PD 3.0, MessageID 0, no objects:
   * B5 data role | B7..6 rev | B8 power role  ->  0x01A0                 */
  const uint8_t goodcrc[2] = { 0xA1u, 0x01u };
  /* Accept (type 2), Source, DFP, PD3, id 1 -> 0x03A2 */
  const uint8_t accept[2] = { 0xA3u, 0x03u };
  /* PS_RDY (type 5) */
  const uint8_t psrdy[2] = { 0xA6u, 0x03u };
  APP_DEC_Msg_t m;

  printf("test_control\n");
  CHECK(APP_DEC_Decode(goodcrc, 2u, &m) == 0, "decode goodcrc");
  CHECK_U(m.msg_class, APP_DEC_CLASS_CONTROL, "goodcrc class");
  CHECK_S(APP_DEC_MsgName(&m), "GoodCRC", "goodcrc name");
  CHECK_U(m.flags, 0u, "goodcrc flags");

  CHECK(APP_DEC_Decode(accept, 2u, &m) == 0, "decode accept");
  CHECK_S(APP_DEC_MsgName(&m), "Accept", "accept name");
  CHECK_U(m.msg_id, 1u, "accept msg id");

  CHECK(APP_DEC_Decode(psrdy, 2u, &m) == 0, "decode psrdy");
  CHECK_S(APP_DEC_MsgName(&m), "PS_RDY", "psrdy name");

  /* reserved control type 31 must be flagged, not crash */
  {
    const uint8_t rsv[2] = { 0x9Fu, 0x01u };
    CHECK(APP_DEC_Decode(rsv, 2u, &m) == 0, "decode reserved");
    CHECK_U(m.flags & APP_DEC_F_TYPE_RSV, APP_DEC_F_TYPE_RSV, "reserved flagged");
  }
}

/* ------------------------------------------------------------------ */
/* 6. Extended / chunked messages                                     */
/* ------------------------------------------------------------------ */
static void test_extended(void)
{
  /* Source_Capabilities_Extended (extended type 1), chunked, chunk 0,
   * declared data size 24, padded to 7 data objects.
   * PD header: type 1 | data role | rev | power role | ndo 7 | extended. */
  uint8_t frame[2 + 2 + 28];
  APP_DEC_Msg_t m;

  printf("test_extended\n");
  memset(frame, 0xA5, sizeof(frame));
  frame[0] = 0xA1u;  /* type 1 | data role DFP (B5) | spec rev 10b (B7..6) */
  frame[1] = 0xF1u;  /* power role Source (B8) | ndo 7 (B14..12) | ext (B15) */
  /* extended header: data size 24, chunk 0, chunked = 1 */
  frame[2] = 24u;
  frame[3] = 0x80u;

  CHECK(APP_DEC_Decode(frame, sizeof(frame), &m) == 0, "decode extended");
  CHECK_U(m.extended, 1u, "extended flag");
  CHECK_U(m.msg_class, APP_DEC_CLASS_EXTENDED, "class");
  CHECK_S(APP_DEC_MsgName(&m), "Source_Capabilities_Ext", "extended name");
  CHECK_U(m.ext_data_size, 24u, "ext data size");
  CHECK_U(m.ext_chunked, 1u, "chunked");
  CHECK_U(m.ext_chunk_num, 0u, "chunk number");
  CHECK_U(m.data_offset, 4u, "data offset");
  CHECK_U(m.flags & APP_DEC_F_TRUNCATED, 0u, "no truncation when full");

  /* same frame but only 10 payload bytes present -> truncated */
  CHECK(APP_DEC_Decode(frame, 4u + 10u, &m) == 0, "decode short extended");
  CHECK_U(m.flags & APP_DEC_F_TRUNCATED, APP_DEC_F_TRUNCATED, "truncation flagged");
  CHECK_U(m.data_len, 10u, "available bytes");

  /* EPR capability message types (PD 3.1) */
  CHECK_S(APP_DEC_ExtendedName(17u), "EPR_Source_Capabilities", "epr src cap");
  CHECK_S(APP_DEC_ExtendedName(18u), "EPR_Sink_Capabilities", "epr snk cap");
  CHECK_S(APP_DEC_DataName(9u), "EPR_Request", "epr request");
  CHECK_S(APP_DEC_DataName(10u), "EPR_Mode", "epr mode");
}

/* ------------------------------------------------------------------ */
/* 7. Full frames and malformed input                                 */
/* ------------------------------------------------------------------ */
static void test_frames_and_malformed(void)
{
  /* Source_Capabilities with 2 objects: 5 V/3 A and 9 V/3 A */
  const uint8_t srccap[2 + 8] =
  {
    0xA1u, 0x21u,                                  /* type 1, ndo 2     */
    0x2Cu, 0x91u, 0x01u, 0x08u,                    /* 0x0801912C 5V/3A  */
    0x2Cu, 0xD1u, 0x02u, 0x00u                     /* 0x0002D12C 9V/3A  */
  };
  /* Request: type 2, ndo 1, RDO 0x2104B12C */
  const uint8_t req[2 + 4] =
  {
    0x82u, 0x10u,
    0x2Cu, 0xB1u, 0x04u, 0x21u
  };
  APP_DEC_Msg_t m;
  char line[192];

  printf("test_frames_and_malformed\n");

  CHECK_U(APP_DEC_FrameSize(srccap, sizeof(srccap)), 10u, "frame size srccap");
  CHECK_U(APP_DEC_FrameSize(req, sizeof(req)), 6u, "frame size req");
  CHECK_U(rd32(&srccap[2]), 0x0801912Cu, "first pdo raw");
  CHECK_U(rd32(&srccap[6]), 0x0002D12Cu, "second pdo raw");

  CHECK(APP_DEC_Decode(req, sizeof(req), &m) == 0, "decode request");
  CHECK_S(APP_DEC_MsgName(&m), "Request", "request name");
  CHECK_U(m.num_obj, 1u, "request ndo");
  APP_DEC_FormatFrame(req, sizeof(req), line, sizeof(line));
  CHECK(strstr(line, "Request") != NULL, "frame line has Request");
  CHECK(strstr(line, "req pos2") != NULL, "frame line decodes the RDO");

  APP_DEC_FormatFrame(srccap, sizeof(srccap), line, sizeof(line));
  CHECK(strstr(line, "Source_Capabilities") != NULL, "srccap frame line");
  CHECK(strstr(line, "fixed 5.00V 3.00A") != NULL, "srccap first pdo decoded");

  /* header claims 3 objects but only 1 is present */
  {
    const uint8_t bad[2 + 4] = { 0xA1u, 0x31u, 0x2Cu, 0x91u, 0x01u, 0x08u };
    CHECK(APP_DEC_Decode(bad, sizeof(bad), &m) == 0, "decode bad ndo");
    CHECK_U(m.flags & APP_DEC_F_NUMOBJ, APP_DEC_F_NUMOBJ, "numobj flagged");
  }

  /* a control message carrying data objects */
  {
    const uint8_t bad[6] = { 0xA0u, 0x21u, 0x00u, 0x00u, 0x00u, 0x00u };
    CHECK(APP_DEC_Decode(bad, sizeof(bad), &m) == 0, "decode ctrl+do");
    CHECK_U(m.num_obj, 2u, "ndo read");
    CHECK_U(m.flags & APP_DEC_F_NUMOBJ, APP_DEC_F_NUMOBJ, "short payload flagged");
  }

  /* 1-byte frame: must not read past the buffer */
  {
    const uint8_t tiny[1] = { 0xA1u };
    CHECK(APP_DEC_Decode(tiny, 1u, &m) == 0, "decode 1 byte");
    CHECK_U(m.flags & APP_DEC_F_SHORT, APP_DEC_F_SHORT, "short flagged");
    CHECK_U(m.msg_class, APP_DEC_CLASS_INVALID, "class invalid");
  }

  /* NULL / zero-length arguments */
  CHECK(APP_DEC_Decode(NULL, 0u, &m) == -1, "NULL msg rejected");
  CHECK(APP_DEC_Decode(srccap, 0u, NULL) == -1, "NULL out rejected");

  /* formatting into a tiny buffer must not overflow */
  {
    char small[8];
    memset(small, '#', sizeof(small));
    APP_DEC_FormatFrame(srccap, sizeof(srccap), small, sizeof(small));
    CHECK_U(small[7], '\0', "small buffer NUL-terminated");
  }
}

/* ------------------------------------------------------------------ */
/* 8. SOP naming (cable / E-marker engine relies on this)             */
/* ------------------------------------------------------------------ */
static void test_sop(void)
{
  printf("test_sop\n");
  CHECK_S(APP_DEC_SopName(0u), "SOP", "SOP");
  CHECK_S(APP_DEC_SopName(1u), "SOP'", "SOP'");
  CHECK_S(APP_DEC_SopName(2u), "SOP''", "SOP''");
  CHECK_S(APP_DEC_SopName(0xFFu), "-", "no sop");
}

int main(void)
{
  printf("=== app_dec host tests ===\n");
  test_header_fields();
  test_pdo_fixed();
  test_pdo_pps();
  test_rdo();
  test_control();
  test_extended();
  test_frames_and_malformed();
  test_sop();
  printf("=== %d passed, %d failed ===\n", s_pass, s_fail);
  return (s_fail == 0) ? 0 : 1;
}
