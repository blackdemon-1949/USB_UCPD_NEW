/**
 * @file    app_vdm.h
 * @brief   Vendor Defined Message / alternate-mode control.
 *
 * Thin, explicit application layer over the ST Policy Engine SVDM entry points
 * declared in Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_core.h:
 *
 *   USBPD_PE_SVDM_RequestIdentity(PortNum, SOPType)
 *   USBPD_PE_SVDM_RequestSVID(PortNum, SOPType)
 *   USBPD_PE_SVDM_RequestMode(PortNum, SOPType, SVID)
 *   USBPD_PE_SVDM_RequestModeEnter(PortNum, SOPType, SVID, ModeIndex)
 *   USBPD_PE_SVDM_RequestModeExit(PortNum, SOPType, SVID, ModeIndex)
 *
 * This module does NOT implement a Policy Engine.  Every request is handed to
 * the precompiled ST PE, which owns the SVDM transaction, retries and
 * timeouts.  The responses come back through the USBPD_VDM_InformModeEnter /
 * USBPD_VDM_InformModeExit callbacks, which are registered in
 * Appli/USBPD/Target/usbpd_vdm_user.c.
 *
 * Those callbacks are invoked by the PE state machine, which runs from the
 * main loop rather than a hard ISR.  They are still kept to state recording
 * only - no printf, no CDC - and all reporting happens from CLI context.
 */
#ifndef APP_VDM_H
#define APP_VDM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** SOP type, mirroring USBPD_SOPTYPE_* from usbpd_def.h. */
#define APP_VDM_SOP          0u   /* SOP   - port partner */
#define APP_VDM_SOP1         1u   /* SOP'  - cable plug 1 */
#define APP_VDM_SOP2         2u   /* SOP'' - cable plug 2 */

/** Responder status, mirroring SVDM_RESPONDER_* from usbpd_def.h. */
#define APP_VDM_STAT_NONE    0u
#define APP_VDM_STAT_ACK     1u   /* SVDM_RESPONDER_ACK  */
#define APP_VDM_STAT_NAK     2u   /* SVDM_RESPONDER_NAK  */
#define APP_VDM_STAT_BUSY    3u   /* SVDM_RESPONDER_BUSY */

/** Which request is outstanding, for timeout bookkeeping. */
typedef enum
{
  APP_VDM_REQ_NONE = 0,
  APP_VDM_REQ_IDENTITY,
  APP_VDM_REQ_SVID,
  APP_VDM_REQ_MODE,
  APP_VDM_REQ_MODE_ENTER,
  APP_VDM_REQ_MODE_EXIT,
  APP_VDM_REQ_COUNT
} APP_VDM_Req_t;

/** ST status returned by the PE entry points, as reported to the caller. */
#define APP_VDM_CALL_REJECTED  (-1)   /* PE refused (not ready / bad arg) */
#define APP_VDM_CALL_OK         (0)   /* PE accepted the request          */

typedef struct
{
  uint8_t  port;
  uint8_t  sop;
  uint8_t  pending;            /* a request is outstanding              */
  uint8_t  pending_req;        /* APP_VDM_Req_t                         */
  uint16_t svid;
  uint8_t  mode_index;
  uint8_t  last_status;        /* APP_VDM_STAT_*                        */
  uint8_t  last_req;           /* APP_VDM_Req_t                         */
  uint8_t  last_rejected;      /* 1 when the PE refused the last request */
  uint16_t last_svid;
  uint8_t  last_mode_index;
  uint8_t  in_alt_mode;        /* 1 between an ACK'd enter and an ACK'd exit */
  uint32_t n_enter_req;
  uint32_t n_enter_ack;
  uint32_t n_enter_nak;
  uint32_t n_enter_busy;
  uint32_t n_exit_req;
  uint32_t n_exit_ack;
  uint32_t n_exit_nak;
  uint32_t n_exit_busy;
  uint32_t n_identity_req;
  uint32_t n_svid_req;
  uint32_t n_mode_req;
  uint32_t n_rejected;
} APP_VDM_State_t;

void APP_VDM_Init(void);

/** Issue a Discover Identity request - this is the cable re-discovery entry
 *  point.  @p sop selects the plug: APP_VDM_SOP1 for SOP'. */
int APP_VDM_RequestIdentity(uint8_t port, uint8_t sop);

/** Discover the SVIDs a partner supports. */
int APP_VDM_RequestSVID(uint8_t port, uint8_t sop);

/** Discover the modes available under one SVID. */
int APP_VDM_RequestMode(uint8_t port, uint8_t sop, uint16_t svid);

/** Request entry into alternate mode @p svid, mode @p index. */
int APP_VDM_ModeEnter(uint8_t port, uint8_t sop, uint16_t svid, uint8_t index);

/** Request exit from alternate mode @p svid, mode @p index. */
int APP_VDM_ModeExit(uint8_t port, uint8_t sop, uint16_t svid, uint8_t index);

/* Internal glue, exposed so app_vdm_target.c can drive a request in two
 * steps: validate and latch, then record the PE's answer. */
int APP_VDM_Prepare(uint8_t port, uint8_t sop, APP_VDM_Req_t req,
                    uint16_t svid, uint8_t index);
int APP_VDM_CompleteCall(int st_ok);

/* Called from the ST VDM callbacks.  State recording only. */
void APP_VDM_OnModeEnter(uint8_t port, uint8_t sop, uint32_t status,
                         uint16_t svid, uint32_t mode_index);
void APP_VDM_OnModeExit(uint8_t port, uint8_t sop, uint32_t status,
                        uint16_t svid, uint32_t mode_index);

const APP_VDM_State_t *APP_VDM_Get(void);
void APP_VDM_Clear(void);
const char *APP_VDM_ReqName(APP_VDM_Req_t r);
const char *APP_VDM_StatName(uint8_t status);
const char *APP_VDM_SopName(uint8_t sop);

/** Pure classifier, exposed for host testing: given a status and whether a
 *  mode enter/exit is outstanding, decide the resulting in_alt_mode state. */
uint8_t APP_VDM_ApplyStatus(uint8_t in_alt_mode, uint8_t is_enter,
                            uint32_t status);

int APP_VDM_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_VDM_H */
