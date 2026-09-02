/**
 * @file    app_cable_target.c
 * @brief   Live cable/E-marker path: real ST VDM callback -> verdict engine.
 *
 * Split out of app_cable.c so that the pure VDO decode logic stays free of any
 * ST include and remains host-testable, while this file is the only place that
 * touches USBPD_DiscoveryIdentity_TypeDef.
 */
#include "app_cable.h"
#include "app_log.h"
#include "app_epr.h"
#include "app_diag.h"
#include "app_epr.h"
#include "app_txn.h"
#include "usbpd_def.h"
#include "usbpd_core.h"   /* USBPD_VDM_Callbacks, USBPD_PE_InitVDM_Callback */

#include <string.h>
#include <stdio.h>

static APP_CBL_Info_t s_live;
static uint8_t        s_live_valid;
static uint8_t        s_verdict = APP_CBL_NO_CABLE;
static uint8_t        s_vdm_registered;

/* Compile-time cross-check: app_epr.h restates these notification values so
 * that app_epr.c stays free of CMSIS and host-testable.  If ST ever changes
 * them, this file fails to compile rather than the firmware drifting. */
_Static_assert(APP_EPR_NOTIFY_MODE_INIT       == (uint32_t)USBPD_NOTIFY_EPRMODE_INIT,
               "app_epr.h: EPRMODE_INIT drifted from usbpd_def.h");
_Static_assert(APP_EPR_NOTIFY_MODE_ACK        == (uint32_t)USBPD_NOTIFY_EPRMODE_ACK,
               "app_epr.h: EPRMODE_ACK drifted from usbpd_def.h");
_Static_assert(APP_EPR_NOTIFY_MODE_SUCCEEDED  == (uint32_t)USBPD_NOTIFY_EPRMODE_SUCCEEDED,
               "app_epr.h: EPRMODE_SUCCEEDED drifted from usbpd_def.h");
_Static_assert(APP_EPR_NOTIFY_MODE_FAILED     == (uint32_t)USBPD_NOTIFY_EPRMODE_FAILED,
               "app_epr.h: EPRMODE_FAILED drifted from usbpd_def.h");
_Static_assert(APP_EPR_NOTIFY_MODE_EXIT       == (uint32_t)USBPD_NOTIFY_EPRMODE_EXIT,
               "app_epr.h: EPRMODE_EXIT drifted from usbpd_def.h");
_Static_assert(APP_EPR_NOTIFY_MODE_INVALID    == (uint32_t)USBPD_NOTIFY_EPRMODE_INVALID,
               "app_epr.h: EPRMODE_INVALID drifted from usbpd_def.h");
_Static_assert(APP_EPR_NOTIFY_SRCCAP_RECEIVED == (uint32_t)USBPD_NOTIFY_EPR_SRCCAP_RECEIVED,
               "app_epr.h: EPR_SRCCAP_RECEIVED drifted from usbpd_def.h");

/* Defined in usbpd_vdm_user.c (ST-generated, unmodified). */
extern const USBPD_VDM_Callbacks vdmCallbacks;

int APP_CBL_RegisterVdm(uint8_t port)
{
  /* USBPD_PE_InitVDM_Callback() takes a non-const pointer; the table itself is
   * const and the PE only reads it. */
  USBPD_PE_InitVDM_Callback(port, (USBPD_VDM_Callbacks *)&vdmCallbacks);
  s_vdm_registered = 1u;
  return 1;
}

uint8_t APP_CBL_VdmRegistered(void)
{
  return s_vdm_registered;
}

void APP_CBL_OnIdentity(const void *identity, uint8_t ok)
{
  const USBPD_DiscoveryIdentity_TypeDef *id =
      (const USBPD_DiscoveryIdentity_TypeDef *)identity;

  if ((ok == 0u) || (id == NULL))
  {
    s_live_valid = 0u;
    s_verdict = APP_CBL_NO_CABLE;
    return;
  }

  memset(&s_live, 0, sizeof(s_live));

  /* The ID header is always present: it tells us what answered. */
  s_live.vid = (uint16_t)id->IDHeader.b20.VID;
  s_live.pid = (uint32_t)id->ProductVDO.b.USBProductId;

  if (id->CableVDO_Presence != 0u)
  {
    /* A cable answered on SOP' - decode the passive cable VDO. */
    APP_CBL_DecodeVdo(id->CableVDO.d32, 0u, &s_live);
    s_live.vid = (uint16_t)id->IDHeader.b20.VID;
    s_live.pid = (uint32_t)id->ProductVDO.b.USBProductId;
    s_live.valid = 1u;
  }
  else if (id->ActiveCableVDO1_Presence != 0u)
  {
    /* Active cable: VDO1 carries the same electrical fields plus SOP'' info. */
    APP_CBL_DecodeVdo(id->ActiveCableVDO1.d32, 1u, &s_live);
    s_live.vid = (uint16_t)id->IDHeader.b20.VID;
    s_live.pid = (uint32_t)id->ProductVDO.b.USBProductId;
    s_live.valid = 1u;
  }

  if (s_live.valid != 0u)
  {
    s_live_valid = 1u;
    APP_DIAG_Inc(APP_DIAG_CAD_EVENT);
    /* Re-check the standing EPR request against what the cable can do. */
    s_verdict = APP_CBL_Check(&s_live, APP_EPR_Ctx.ceiling_mv, 5000u,
                              APP_EPR_ShouldRequest());
  }
  else
  {
    /* Something answered Discover Identity but it is not a cable (a hub or a
     * device).  That is still useful: no cable VDO means no EPR. */
    s_live_valid = 0u;
    s_verdict = APP_CBL_NO_CABLE;
  }
}

uint8_t APP_CBL_IsLive(void)
{
  return s_live_valid;
}

const APP_CBL_Info_t *APP_CBL_GetLive(void)
{
  return &s_live;
}

uint8_t APP_CBL_Evaluate(uint32_t mv, uint32_t ma, uint8_t want_epr)
{
  if (s_live_valid == 0u)
  {
    s_verdict = APP_CBL_NO_CABLE;
  }
  else
  {
    s_verdict = APP_CBL_Check(&s_live, mv, ma, want_epr);
  }
  return s_verdict;
}

int APP_CBL_LiveCmd(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "status";
  char line[96];

  if (strcmp(sub, "vdo") == 0)
  {
    APP_CBL_Info_t info;
    unsigned vdo = 0u;

    if ((argc < 3) || (sscanf(argv[2], "%x", &vdo) != 1))
    {
      APP_LOG_Write("usage: cable vdo <hex>\r\n");
      return 1;
    }
    memset(&info, 0, sizeof(info));
    APP_CBL_DecodeVdo((uint32_t)vdo, 0u, &info);
    APP_CBL_FormatInfo(&info, line, sizeof(line));
    APP_LOG_Printf("cable VDO 0x%08X\r\n  %s\r\n", vdo, line);
    return 1;
  }

  if (strcmp(sub, "discover") == 0)
  {
    /* The ST stack runs Discover Identity automatically on attach; this only
     * reports what the last response yielded. */
    APP_LOG_Printf("last identity: %s\r\n",
                   s_live_valid ? "cable answered" : "no cable identity");
    APP_LOG_Printf("verdict      : %s\r\n", APP_CBL_VerdictName(s_verdict));
    return 1;
  }

  if (strcmp(sub, "status") != 0)
  {
    APP_LOG_Write("usage: cable [status|vdo <hex>|discover]\r\n");
    return 1;
  }

  /* Re-evaluate the verdict against the contract that is actually in force,
   * so the reported answer reflects the present operating point rather than
   * whatever it was when the identity arrived. */
  {
    extern APP_TXN_Port_t APP_TXN_Port0;

    (void)APP_CBL_Evaluate(APP_TXN_Port0.contract_mv,
                           APP_TXN_Port0.contract_ma,
                           APP_EPR_Ctx.enable);
  }

  APP_LOG_Printf("cable (live Discover Identity, VDM callbacks %s)\r\n",
                 APP_CBL_VdmRegistered() ? "registered" : "NOT REGISTERED");
  if (APP_CBL_IsLive() == 0u)
  {
    APP_LOG_Write("  identity : not discovered - no cable VDO received yet\r\n");
    APP_LOG_Printf("  verdict  : %s\r\n", APP_CBL_VerdictName(s_verdict));
    return 1;
  }

  {
    const APP_CBL_Info_t *i = APP_CBL_GetLive();

    APP_CBL_FormatInfo(i, line, sizeof(line));
    APP_LOG_Printf("  %s\r\n", line);
    APP_LOG_Printf("  VID/PID  : 0x%04X / 0x%04lX\r\n",
                   (unsigned)i->vid, (unsigned long)i->pid);
    APP_LOG_Printf("  type     : %s\r\n", i->active ? "active" : "passive");
    APP_LOG_Printf("  USB speed: %s\r\n", APP_CBL_SsName(i->ss_support));
    APP_LOG_Printf("  current  : %lu mA\r\n",
                   (unsigned long)APP_CBL_MaxCurrentMa(i->current_cap));
    APP_LOG_Printf("  max VBUS : %lu mV (%s)\r\n",
                   (unsigned long)APP_CBL_MaxVoltageMv(i->max_vbus),
                   APP_CBL_TermName(i->term_type));
    APP_LOG_Printf("  EPR      : %s\r\n", i->epr_capable ? "capable" : "not capable");
  }
  APP_LOG_Printf("  verdict  : %s\r\n", APP_CBL_VerdictName(s_verdict));
  return 1;
}
