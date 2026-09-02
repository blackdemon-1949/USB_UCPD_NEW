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

typedef struct
{
  uint8_t  enable;          /* user-controlled EPR enable              */
  uint32_t ceiling_mv;      /* never request above this                */
  uint32_t want_mv;         /* desired operating point (0 = highest)   */
  uint32_t want_ma;

  uint8_t  src_epr_capable; /* source sent EPR AVS PDOs (EPR mode)     */
  uint8_t  src_spr_epr_capable; /* SPR 5V Fixed PDO had EPR-capable bit */
  uint8_t  enter_st;        /* last EPR_Mode(Enter) API status         */
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
/** Called from USBPD_DPM_SetDataInfo for received EPR source PDOs. */
void APP_EPR_OnSrcPdo(const uint8_t *ptr, uint32_t size);
#if defined(USBPDCORE_EPR)
USBPD_StatusTypeDef APP_EPR_RequestSrcCapa(uint8_t port);
/** Send EPR_Mode(Enter); returns the real ST policy-engine status. */
USBPD_StatusTypeDef APP_EPR_ModeEnter(uint8_t port);
/** Send EPR_Mode(Exit); returns the real ST policy-engine status. */
USBPD_StatusTypeDef APP_EPR_ModeExit(uint8_t port);
#endif
/** Human-readable USBPD_StatusTypeDef, for honest CLI reporting. */
const char *APP_EPR_StatusName(int st);
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
