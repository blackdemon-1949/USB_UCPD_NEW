/**
 * @file    app_epr.c
 * @brief   EPR engine (see app_epr.h).
 */
#include "app_epr.h"
#include "app_log.h"
#if APP_ENG_EPR
#include "usbpd_core.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

APP_EPR_t APP_EPR_Ctx;

static void epr_snprintf(char *out, size_t outsz, const char *fmt, ...)
{
  va_list ap;
  if (outsz == 0u)
  {
    return;
  }
  va_start(ap, fmt);
  (void)vsnprintf(out, outsz, fmt, ap);
  va_end(ap);
  out[outsz - 1u] = '\0';
}

void APP_EPR_Init(void)
{
  memset(&APP_EPR_Ctx, 0, sizeof(APP_EPR_Ctx));
  APP_EPR_Ctx.enable = 1u;
  APP_EPR_Ctx.ceiling_mv = APP_EPR_DEFAULT_CEILING_MV;
  APP_EPR_Ctx.want_mv = 0u;      /* 0 = highest the source and cable allow */
  APP_EPR_Ctx.want_ma = 5000u;
}

/* ------------------------------------------------------------------ */
/* AVS PDO construction / parsing                                      */
/* ------------------------------------------------------------------ */

uint32_t APP_EPR_BuildAvsPdo(uint32_t pdp_w, uint32_t min_mv, uint32_t max_mv,
                             uint8_t peak, uint8_t sink)
{
  uint32_t pdo = 0u;

  if (pdp_w > 255u)    { pdp_w = 255u; }
  if (min_mv > 25500u) { min_mv = 25500u; }
  if (max_mv > 51100u) { max_mv = 51100u; }

  pdo |= (pdp_w & 0xFFu);                        /* B7..0   PDP, 1 W     */
  pdo |= ((min_mv / 100u) & 0xFFu) << 8;         /* B15..8  min, 100 mV  */
  pdo |= ((max_mv / 100u) & 0x1FFu) << 17;       /* B25..17 max, 100 mV  */
  if (sink == 0u)
  {
    pdo |= ((uint32_t)(peak & 0x3u)) << 26;      /* B27..26 peak (SRC)   */
  }
  pdo |= ((uint32_t)APP_EPR_AVS_KIND_AVS) << 28; /* B29..28 = 01b        */
  pdo |= ((uint32_t)APP_EPR_AVS_OBJ_AVSPDO) << 30; /* B31..30 = 11b      */
  return pdo;
}

int APP_EPR_IsAvsPdo(uint32_t pdo)
{
  return ((APP_EPR_AVS_OBJ(pdo) == APP_EPR_AVS_OBJ_AVSPDO) &&
          (APP_EPR_AVS_KIND(pdo) == APP_EPR_AVS_KIND_AVS)) ? 1 : 0;
}

int APP_EPR_ClampRequest(uint32_t avs_pdo, uint32_t ceiling_mv,
                         uint32_t want_mv, uint32_t *out_mv, uint32_t *out_ma)
{
  uint32_t lo;
  uint32_t hi;
  uint32_t pdp_w;
  uint32_t mv;
  uint32_t ma;

  if ((out_mv == NULL) || (out_ma == NULL))
  {
    return 0;
  }
  if (APP_EPR_IsAvsPdo(avs_pdo) == 0)
  {
    return 0;
  }

  lo = APP_EPR_AVS_MIN_MV(avs_pdo);
  hi = APP_EPR_AVS_MAX_MV(avs_pdo);
  pdp_w = APP_EPR_AVS_PDP_W(avs_pdo);

  if (ceiling_mv != 0u)
  {
    if (hi > ceiling_mv) { hi = ceiling_mv; }
  }
  if (hi < lo)
  {
    return 0;                 /* the ceiling excludes the whole window */
  }

  mv = (want_mv == 0u) ? hi : want_mv;
  if (mv < lo) { mv = lo; }
  if (mv > hi) { mv = hi; }
  /* EPR AVS voltage steps are 100 mV */
  mv = (mv / 100u) * 100u;

  /* Current is limited by the advertised PDP at that voltage. */
  /* I[mA] = P[W] * 1e6 / V[mV].  (P = V*I, with V in volts; V = mv/1000,
   * so A = W*1000/mv and mA = W*1e6/mv.) */
  ma = (mv != 0u) ? ((uint32_t)(((uint64_t)pdp_w * 1000000ull) / (uint64_t)mv))
                  : 0u;
  if (ma > 5000u) { ma = 5000u; }                  /* 5 A connector limit */
  ma = (ma / 50u) * 50u;                           /* 50 mA steps */

  *out_mv = mv;
  *out_ma = ma;
  return 1;
}

const char *APP_EPR_ActionName(uint8_t action)
{
  switch (action)
  {
    case APP_EPR_ACT_ENTER:           return "Enter";
    case APP_EPR_ACT_ENTER_ACK:       return "Enter Acknowledged";
    case APP_EPR_ACT_ENTER_SUCCEEDED: return "Enter Succeeded";
    case APP_EPR_ACT_ENTER_FAILED:    return "Enter Failed";
    case APP_EPR_ACT_EXIT:            return "Exit";
    default:                          return "RESERVED";
  }
}

const char *APP_EPR_ErrorName(uint8_t code)
{
  switch (code)
  {
    case APP_EPR_ERR_UNKNOWN:        return "unknown cause";
    case APP_EPR_ERR_CABLE_NOT_EPR:  return "cable not EPR capable";
    case APP_EPR_ERR_SRC_NOT_VCONN:  return "source failed to become VCONN source";
    case APP_EPR_ERR_RDO_NOT_EPR:    return "EPR Mode Capable not set in RDO";
    case APP_EPR_ERR_SRC_UNABLE_NOW: return "source unable to enter EPR now";
    case APP_EPR_ERR_PDO_NOT_EPR:    return "EPR Mode Capable not set in PDO";
    default:                         return "RESERVED";
  }
}

void APP_EPR_FormatAvs(uint32_t pdo, char *out, size_t outsz)
{
  if (outsz == 0u)
  {
    return;
  }
  if (APP_EPR_IsAvsPdo(pdo) == 0)
  {
    epr_snprintf(out, outsz, "not an AVS PDO (0x%08lX)", (unsigned long)pdo);
    return;
  }
  epr_snprintf(out, outsz, "AVS %lu.%lu-%lu.%luV %luW",
               (unsigned long)(APP_EPR_AVS_MIN_MV(pdo) / 1000u),
               (unsigned long)((APP_EPR_AVS_MIN_MV(pdo) / 100u) % 10u),
               (unsigned long)(APP_EPR_AVS_MAX_MV(pdo) / 1000u),
               (unsigned long)((APP_EPR_AVS_MAX_MV(pdo) / 100u) % 10u),
               (unsigned long)APP_EPR_AVS_PDP_W(pdo));
}

/* ------------------------------------------------------------------ */
/* Target glue                                                         */
/* ------------------------------------------------------------------ */

uint32_t APP_EPR_GetSinkAvsPdo(void)
{
  uint32_t lo = 15000u;
  uint32_t hi = APP_EPR_Ctx.ceiling_mv;

  if (hi > APP_EPR_MAX_MV) { hi = APP_EPR_MAX_MV; }
  if (hi < lo) { hi = lo; }

  /* Advertise the widest window the board is allowed to accept; the actual
   * operating point is chosen when the source capabilities arrive. */
  return APP_EPR_BuildAvsPdo(APP_EPR_GetSinkPdpW(), lo, hi, 0u, 1u);
}

uint32_t APP_EPR_GetSinkPdpW(void)
{
  /* PDP implied by the ceiling at 5 A, capped at the 240 W EPR maximum. */
  uint32_t w = (APP_EPR_Ctx.ceiling_mv / 1000u) * 5u;
  return (w > 240u) ? 240u : w;
}

#if defined(USBPDCORE_EPR)
/**
  * @brief  Actively ask the connected source for its EPR Source Capabilities.
  *
  * THIS IS THE PIECE THAT WAS MISSING.  An SPR source does not put AVS PDOs in
  * its normal Source_Capabilities, so a sink that only inspects the cached
  * SPR capabilities can never learn that the partner supports EPR.  PD 3.1
  * discovery is initiated by the SINK sending the EPR_Get_Source_Cap extended
  * control message (USBPD_EXTENDED_CONTROL_EPR_GETSRCCAPA = 1,
  * usbpd_def.h:1361) via USBPD_PE_Send_ExtendeControlMessage()
  * (usbpd_core.h, declared under #if defined(USBPDCORE_EPR)).
  *
  * @retval USBPD_StatusTypeDef from the ST policy engine.
  */
USBPD_StatusTypeDef APP_EPR_RequestSrcCapa(uint8_t port)
{
  USBPD_StatusTypeDef st;
  st = USBPD_PE_Send_ExtendeControlMessage(port,
                                           USBPD_EXTENDED_CONTROL_EPR_GETSRCCAPA);
  APP_EPR_Ctx.getsrc_st = (uint8_t)st;
  APP_LOG_Printf("EPR: sent EPR_Get_Source_Cap (st=%d)\r\n", (int)st);
  return st;
}
#endif /* USBPDCORE_EPR */

void APP_EPR_OnSrcPdo(const uint8_t *ptr, uint32_t size)
{
  uint32_t n = size / 4u;
  uint32_t i;
  uint32_t pdo;

  if ((ptr == NULL) || (n == 0u))
  {
    return;
  }
  if (n > 7u) { n = 7u; }

  APP_EPR_Ctx.n_src_avs = 0u;
  APP_EPR_Ctx.src_min_mv = 0u;
  APP_EPR_Ctx.src_max_mv = 0u;
  APP_EPR_Ctx.src_max_pdp_w = 0u;

  for (i = 0u; i < n; i++)
  {
    pdo = (uint32_t)ptr[i * 4u] | ((uint32_t)ptr[i * 4u + 1u] << 8) |
          ((uint32_t)ptr[i * 4u + 2u] << 16) | ((uint32_t)ptr[i * 4u + 3u] << 24);
    if (APP_EPR_IsAvsPdo(pdo) == 0)
    {
      continue;
    }
    APP_EPR_Ctx.src_avs[APP_EPR_Ctx.n_src_avs] = pdo;
    APP_EPR_Ctx.n_src_avs++;
    APP_EPR_Ctx.src_epr_capable = 1u;

    if ((APP_EPR_Ctx.src_min_mv == 0u) ||
        (APP_EPR_AVS_MIN_MV(pdo) < APP_EPR_Ctx.src_min_mv))
    {
      APP_EPR_Ctx.src_min_mv = APP_EPR_AVS_MIN_MV(pdo);
    }
    if (APP_EPR_AVS_MAX_MV(pdo) > APP_EPR_Ctx.src_max_mv)
    {
      APP_EPR_Ctx.src_max_mv = APP_EPR_AVS_MAX_MV(pdo);
    }
    if (APP_EPR_AVS_PDP_W(pdo) > APP_EPR_Ctx.src_max_pdp_w)
    {
      APP_EPR_Ctx.src_max_pdp_w = APP_EPR_AVS_PDP_W(pdo);
    }
  }

#if defined(USBPDCORE_EPR) && (defined(USBPDCORE_SNK) || defined(USBPDCORE_DRP))
  /* THE ACTUAL MISSING WIRING.
   *
   * usbpd_def.h:137-145 enables USBPDCORE_EPR together with SNK/DRP for the
   * PD3_FULL build, so usbpd_pe_epr.o is compiled into the library and
   * USBPD_PE_Request_EPRModeEnter() is declared in usbpd_core.h:1215.  Until
   * now nothing in the application ever called it: the sink advertised its
   * AVS PDO through the DPM data-info path but never asked the policy engine
   * to enter EPR mode.  With an SPR-only source this was invisible; with an
   * EPR-capable source it is the reason no EPR session starts.
   *
   * Only request entry when the source actually advertised AVS PDOs and the
   * operator has not disabled EPR.  Non-blocking: this posts a request to the
   * PE and returns, so it is safe to call from the DPM callback path. */
  if ((APP_EPR_Ctx.src_epr_capable != 0u) &&
      (APP_EPR_Ctx.enable != 0u) &&
      (APP_EPR_Ctx.mode == 0u))
  {
    /* Source already told us it has AVS PDOs: go straight to mode entry. */
    USBPD_StatusTypeDef st = USBPD_PE_Request_EPRModeEnter(0u);
    APP_EPR_Ctx.enter_req_st = (uint8_t)st;
    APP_LOG_Printf("EPR: source advertises AVS -> requested EPR mode entry (st=%d)\r\n",
                   (int)st);
  }
#endif
}

void APP_EPR_OnNotify(uint32_t event)
{
  switch (event)
  {
    case APP_EPR_NOTIFY_MODE_INIT:
      APP_EPR_Ctx.n_enter++;
      APP_EPR_Ctx.last_action = APP_EPR_ACT_ENTER;
      break;

    case APP_EPR_NOTIFY_MODE_ACK:
      APP_EPR_Ctx.last_action = APP_EPR_ACT_ENTER_ACK;
      APP_EPR_Ctx.n_keepalive_ack++;
      break;

    case APP_EPR_NOTIFY_MODE_SUCCEEDED:
      APP_EPR_Ctx.entered = 1u;
      APP_EPR_Ctx.mode = 1u;
      APP_EPR_Ctx.last_action = APP_EPR_ACT_ENTER_SUCCEEDED;
      break;

    case APP_EPR_NOTIFY_MODE_FAILED:
      APP_EPR_Ctx.n_failed++;
      APP_EPR_Ctx.entered = 0u;
      APP_EPR_Ctx.mode = 0u;
      APP_EPR_Ctx.error_valid = 1u;
      APP_EPR_Ctx.last_action = APP_EPR_ACT_ENTER_FAILED;
      break;

    case APP_EPR_NOTIFY_MODE_EXIT:
      APP_EPR_Ctx.n_exit++;
      APP_EPR_Ctx.entered = 0u;
      APP_EPR_Ctx.mode = 0u;
      APP_EPR_Ctx.last_action = APP_EPR_ACT_EXIT;
      break;

    case APP_EPR_NOTIFY_MODE_INVALID:
      APP_EPR_Ctx.n_failed++;
      APP_EPR_Ctx.error_valid = 1u;
      APP_EPR_Ctx.last_error = APP_EPR_ERR_UNKNOWN;
      break;

    case APP_EPR_NOTIFY_SRCCAP_RECEIVED:
      APP_EPR_Ctx.n_src_cap++;
      break;

    default:
      break;
  }
}

uint8_t APP_EPR_ShouldRequest(void)
{
  return ((APP_EPR_Ctx.enable != 0u) && (APP_EPR_Ctx.src_epr_capable != 0u))
         ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

int APP_EPR_Cmd(int argc, char *argv[])
{
  unsigned v;
  uint32_t i;
  char line[96];

  if (argc >= 3)
  {
    if (sscanf(argv[2], "%u", &v) != 1)
    {
      APP_LOG_Write("usage: epr [on|off|ceiling <mv>|want <mv>|status]\r\n");
      return 1;
    }
  }

  if (argc < 2)
  {
    argv[1] = (char *)"status";
  }

  if (strcmp(argv[1], "on") == 0)
  {
    APP_EPR_Ctx.enable = 1u;
    APP_LOG_Printf("EPR enabled, ceiling %lu mV\r\n",
                   (unsigned long)APP_EPR_Ctx.ceiling_mv);
  }
  else if (strcmp(argv[1], "off") == 0)
  {
    APP_EPR_Ctx.enable = 0u;
    APP_LOG_Write("EPR disabled - SPR/PPS only\r\n");
  }
#if defined(USBPDCORE_EPR)
  else if (strcmp(argv[1], "request") == 0)
  {
    /* Actively start PD3.1 EPR discovery instead of waiting passively.
     * Step 1: ask the source for EPR Source Capabilities. */
    APP_EPR_Ctx.enable = 1u;
    (void)APP_EPR_RequestSrcCapa(0u);
  }
  else if (strcmp(argv[1], "enter") == 0)
  {
    /* Step 2 (manual): ask the policy engine to enter EPR mode. */
    USBPD_StatusTypeDef st = USBPD_PE_Request_EPRModeEnter(0u);
    APP_EPR_Ctx.enter_req_st = (uint8_t)st;
    APP_LOG_Printf("EPR mode entry requested (st=%d)\r\n", (int)st);
  }
#endif
  else if ((strcmp(argv[1], "ceiling") == 0) && (argc >= 3))
  {
    if ((v < APP_EPR_MIN_MV) || (v > APP_EPR_MAX_MV))
    {
      APP_LOG_Printf("ceiling must be %u..%u mV\r\n",
                     (unsigned)APP_EPR_MIN_MV, (unsigned)APP_EPR_MAX_MV);
    }
    else
    {
      APP_EPR_Ctx.ceiling_mv = v;
      APP_LOG_Printf("EPR ceiling %u mV (PDP %lu W)\r\n", v,
                     (unsigned long)APP_EPR_GetSinkPdpW());
    }
  }
  else if ((strcmp(argv[1], "want") == 0) && (argc >= 3))
  {
    APP_EPR_Ctx.want_mv = v;
    APP_LOG_Printf("EPR target %u mV (0 = highest available)\r\n", v);
  }
  else
  {
    /* Report the five distinct things people conflate.  Sink capability that
     * is merely CONFIGURED must never read as an available capability: an
     * SPR-only source cannot supply EPR however large our AVS PDO is. */
    APP_LOG_Write("EPR\r\n");
    APP_LOG_Printf("  ST core    : PD3_FULL lib (usbpd_pe_epr.o)\r\n");
    APP_LOG_Printf("  src advert : %s\r\n",
                   APP_EPR_Ctx.src_epr_capable
                     ? "EPR advertised by connected source"
                     : "NOT advertised by connected source (SPR only)");
    APP_LOG_Printf("  mode       : %s\r\n", APP_EPR_Ctx.mode ? "EPR" : "SPR");
    APP_LOG_Printf("  verdict    : %s\r\n",
                   APP_EPR_Ctx.src_epr_capable
                     ? (APP_EPR_Ctx.mode ? "EPR contract active"
                                         : "EPR available, not entered")
                     : "EPR unavailable on this session");
    APP_LOG_Write("  -- sink capability (configuration only) --\r\n");
    APP_LOG_Printf("  enable     : %s\r\n", APP_EPR_Ctx.enable ? "on" : "off");
    APP_LOG_Printf("  ceiling    : %lu mV\r\n",
                   (unsigned long)APP_EPR_Ctx.ceiling_mv);
    APP_LOG_Printf("  sink PDP   : %lu W\r\n",
                   (unsigned long)APP_EPR_GetSinkPdpW());
    APP_LOG_Printf("  sink AVS   : 0x%08lX\r\n",
                   (unsigned long)APP_EPR_GetSinkAvsPdo());
    APP_EPR_FormatAvs(APP_EPR_GetSinkAvsPdo(), line, sizeof(line));
    APP_LOG_Printf("               %s\r\n", line);
    APP_LOG_Printf("  src window : %lu..%lu mV, %lu W, %lu AVS PDOs\r\n",
                   (unsigned long)APP_EPR_Ctx.src_min_mv,
                   (unsigned long)APP_EPR_Ctx.src_max_mv,
                   (unsigned long)APP_EPR_Ctx.src_max_pdp_w,
                   (unsigned long)APP_EPR_Ctx.n_src_avs);
    for (i = 0u; i < APP_EPR_Ctx.n_src_avs; i++)
    {
      APP_EPR_FormatAvs(APP_EPR_Ctx.src_avs[i], line, sizeof(line));
      APP_LOG_Printf("   [%lu] 0x%08lX  %s\r\n", (unsigned long)i,
                     (unsigned long)APP_EPR_Ctx.src_avs[i], line);
    }
    APP_LOG_Printf("  last action: %s\r\n",
                   APP_EPR_ActionName(APP_EPR_Ctx.last_action));
    if (APP_EPR_Ctx.error_valid != 0u)
    {
      APP_LOG_Printf("  last error : %s\r\n",
                     APP_EPR_ErrorName(APP_EPR_Ctx.last_error));
    }
    APP_LOG_Printf("  counters   : srccap %lu enter %lu exit %lu failed %lu\r\n",
                   (unsigned long)APP_EPR_Ctx.n_src_cap,
                   (unsigned long)APP_EPR_Ctx.n_enter,
                   (unsigned long)APP_EPR_Ctx.n_exit,
                   (unsigned long)APP_EPR_Ctx.n_failed);
  }
  return 1;
}
