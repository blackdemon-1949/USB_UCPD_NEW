/**
 * @file    app_epr.h
 * @brief   EPR (Extended Power Range) engine.
 *
 * EPR is already compiled into this project: USBPDCORE_LIB_PD3_FULL defines
 * USBPDCORE_EPR in usbpd_def.h, and the prebuilt library contains the whole
 * state machine (EPRMode_Enter, EPRMode_Exit, EPRMode_KeepMode_SPR,
 * EPRMode_SRC_* ...).  This engine therefore drives the ST stack, it does not
 * reimplement it:
 *
 *   - the app publishes a Sink AVS PDO and an EPR Sink Operational PDP through
 *     USBPD_DPM_GetDataInfo(),
 *   - the stack performs EPR mode entry / keepalive / exit,
 *   - the app records the EPR source capabilities it is handed through
 *     USBPD_DPM_SetDataInfo() and the EPRMODE_* notifications.
 *
 * The AVS PDO layout is USBPD_SNKProgrammablePowerSupplyAVSPDO_TypeDef /
 * USBPD_SRCProgrammablePowerSupplyAVSPDO_TypeDef from usbpd_def.h:
 *   B7..0    PDP in 1 W        B15..8  Min voltage in 100 mV
 *   B16      Reserved          B25..17 Max voltage in 100 mV
 *   B27..26  Peak current (SRC) / reserved (SNK)
 *   B29..28  01b EPR Adjustable Voltage Supply
 *   B31..30  11b AVSPDO
 */
#ifndef APP_EPR_H
#define APP_EPR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "app_engines.h"

/* The boundary probe reads DPM_Settings/DPM_Params and the diagnostic
 * counters, which only exist in the firmware.  Host tests define
 * APP_EPR_HOSTTEST to compile the probe as a neutral stub. */
#if !defined(APP_EPR_HOSTTEST)
#define APP_EPR_TARGET_PROBE 1
#endif

/* AVS PDO field accessors */
#define APP_EPR_AVS_PDP_W(p)      ((uint32_t)((p) & 0xFFu))
#define APP_EPR_AVS_MIN_MV(p)     ((uint32_t)(((p) >> 8) & 0xFFu) * 100u)
#define APP_EPR_AVS_MAX_MV(p)     ((uint32_t)(((p) >> 17) & 0x1FFu) * 100u)
#define APP_EPR_AVS_PEAK(p)       ((uint8_t)(((p) >> 26) & 0x3u))
#define APP_EPR_AVS_KIND(p)       ((uint8_t)(((p) >> 28) & 0x3u))
#define APP_EPR_AVS_OBJ(p)        ((uint8_t)(((p) >> 30) & 0x3u))

#define APP_EPR_AVS_KIND_AVS      1u   /* 01b EPR Adjustable Voltage Supply */
#define APP_EPR_AVS_OBJ_AVSPDO    3u   /* 11b                               */

/* EPR mode message actions, from usbpd_def.h */
#define APP_EPR_ACT_ENTER           0x01u
#define APP_EPR_ACT_ENTER_ACK       0x02u
#define APP_EPR_ACT_ENTER_SUCCEEDED 0x03u
#define APP_EPR_ACT_ENTER_FAILED    0x04u
#define APP_EPR_ACT_EXIT            0x05u

/* USBPD_NotifyEventValue_TypeDef members this engine reacts to.  Values are
 * verbatim from usbpd_def.h (lines 1714-1721 of the copy in this project);
 * they are re-stated here so that app_epr.c needs neither usbpd_def.h nor
 * CMSIS and can therefore be unit-tested on the host.  A static check in
 * app_epr_target.c verifies them against the real enum at build time. */
#define APP_EPR_NOTIFY_MODE_INVALID      112u
#define APP_EPR_NOTIFY_MODE_ACK          113u
#define APP_EPR_NOTIFY_MODE_SUCCEEDED    114u
#define APP_EPR_NOTIFY_MODE_FAILED       115u
#define APP_EPR_NOTIFY_MODE_INIT         116u
#define APP_EPR_NOTIFY_MODE_EXIT         117u
#define APP_EPR_NOTIFY_SNKCAP_RECEIVED   118u
#define APP_EPR_NOTIFY_SRCCAP_RECEIVED   119u

/* EPR mode failure causes, from usbpd_def.h */
#define APP_EPR_ERR_UNKNOWN         0x00u
#define APP_EPR_ERR_CABLE_NOT_EPR   0x01u
#define APP_EPR_ERR_SRC_NOT_VCONN   0x02u
#define APP_EPR_ERR_RDO_NOT_EPR     0x03u
#define APP_EPR_ERR_SRC_UNABLE_NOW  0x04u
#define APP_EPR_ERR_PDO_NOT_EPR     0x05u

/* Hardware ceiling: this board's VBUS path is not rated beyond 28 V by
 * default, so EPR is capped here unless the user explicitly raises it. */
#define APP_EPR_DEFAULT_CEILING_MV  28000u
#define APP_EPR_MIN_MV              15000u   /* EPR starts above SPR 15 V   */
#define APP_EPR_MAX_MV              48000u

/* PD3.1: EPR Mode Capable bit in the first (5 V Fixed) Source PDO, B23. */
#define APP_EPR_SRC_FIXED_EPR_CAPABLE  (1u << 23)

/** Max main-loop polls to wait for a queued EPR request to reach the wire. */
#define APP_EPR_TX_POLL_LIMIT   200u

/* PD3.1 tEnterEPR is 500 ms; allow margin for a chunked reply. */
#define APP_EPR_ENTER_REPLY_MS  1200u

/**
  * @brief Snapshot of every precondition the ST core library actually tests,
  *        plus the layer counters, taken at one instant.
  *
  * Field names map 1:1 onto the decoded gates so a CLI report can attribute a
  * refusal to a specific instruction in the shipped library.
  */
typedef struct
{
  uint32_t params_word;     /* raw DPM_Params word the library dereferences */
  uint16_t pd3_support;     /* raw DPM_Settings.PE_PD3_Support              */

  uint8_t  spec_rev;        /* Params b1..0  : 2 = REV3                     */
  uint8_t  power_role;      /* Params b2     : 0 = SNK                      */
  uint8_t  pe_power;        /* Params b10..8 : 3 = EXPLICITCONTRACT         */
  uint8_t  is_connected;    /* Params b12                                   */
  uint8_t  power_range;     /* Params b29    : 1 = EPR mode active          */
  uint8_t  epr_snk_flag;    /* PD3_Support b11 Is_EPR_Supported_SNK         */
  uint8_t  epr_src_flag;    /* PD3_Support b12 Is_EPR_Supported_SRC         */

  uint8_t  g_connected;     /* individual gate verdicts                     */
  uint8_t  g_explicit;
  uint8_t  g_sink_role;
  uint8_t  g_rev3;
  uint8_t  g_epr_flag;

  uint8_t  extctrl_ok;      /* 1 = EPR_Get_Source_Cap may queue             */
  uint8_t  modeenter_ok;    /* 1 = EPR_Mode(Enter) may queue                */

  uint32_t pd_tx;           /* layer counters at snapshot time              */
  uint32_t pd_rx;
  uint32_t goodcrc_rx;
  uint32_t prot_err;
  uint32_t timeouts;
} APP_EPR_Probe_t;

typedef struct
{
  uint8_t  enable;          /* user-controlled EPR enable              */
  uint32_t ceiling_mv;      /* never request above this                */
  uint32_t want_mv;         /* desired operating point (0 = highest)   */
  uint32_t want_ma;

  uint8_t  src_epr_capable; /* source sent EPR AVS PDOs (EPR mode)     */
  uint8_t  src_spr_epr_capable; /* SPR 5V Fixed PDO had EPR-capable bit */
  uint8_t  enter_st;        /* last EPR_Mode(Enter) API status         */
  uint8_t  enter_valid;     /* 0 = EPR_Mode(Enter) never attempted     */
  uint8_t  getsrc_valid;    /* 0 = EPR_Get_Source_Cap never attempted  */
  uint32_t last_mode_do;    /* raw EPRMDO last received from the partner   */
  uint8_t  cable_5a;        /* 1 = e-marker confirmed a 5 A cable          */
  uint8_t  enter_pending;   /* EPR_Mode(Enter) queued, reply not seen yet  */
  uint8_t  enter_wanted;    /* deferred entry request, serviced in task ctx */
  uint32_t enter_deadline;  /* tick by which the reply must arrive         */
  uint8_t  getsrc_pending;  /* request accepted by PE, wire outcome unknown */
  uint8_t  getsrc_txd;      /* 1 = a UCPD TX was actually observed after it */
  uint32_t getsrc_tx_at;    /* PD TX counter when the request was accepted  */
  uint32_t getsrc_poll;     /* poll iterations spent waiting for that TX    */
  APP_EPR_Probe_t probe;    /* gate state captured at the last request      */
  uint8_t  enter_req_st;    /* last USBPD_PE_Request_EPRModeEnter() status */
  uint8_t  getsrc_st;       /* last EPR_Get_Source_Cap request status      */
  uint8_t  n_src_avs;       /* number of EPR AVS PDOs received         */
  uint32_t src_avs[7];      /* received EPR source AVS PDOs            */
  uint32_t src_min_mv;      /* widest offered window                   */
  uint32_t src_max_mv;
  uint32_t src_max_pdp_w;

  uint8_t  mode;            /* 0 = SPR, 1 = EPR (from USBPD_Params)    */
  uint8_t  entered;         /* EPR mode entry completed                */
  uint8_t  last_action;
  uint8_t  last_error;
  uint8_t  error_valid;

  uint32_t n_src_cap;       /* EPR source capability messages received */
  uint32_t n_enter;
  uint32_t n_exit;
  uint32_t n_failed;
  uint32_t n_keepalive_ack;
} APP_EPR_t;

extern APP_EPR_t APP_EPR_Ctx;

void APP_EPR_Init(void);

/* --- pure helpers (host testable) ---------------------------------- */
/** Build an AVS PDO.  @p sink selects the Sink variant (no peak current). */
uint32_t APP_EPR_BuildAvsPdo(uint32_t pdp_w, uint32_t min_mv, uint32_t max_mv,
                             uint8_t peak, uint8_t sink);
/** 1 when @p pdo is a well-formed EPR AVS PDO. */
int APP_EPR_IsAvsPdo(uint32_t pdo);
/** Clamp a desired operating point to a source AVS PDO and the user ceiling. */
int APP_EPR_ClampRequest(uint32_t avs_pdo, uint32_t ceiling_mv,
                         uint32_t want_mv, uint32_t *out_mv, uint32_t *out_ma);

const char *APP_EPR_ActionName(uint8_t action);
const char *APP_EPR_ErrorName(uint8_t code);
void APP_EPR_FormatAvs(uint32_t pdo, char *out, size_t outsz);

/* --- target glue ---------------------------------------------------- */
/** Fill the Sink AVS PDO the stack will publish (USBPD_DPM_GetDataInfo). */
uint32_t APP_EPR_GetSinkAvsPdo(void);
/** EPR Sink Operational PDP in watts. */
uint32_t APP_EPR_GetSinkPdpW(void);
/** Refresh the 5 A cable flag from the live e-marker identity. */
void APP_EPR_RefreshCable(void);
/** Called from USBPD_DPM_SetDataInfo for received EPR source PDOs. */
void APP_EPR_OnSrcPdo(const uint8_t *ptr, uint32_t size);
/** Called from USBPD_DPM_SetDataInfo for the partner's EPR_Mode object. */
void APP_EPR_OnModeDo(const uint8_t *ptr, uint32_t size);
#if defined(USBPDCORE_EPR)
USBPD_StatusTypeDef APP_EPR_RequestSrcCapa(uint8_t port);
/** Send EPR_Mode(Enter); returns the real ST policy-engine status. */
USBPD_StatusTypeDef APP_EPR_ModeEnter(uint8_t port);
/** Send EPR_Mode(Exit); returns the real ST policy-engine status. */
USBPD_StatusTypeDef APP_EPR_ModeExit(uint8_t port);
#endif
/** Human-readable USBPD_StatusTypeDef, for honest CLI reporting. */
const char *APP_EPR_StatusName(int st);
/** Human-readable PE_Power contract state. */
const char *APP_EPR_PowerStateName(uint8_t pe_power);
/** Capture every ST precondition and layer counter at this instant. */
void APP_EPR_Probe(uint8_t port, APP_EPR_Probe_t *pr);
/** Print the full PE/PRL/UCPD boundary report ('epr diag'). */
void APP_EPR_Diag(uint8_t port);
/** Main-loop poll: resolve a queued request into sent / not-sent. */
void APP_EPR_PollTx(uint8_t port);
/** Main-loop poll: report an EPR_Mode(Enter) the partner never answered. */
void APP_EPR_PollEnter(void);
/** Install a minimal PD frame counter funnel when the capture engine is
 *  compiled out, so PD TX/RX/GoodCRC counters are real in every profile. */
void APP_EPR_InstallTraceFunnel(void);
/** Called from APP_PD_OnNotify for the EPRMODE_* notifications. */
void APP_EPR_OnNotify(uint32_t event);
/** True when the local policy engine should set the RDO EPR-Mode-Capable bit. */
uint8_t APP_EPR_ShouldRequest(void);
/** Inspect SPR Source_Capabilities for the 5 V Fixed PDO EPR-capable bit. */
void APP_EPR_OnSprSrcCaps(const uint32_t *pdo, uint32_t n);
/** `epr` CLI command. */
int APP_EPR_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_EPR_H */
