/**
 * @file    app_epr.c
 * @brief   EPR engine (see app_epr.h).
 */
#include "app_epr.h"
#include "app_log.h"
#include "app_cable.h"
#if APP_ENG_EPR
#include "usbpd_core.h"
#endif
/* The boundary probe reads the same two structures the ST library
 * dereferences (DPM_Settings for the EPR gate, DPM_Params for the contract
 * state) and the diagnostic counters fed by the trace funnel.  Host builds
 * have no PD stack, so the probe compiles only for the target. */
#if defined(APP_EPR_TARGET_PROBE)
#include "usbpd_dpm_conf.h"
#include "usbpd_dpm_core.h"
#include "app_diag.h"
#include "usbpd_trace.h"
#include "main.h"          /* HAL_GetTick */
#endif

#if !defined(APP_EPR_TARGET_PROBE)
/* Host build: no HAL tick source.  The reply watchdog is target behaviour and
 * is not exercised by the host tests, so a monotonic stub is sufficient. */
static uint32_t HAL_GetTick(void) { static uint32_t t; return ++t; }
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

/**
  * @brief  Refresh the 5 A cable flag from the live e-marker identity.
  *
  * EPR requires a cable that declares 5 A capability.  Reading it from the
  * decoded Discover Identity keeps the advertised Sink Operational PDP tied
  * to real hardware instead of an assumption.
  */
void APP_EPR_RefreshCable(void)
{
#if APP_ENG_CABLE_VDM
  /* APP_CBL_GetLive() returns the address of a static and is NEVER NULL, so a
   * null check proves nothing.  Validity is carried by APP_CBL_IsLive(); use
   * that, otherwise an all-zero struct would be read as a real identity. */
  if (APP_CBL_IsLive() != 0u)
  {
    const APP_CBL_Info_t *cbl = APP_CBL_GetLive();

    APP_EPR_Ctx.cable_5a = (cbl->current_cap == APP_CBL_CUR_5A) ? 1u : 0u;
  }
#endif
}

uint32_t APP_EPR_GetSinkPdpW(void)
{
  /* EPR Sink Operational PDP, in watts, as sent in EPR_Mode(Enter).
   *
   * This must describe what the sink will actually draw, not an aspiration.
   * The previous form assumed 5 A unconditionally and reported 140 W at the
   * 28 V ceiling even when attached through a 3 A cable to a 100 W source -
   * an overstated capability, and exactly the kind of fabricated number that
   * must not go on the wire.  Cap by the current the cable is rated for. */
  uint32_t ma = APP_EPR_Ctx.want_ma;
  uint32_t w;

  if (ma == 0u)
  {
    ma = 5000u;
  }
  /* An e-marked 5 A cable is required for EPR; without a confirmed 5 A
   * marking assume the 3 A default the Type-C spec guarantees.
   *
   * cable_5a is refreshed from the main loop (APP_EPR_RefreshCable), NOT from
   * here.  This function is reached from USBPD_DPM_GetDataInfo, i.e. from
   * inside the ST policy engine's own callback while it is building
   * EPR_Mode(Enter); doing cable/VDM work there re-enters the stack from its
   * own call stack.  Keep this path a pure read of already-decoded state. */
  if (APP_EPR_Ctx.cable_5a == 0u)
  {
    if (ma > 3000u) { ma = 3000u; }
  }
  if (ma > 5000u) { ma = 5000u; }

  w = ((APP_EPR_Ctx.ceiling_mv / 1000u) * ma) / 1000u;
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
/* ------------------------------------------------------------------ */
/* EPR boundary probe                                                  */
/*                                                                     */
/* Evaluates every precondition the ST library actually tests, then    */
/* measures what crossed each layer.  The gate list is not guesswork:  */
/* it is decoded from the shipped PD3_FULL core library.               */
/*                                                                     */
/*   USBPD_PE_Send_ExtendeControlMessage (usbpd_pe.o +0x682)           */
/*     ldrb r2,[r5,#0xa]      ctx+0x26a  AMS/request slot must be 0    */
/*     ubfx r2,r6,#0xc,#1     Params bit12  PE_IsConnected             */
/*     ubfx r2,r6,#8,#3       PE_Power, must be 3 (EXPLICITCONTRACT)   */
/*     ubfx r1,r6,#2,#1       PE_PowerRole, must be 0 (SNK)            */
/*     ldrh r2,[r1,#8]; ubfx #0xb,#1   Is_EPR_Supported_SNK            */
/*                                                                     */
/*   USBPD_PE_Request_EPRModeEnter (usbpd_pe.o +0x48e)                 */
/*     same connect gate, then (Params & 0x704) == 0x300               */
/*     and PE_SpecRevision >= 2 (REV3)                                 */
/* ------------------------------------------------------------------ */

void APP_EPR_Probe(uint8_t port, APP_EPR_Probe_t *pr)
{
  uint32_t w;

  if (pr == NULL)
  {
    return;
  }
  memset(pr, 0, sizeof(*pr));

#if defined(APP_EPR_TARGET_PROBE)
  if (port >= USBPD_PORT_COUNT)
  {
    return;
  }

  /* Snapshot the live Params word the library itself dereferences. */
  memcpy(&w, &DPM_Params[port], sizeof(w));
  pr->params_word   = w;
  pr->pd3_support   = DPM_Settings[port].PE_PD3_Support.PD3_Support;

  pr->spec_rev      = (uint8_t)(w & 0x3u);
  pr->power_role    = (uint8_t)((w >> 2) & 0x1u);
  pr->pe_power      = (uint8_t)((w >> 8) & 0x7u);
  pr->is_connected  = (uint8_t)((w >> 12) & 0x1u);
  pr->power_range   = (uint8_t)((w >> 29) & 0x1u);
  pr->epr_snk_flag  = (uint8_t)((pr->pd3_support >> 11) & 0x1u);
  pr->epr_src_flag  = (uint8_t)((pr->pd3_support >> 12) & 0x1u);

  /* Individual gate verdicts, in the order the library evaluates them. */
  pr->g_connected   = pr->is_connected;
  pr->g_explicit    = (pr->pe_power == 3u) ? 1u : 0u;
  pr->g_sink_role   = (pr->power_role == 0u) ? 1u : 0u;
  pr->g_rev3        = (pr->spec_rev >= 2u) ? 1u : 0u;
  pr->g_epr_flag    = pr->epr_snk_flag;

  pr->extctrl_ok    = (uint8_t)(pr->g_connected & pr->g_explicit &
                                pr->g_sink_role & pr->g_epr_flag);
  /* EPRModeEnter does NOT test the EPR flag; it tests (w & 0x704)==0x300. */
  pr->modeenter_ok  = (uint8_t)((((w & 0x704u) == 0x300u) ? 1u : 0u) & pr->g_rev3);

  /* Layer counters, sampled so a caller can diff them across a request. */
  pr->pd_tx         = APP_DIAG_Get(APP_DIAG_PD_TX);
  pr->pd_rx         = APP_DIAG_Get(APP_DIAG_PD_RX);
  pr->goodcrc_rx    = APP_DIAG_Get(APP_DIAG_PD_GOODCRC_RX);
  pr->prot_err      = APP_DIAG_Get(APP_DIAG_PD_PROTOCOL_ERR);
  pr->timeouts      = APP_DIAG_Get(APP_DIAG_PD_TIMEOUT);
#else
  (void)port;
  (void)w;
#endif /* APP_EPR_TARGET_PROBE */
}

const char *APP_EPR_PowerStateName(uint8_t pe_power)
{
  switch (pe_power)
  {
    case 0u: return "NO CONTRACT";
    case 1u: return "DEFAULT 5V";
    case 2u: return "IMPLICIT CONTRACT";
    case 3u: return "EXPLICIT CONTRACT";
    case 4u: return "TRANSITION";
    default: return "reserved";
  }
}

/**
  * @brief  Print the full PE/PRL/UCPD boundary state for one EPR request.
  *
  * Reports the four things that are routinely conflated: whether the API was
  * even allowed to run, whether the stack accepted it, whether a frame left
  * the UCPD transmitter, and whether the partner acknowledged it.  A pass at
  * one layer is never reported as a pass at the next.
  */
void APP_EPR_Diag(uint8_t port)
{
  APP_EPR_Probe_t pr;

  APP_EPR_Probe(port, &pr);

  APP_LOG_Write("EPR boundary probe\r\n");
  APP_LOG_Printf("  DPM_Params word    : 0x%08lX\r\n", (unsigned long)pr.params_word);
  APP_LOG_Printf("  PD3_Support        : 0x%04X\r\n", (unsigned)pr.pd3_support);
  APP_LOG_Write("  -- runtime state --\r\n");
  APP_LOG_Printf("  PE connected       : %s\r\n", pr.is_connected ? "yes" : "NO");
  APP_LOG_Printf("  power role         : %s\r\n", pr.power_role ? "SRC" : "SNK");
  APP_LOG_Printf("  contract           : %s\r\n", APP_EPR_PowerStateName(pr.pe_power));
  APP_LOG_Printf("  spec revision      : %s\r\n",
                 (pr.spec_rev >= 2u) ? "PD3" : "PD2 (too old for EPR)");
  APP_LOG_Printf("  power range        : %s\r\n", pr.power_range ? "EPR" : "SPR");
  APP_LOG_Printf("  Is_EPR_Supported_SNK: %s\r\n", pr.epr_snk_flag ? "1 (enabled)" : "0 (GATE CLOSED)");
  APP_LOG_Write("  -- ST library gates --\r\n");
  APP_LOG_Printf("  EPR_Get_Source_Cap : %s\r\n",
                 pr.extctrl_ok ? "all gates PASS - API may queue"
                               : "BLOCKED before any message is built");
  if (pr.extctrl_ok == 0u)
  {
    APP_LOG_Printf("    connected=%u explicit=%u sink=%u eprflag=%u\r\n",
                   (unsigned)pr.g_connected, (unsigned)pr.g_explicit,
                   (unsigned)pr.g_sink_role, (unsigned)pr.g_epr_flag);
  }
  APP_LOG_Printf("  EPR_Mode(Enter)    : %s\r\n",
                 pr.modeenter_ok ? "all gates PASS - API may queue"
                                 : "BLOCKED ((Params&0x704)!=0x300 or not PD3)");
  APP_LOG_Write("  -- layer counters (cumulative) --\r\n");
  APP_LOG_Printf("  PD TX frames       : %lu\r\n", (unsigned long)pr.pd_tx);
  APP_LOG_Printf("  PD RX frames       : %lu\r\n", (unsigned long)pr.pd_rx);
  APP_LOG_Printf("  GoodCRC RX         : %lu\r\n", (unsigned long)pr.goodcrc_rx);
  APP_LOG_Printf("  protocol errors    : %lu\r\n", (unsigned long)pr.prot_err);
  APP_LOG_Printf("  timeouts           : %lu\r\n", (unsigned long)pr.timeouts);
}

USBPD_StatusTypeDef APP_EPR_RequestSrcCapa(uint8_t port)
{
  USBPD_StatusTypeDef st;
  APP_EPR_Probe_t before;

  /* Record the exact gate state the library is about to evaluate, so a
   * refusal can be attributed to a specific precondition rather than
   * guessed at. */
  APP_EPR_Probe(port, &before);
  APP_EPR_Ctx.probe = before;

  APP_LOG_Printf("EPR_Get_Source_Cap: entered, PE %s / %s / %s, EPRflag=%u\r\n",
                 before.is_connected ? "connected" : "NOT-connected",
                 before.power_role ? "SRC" : "SNK",
                 APP_EPR_PowerStateName(before.pe_power),
                 (unsigned)before.epr_snk_flag);

  if (before.extctrl_ok == 0u)
  {
    /* The library would return without building a message.  Say so plainly
     * instead of calling the API and reporting a misleading status. */
    APP_LOG_Printf("EPR_Get_Source_Cap: BLOCKED by ST gate "
                   "(connected=%u explicit=%u sink=%u eprflag=%u) - "
                   "no message will be built\r\n",
                   (unsigned)before.g_connected, (unsigned)before.g_explicit,
                   (unsigned)before.g_sink_role, (unsigned)before.g_epr_flag);
  }

  st = USBPD_PE_Send_ExtendeControlMessage(port,
                                           USBPD_EXTENDED_CONTROL_EPR_GETSRCCAPA);
  APP_EPR_Ctx.getsrc_st = (uint8_t)st;
  APP_EPR_Ctx.getsrc_valid = 1u;
  APP_EPR_Ctx.getsrc_tx_at = (uint32_t)before.pd_tx;
  APP_EPR_Ctx.getsrc_pending = (uint8_t)((st == USBPD_OK) ? 1u : 0u);

  /* "queued" is NOT "sent".  USBPD_OK means the PE accepted the request into
   * its AMS slot; the frame only exists on CC once the PD TX counter moves
   * and a GoodCRC comes back.  APP_EPR_PollTx() reports that separately. */
  APP_LOG_Printf("EPR_Get_Source_Cap: API status = %s (%d)%s\r\n",
                 APP_EPR_StatusName(st), (int)st,
                 (st == USBPD_OK) ? " -> ACCEPTED into PE AMS slot (queued, NOT yet sent)"
                                  : " -> REJECTED, nothing queued");
  return st;
}

USBPD_StatusTypeDef APP_EPR_ModeEnter(uint8_t port)
{
  USBPD_StatusTypeDef st = USBPD_PE_Request_EPRModeEnter(port);

  APP_EPR_Ctx.enter_st = (uint8_t)st;
  APP_EPR_Ctx.enter_valid = 1u;
  APP_EPR_Ctx.enter_req_st = (uint8_t)st;

  if (st == USBPD_OK)
  {
    /* Arm the reply watchdog.  "Queued" is not "entered": the partner still
     * has to answer with Enter Acknowledged / Succeeded / Failed.  Without
     * this the console would sit for ever showing a request that the source
     * silently ignored. */
    APP_EPR_Ctx.enter_pending  = 1u;
    APP_EPR_Ctx.enter_deadline = HAL_GetTick() + APP_EPR_ENTER_REPLY_MS;
  }

  APP_LOG_Printf("EPR_Mode(Enter): API status = %s (%d)%s\r\n",
                 APP_EPR_StatusName(st), (int)st,
                 (st == USBPD_OK) ? " -> ACCEPTED by PE (queued, awaiting source reply)"
                                  : " -> REJECTED, nothing queued");
  return st;
}

/**
  * @brief  Fail an EPR_Mode(Enter) that the partner never answered.
  *
  * PD3.1 gives the source tEnterEPR to respond.  Reporting the silence is the
  * difference between "we do not know" and a diagnosis.
  */
void APP_EPR_PollEnter(void)
{
  if (APP_EPR_Ctx.enter_pending == 0u)
  {
    return;
  }
  if (APP_EPR_Ctx.mode != 0u)
  {
    APP_EPR_Ctx.enter_pending = 0u;      /* Enter Succeeded already seen */
    return;
  }
  if ((int32_t)(HAL_GetTick() - APP_EPR_Ctx.enter_deadline) < 0)
  {
    return;
  }

  APP_EPR_Ctx.enter_pending = 0u;
  APP_EPR_Ctx.n_failed++;
  APP_LOG_Printf("EPR_Mode(Enter): NO REPLY from source within %u ms - "
                 "entry did not happen (source ignored the request)\r\n",
                 (unsigned)APP_EPR_ENTER_REPLY_MS);
}

USBPD_StatusTypeDef APP_EPR_ModeExit(uint8_t port)
{
  USBPD_StatusTypeDef st = USBPD_PE_Request_EPRModeExit(port);
  APP_LOG_Printf("EPR_Mode(Exit): API status = %s (%d)%s\r\n",
                 APP_EPR_StatusName(st), (int)st,
                 (st == USBPD_OK) ? " -> queued to PE" : " -> NOT queued");
  return st;
}
#endif /* USBPDCORE_EPR */

const char *APP_EPR_StatusName(int st)
{
  switch (st)
  {
    case 0:  return "USBPD_OK";
    case 1:  return "USBPD_NOTSUPPORTED";
    case 2:  return "USBPD_ERROR (PE state does not allow this operation)";
    case 3:  return "USBPD_BUSY (not connected / no SPR explicit contract yet)";
    case 4:  return "USBPD_TIMEOUT";
    default: return "unknown";
  }
}

/**
  * @brief  Resolve a pending EPR request into a real wire outcome.
  *
  * Called from the main loop.  Distinguishes the four layers that the CLI
  * must never conflate:
  *   queued  - PE accepted the request (API returned USBPD_OK)
  *   sent    - the PD TX counter advanced, so UCPD actually transmitted
  *   acked   - a GoodCRC came back, so the partner received it
  *   answered- an EPR_Source_Capabilities arrived
  */
void APP_EPR_PollTx(uint8_t port)
{
  uint32_t tx_now;
  uint32_t crc_now;

  if (APP_EPR_Ctx.getsrc_pending == 0u)
  {
    return;
  }
  if (port >= USBPD_PORT_COUNT)
  {
    return;
  }

#if defined(APP_EPR_TARGET_PROBE)
  tx_now  = APP_DIAG_Get(APP_DIAG_PD_TX);
  crc_now = APP_DIAG_Get(APP_DIAG_PD_GOODCRC_RX);
#else
  tx_now = 0u; crc_now = 0u;
#endif

  if (tx_now != APP_EPR_Ctx.getsrc_tx_at)
  {
    APP_EPR_Ctx.getsrc_pending = 0u;
    APP_EPR_Ctx.getsrc_txd = 1u;
    APP_LOG_Printf("EPR_Get_Source_Cap: UCPD TX observed (PD TX %lu -> %lu), "
                   "GoodCRC RX total %lu\r\n",
                   (unsigned long)APP_EPR_Ctx.getsrc_tx_at,
                   (unsigned long)tx_now, (unsigned long)crc_now);
    return;
  }

  /* No TX within the AMS window: the request was accepted by the PE but the
   * frame never reached the wire.  That is a distinct failure from a
   * rejected API call and must be reported as such. */
  if (APP_EPR_Ctx.getsrc_poll++ > APP_EPR_TX_POLL_LIMIT)
  {
    APP_EPR_Ctx.getsrc_pending = 0u;
    APP_EPR_Ctx.getsrc_txd = 0u;
    APP_LOG_Write("EPR_Get_Source_Cap: accepted by PE but NO UCPD TX observed "
                  "- blocked between PE and PRL\r\n");
  }
}

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
   * Reached once the source's EPR_Source_Capabilities have actually been
   * parsed (i.e. we are already in EPR mode).  Normal first entry does NOT
   * come through here - it is driven from APP_PD_Task() after the SPR
   * explicit contract exists, because USBPD_PE_Request_EPRModeEnter() (see
   * usbpd_pe.o +0x48e) requires Params & 0x704 == 0x300, i.e. PE_IsConnected
   * plus an explicit contract in the sink power role.
   *
   * Only request entry when the source actually advertised AVS PDOs and the
   * operator has not disabled EPR.  Non-blocking: this posts a request to the
   * PE and returns, so it is safe to call from the DPM callback path. */
  if ((APP_EPR_Ctx.src_epr_capable != 0u) &&
      (APP_EPR_Ctx.enable != 0u) &&
      (APP_EPR_Ctx.mode == 0u))
  {
    /* Source already told us it has AVS PDOs: go straight to mode entry. */
    (void)APP_EPR_ModeEnter(0u);
  }
#endif
}

/**
  * @brief  Record the partner's EPR_Mode Data Object (EPRMDO).
  *
  * PD3.1 Figure 6-32: B31..24 Action, B23..16 Data.  For "Enter Failed" the
  * Data byte is the reason code, which is the single most useful piece of
  * evidence when entry does not complete.  Without this the CLI could only
  * say "not entered" and not why.
  */
void APP_EPR_OnModeDo(const uint8_t *ptr, uint32_t size)
{
  uint32_t do32;
  uint8_t  action;
  uint8_t  data;

  if ((ptr == NULL) || (size < 4u))
  {
    return;
  }

  do32 = (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) |
         ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);

  action = (uint8_t)((do32 >> 24) & 0xFFu);
  data   = (uint8_t)((do32 >> 16) & 0xFFu);

  APP_EPR_Ctx.last_action = action;
  APP_EPR_Ctx.last_mode_do = do32;

  switch (action)
  {
    case APP_EPR_ACT_ENTER_FAILED:
      APP_EPR_Ctx.last_error  = data;
      APP_EPR_Ctx.error_valid = 1u;
      APP_EPR_Ctx.entered     = 0u;
      APP_EPR_Ctx.mode        = 0u;
      APP_LOG_Printf("EPR_Mode: source replied Enter Failed - %s (0x%02X)\r\n",
                     APP_EPR_ErrorName(data), (unsigned)data);
      break;

    case APP_EPR_ACT_ENTER_ACK:
      APP_LOG_Write("EPR_Mode: source replied Enter Acknowledged\r\n");
      break;

    case APP_EPR_ACT_ENTER_SUCCEEDED:
      APP_EPR_Ctx.entered = 1u;
      APP_EPR_Ctx.mode    = 1u;
      APP_LOG_Write("EPR_Mode: source replied Enter Succeeded - EPR mode active\r\n");
      break;

    case APP_EPR_ACT_EXIT:
      APP_EPR_Ctx.entered = 0u;
      APP_EPR_Ctx.mode    = 0u;
      APP_LOG_Write("EPR_Mode: EPR mode exited\r\n");
      break;

    default:
      APP_LOG_Printf("EPR_Mode: action 0x%02X data 0x%02X\r\n",
                     (unsigned)action, (unsigned)data);
      break;
  }
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

void APP_EPR_OnSprSrcCaps(const uint32_t *pdo, uint32_t n)
{
  /* PD 3.1 6.4.1.2.2: the EPR Mode Capable bit (B23) is defined only in the
   * FIRST (5 V Fixed) Source PDO and is the sink's ONLY legitimate way to
   * learn that an attached source supports EPR while still in SPR mode.
   *
   * This is the second real defect.  Previously src_epr_capable was set only
   * from received EPR *AVS* PDOs (APP_EPR_OnSrcPdo), which arrive in the
   * EPR_Source_Capabilities message - and that message is only ever sent
   * after EPR mode has been entered.  Entry in turn requires RDO bit 21,
   * which APP_EPR_ShouldRequest() gated on src_epr_capable.  That circular
   * dependency could never resolve, so even with a genuine EPR source the
   * board stayed in SPR forever. */
  APP_EPR_Ctx.src_spr_epr_capable = 0u;
  if ((pdo == NULL) || (n == 0u))
  {
    return;
  }
  /* Must be a Fixed Supply PDO (B31..30 = 00b) to carry the flag. */
  if ((pdo[0] & 0xC0000000u) != 0u)
  {
    return;
  }
  if ((pdo[0] & APP_EPR_SRC_FIXED_EPR_CAPABLE) != 0u)
  {
    APP_EPR_Ctx.src_spr_epr_capable = 1u;
  }
}

uint8_t APP_EPR_ShouldRequest(void)
{
  /* Set RDO B21 (EPR Mode Capable) whenever the local sink supports EPR and
   * the source advertised EPR in its 5 V Fixed PDO.  The source Shall have
   * seen this bit in the most recent Request before it will honour an
   * EPR_Mode(Enter); see PD3.1 6.4.10.1. */
  return ((APP_EPR_Ctx.enable != 0u) &&
          ((APP_EPR_Ctx.src_spr_epr_capable != 0u) ||
           (APP_EPR_Ctx.src_epr_capable != 0u)))
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
      APP_LOG_Write("usage: epr [on|off|caps|enter|exit|request|diag|ceiling <mv>|want <mv>|status]\r\n");
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
    APP_EPR_Ctx.enable = 1u;
    (void)APP_EPR_ModeEnter(0u);
  }
  else if (strcmp(argv[1], "exit") == 0)
  {
    (void)APP_EPR_ModeExit(0u);
  }
  else if (strcmp(argv[1], "caps") == 0)
  {
    (void)APP_EPR_RequestSrcCapa(0u);
  }
  else if (strcmp(argv[1], "diag") == 0)
  {
    APP_EPR_Diag(0u);
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
    APP_LOG_Printf("  src 5V PDO EPR bit : %s\r\n",
                   APP_EPR_Ctx.src_spr_epr_capable ? "SET (source is EPR capable)"
                                                   : "clear (SPR-only source)");
    APP_LOG_Printf("  src EPR AVS PDOs   : %s\r\n",
                   APP_EPR_Ctx.src_epr_capable
                     ? "received (EPR_Source_Capabilities seen)"
                     : "none received yet");
    APP_LOG_Printf("  RDO B21 EPR_Capable: %s\r\n",
                   APP_EPR_ShouldRequest() ? "will be set" : "will be clear");
    /* 0 is USBPD_OK, so an untouched field would read as a success that
     * never happened.  Report "not attempted" until a call really occurred. */
    APP_LOG_Printf("  last Enter status  : %s\r\n",
                   APP_EPR_Ctx.enter_valid
                     ? APP_EPR_StatusName((int)APP_EPR_Ctx.enter_st)
                     : "not attempted");
    APP_LOG_Printf("  last GetSrcCap st  : %s\r\n",
                   APP_EPR_Ctx.getsrc_valid
                     ? APP_EPR_StatusName((int)APP_EPR_Ctx.getsrc_st)
                     : "not attempted");
    APP_LOG_Printf("  EPR_Get_SrcCap wire: %s\r\n",
                   (APP_EPR_Ctx.getsrc_valid == 0u) ? "not attempted"
                     : (APP_EPR_Ctx.getsrc_pending ? "queued, awaiting TX"
                        : (APP_EPR_Ctx.getsrc_txd ? "UCPD TX observed"
                                                  : "queued but NO TX seen")));
    APP_LOG_Printf("  mode       : %s\r\n", APP_EPR_Ctx.mode ? "EPR" : "SPR");
    APP_LOG_Printf("  verdict    : %s\r\n",
                   (APP_EPR_Ctx.src_spr_epr_capable || APP_EPR_Ctx.src_epr_capable)
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
    /* last_action 0 is not a real EPR action; it means the partner has not
     * sent an EPR_Mode message yet.  "RESERVED" was misleading. */
    APP_LOG_Printf("  last action: %s\r\n",
                   (APP_EPR_Ctx.last_action == 0u)
                     ? "none (no EPR_Mode message exchanged)"
                     : APP_EPR_ActionName(APP_EPR_Ctx.last_action));
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

/* ------------------------------------------------------------------ */
/* Minimal PD frame counter funnel                                     */
/*                                                                     */
/* HARDWARE-DRIVEN FIX.  On a real board the boundary probe reported    */
/*   PD TX frames : 0 / PD RX frames : 0 / GoodCRC RX : 0              */
/* even while SPR negotiation was demonstrably working (source caps     */
/* received, Request accepted, 5 V explicit contract, INA226 showing    */
/* 5.020 V).  The counters were not merely wrong, they were dead:       */
/* APP_DIAG_PD_TX/RX are incremented only inside APP_PDCAP_Trace(),     */
/* which lives behind APP_ENG_CAPTURE.  The shipped CORE profile builds */
/* with cap=0, so that trace funnel is never installed and nothing ever */
/* fed those counters.                                                  */
/*                                                                     */
/* Counting PD frames must not depend on the optional analyzer, so this */
/* funnel is registered whenever the capture engine is absent.  It does */
/* nothing but bump counters and chain to the stock TRACER_EMB path, so */
/* the CubeMonitor-UCPD trace stream is unaffected.                     */
/* ------------------------------------------------------------------ */
#if defined(APP_EPR_TARGET_PROBE) && !APP_ENG_CAPTURE && defined(_TRACE)

static void epr_trace_funnel(TRACE_EVENT type, uint8_t port, uint8_t sop,
                             uint8_t *ptr, uint32_t size)
{
  if (type == USBPD_TRACE_MESSAGE_OUT)
  {
    APP_DIAG_Inc(APP_DIAG_PD_TX);
  }
  else if (type == USBPD_TRACE_MESSAGE_IN)
  {
    APP_DIAG_Inc(APP_DIAG_PD_RX);

    /* A GoodCRC is a 2-byte header with no data objects and message type 1
     * in the control-message space.  Counting it here is what lets the CLI
     * distinguish "transmitted" from "acknowledged by the partner". */
    if ((ptr != NULL) && (size >= 2u))
    {
      uint16_t hdr = (uint16_t)((uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8));
      uint16_t ndo = (uint16_t)((hdr >> 12) & 0x7u);
      uint16_t mtype = (uint16_t)(hdr & 0x1Fu);

      if ((ndo == 0u) && (mtype == 1u))
      {
        APP_DIAG_Inc(APP_DIAG_PD_GOODCRC_RX);
      }
    }
  }

  USBPD_TRACE_Add(type, port, sop, ptr, size);
}

void APP_EPR_InstallTraceFunnel(void)
{
  USBPD_PE_SetTrace(epr_trace_funnel, 3u);
}

#else /* capture engine owns the funnel, or no trace at all */

void APP_EPR_InstallTraceFunnel(void)
{
}

#endif
