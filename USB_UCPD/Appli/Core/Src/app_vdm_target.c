/**
 * @file    app_vdm_target.c
 * @brief   ST Policy Engine SVDM glue for the VDM engine.
 *
 * This is the only file that calls into the precompiled ST PD library for
 * alternate-mode control.  Every call goes to a public entry point declared in
 * Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_core.h and verified present
 * in Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a:
 *
 *   USBPD_PE_SVDM_RequestIdentity   USBPD_PE_SVDM_RequestSVID
 *   USBPD_PE_SVDM_RequestMode       USBPD_PE_SVDM_RequestModeEnter
 *   USBPD_PE_SVDM_RequestModeExit
 *
 * Those prototypes are compiled under
 *   #if defined(USBPDCORE_SVDM) || defined(USBPDCORE_VCONN_SUPPORT)
 * and usbpd_def.h defines both when USBPDCORE_LIB_PD3_FULL is set, which this
 * project defines.  No Policy Engine logic is reimplemented here: the ST PE
 * owns the SVDM transaction, its retries and its timeouts.
 *
 * Validation and counters live in app_vdm.c, which stays free of ST headers so
 * it can be host-tested under sanitizers.
 */
#include "app_vdm.h"
#include "app_log.h"

#include "usbpd_core.h"
#include "usbpd_def.h"

/* The five entry points this file uses must exist in the linked library.  If a
 * future middleware update renames one, the link fails here rather than
 * silently dropping the feature.
 *
 * With PDENGINE_PDSINK the closed ST PE is not the running engine; the SVDM
 * request wrappers below reject before touching any PE symbol, so the table
 * and its typedefs are compiled out too. */
#if !defined(PDENGINE_PDSINK)
typedef USBPD_StatusTypeDef (*app_vdm_fn2_t)(uint8_t, USBPD_SOPType_TypeDef);
typedef USBPD_StatusTypeDef (*app_vdm_fn3_t)(uint8_t, USBPD_SOPType_TypeDef,
                                             uint16_t);
typedef USBPD_StatusTypeDef (*app_vdm_fn4_t)(uint8_t, USBPD_SOPType_TypeDef,
                                             uint16_t, uint8_t);

static const app_vdm_fn2_t s_fn_identity = &USBPD_PE_SVDM_RequestIdentity;
static const app_vdm_fn2_t s_fn_svid     = &USBPD_PE_SVDM_RequestSVID;
static const app_vdm_fn3_t s_fn_mode     = &USBPD_PE_SVDM_RequestMode;
static const app_vdm_fn4_t s_fn_enter    = &USBPD_PE_SVDM_RequestModeEnter;
static const app_vdm_fn4_t s_fn_exit     = &USBPD_PE_SVDM_RequestModeExit;
#endif
int APP_VDM_RequestIdentity(uint8_t port, uint8_t sop)
{
#if defined(PDENGINE_PDSINK)
  (void)port;
  (void)sop;
  /* No SVDM client exists in the pdsink PE (the closed-core SVDM
   * entry points are not linked on this path).  Rejecting here
   * keeps the CLI result line truthful. */
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return APP_VDM_CALL_REJECTED;
#else

  USBPD_StatusTypeDef st;

  if (APP_VDM_Prepare(port, sop, APP_VDM_REQ_IDENTITY, 0u, 0u)
      != APP_VDM_CALL_OK)
  {
    return APP_VDM_CALL_REJECTED;
  }
  st = s_fn_identity(port, (USBPD_SOPType_TypeDef)sop);
  return APP_VDM_CompleteCall((int)(st == USBPD_OK));

#endif
}

int APP_VDM_RequestSVID(uint8_t port, uint8_t sop)
{
#if defined(PDENGINE_PDSINK)
  (void)port;
  (void)sop;
  /* No SVDM client exists in the pdsink PE (the closed-core SVDM
   * entry points are not linked on this path).  Rejecting here
   * keeps the CLI result line truthful. */
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return APP_VDM_CALL_REJECTED;
#else

  USBPD_StatusTypeDef st;

  if (APP_VDM_Prepare(port, sop, APP_VDM_REQ_SVID, 0u, 0u) != APP_VDM_CALL_OK)
  {
    return APP_VDM_CALL_REJECTED;
  }
  st = s_fn_svid(port, (USBPD_SOPType_TypeDef)sop);
  return APP_VDM_CompleteCall((int)(st == USBPD_OK));

#endif
}

int APP_VDM_RequestMode(uint8_t port, uint8_t sop, uint16_t svid)
{
#if defined(PDENGINE_PDSINK)
  (void)port;
  (void)sop;
  (void)svid;
  /* No SVDM client exists in the pdsink PE (the closed-core SVDM
   * entry points are not linked on this path).  Rejecting here
   * keeps the CLI result line truthful. */
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return APP_VDM_CALL_REJECTED;
#else

  USBPD_StatusTypeDef st;

  if (APP_VDM_Prepare(port, sop, APP_VDM_REQ_MODE, svid, 0u)
      != APP_VDM_CALL_OK)
  {
    return APP_VDM_CALL_REJECTED;
  }
  st = s_fn_mode(port, (USBPD_SOPType_TypeDef)sop, svid);
  return APP_VDM_CompleteCall((int)(st == USBPD_OK));

#endif
}

int APP_VDM_ModeEnter(uint8_t port, uint8_t sop, uint16_t svid, uint8_t index)
{
#if defined(PDENGINE_PDSINK)
  (void)port;
  (void)sop;
  (void)svid;
  (void)index;
  /* No SVDM client exists in the pdsink PE (the closed-core SVDM
   * entry points are not linked on this path).  Rejecting here
   * keeps the CLI result line truthful. */
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return APP_VDM_CALL_REJECTED;
#else

  USBPD_StatusTypeDef st;

  if (APP_VDM_Prepare(port, sop, APP_VDM_REQ_MODE_ENTER, svid, index)
      != APP_VDM_CALL_OK)
  {
    return APP_VDM_CALL_REJECTED;
  }
  st = s_fn_enter(port, (USBPD_SOPType_TypeDef)sop, svid, index);
  return APP_VDM_CompleteCall((int)(st == USBPD_OK));

#endif
}

int APP_VDM_ModeExit(uint8_t port, uint8_t sop, uint16_t svid, uint8_t index)
{
#if defined(PDENGINE_PDSINK)
  (void)port;
  (void)sop;
  (void)svid;
  (void)index;
  /* No SVDM client exists in the pdsink PE (the closed-core SVDM
   * entry points are not linked on this path).  Rejecting here
   * keeps the CLI result line truthful. */
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return APP_VDM_CALL_REJECTED;
#else

  USBPD_StatusTypeDef st;

  if (APP_VDM_Prepare(port, sop, APP_VDM_REQ_MODE_EXIT, svid, index)
      != APP_VDM_CALL_OK)
  {
    return APP_VDM_CALL_REJECTED;
  }
  st = s_fn_exit(port, (USBPD_SOPType_TypeDef)sop, svid, index);
  return APP_VDM_CompleteCall((int)(st == USBPD_OK));

#endif
}
