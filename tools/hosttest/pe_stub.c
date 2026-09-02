/*
 * Host stubs for the ST policy-engine EPR entry points.
 *
 * app_epr.c calls into the prebuilt ST core library on the target.  On the
 * host there is no PD stack, so these stubs stand in for it.  They return
 * USBPD_OK so the calling code paths are exercised; the tests that matter
 * (discovery, clamping, power maths) are pure logic and do not depend on
 * the value returned here.
 */
#include "usbpd_def.h"

USBPD_StatusTypeDef USBPD_PE_Send_ExtendeControlMessage(uint8_t PortNum,
                                                        USBPD_ExtendedControl_Typedef MessageType)
{
  (void)PortNum;
  (void)MessageType;
  return USBPD_OK;
}

USBPD_StatusTypeDef USBPD_PE_Request_EPRModeEnter(uint8_t PortNum)
{
  (void)PortNum;
  return USBPD_OK;
}

USBPD_StatusTypeDef USBPD_PE_Request_EPRModeExit(uint8_t PortNum)
{
  (void)PortNum;
  return USBPD_OK;
}

/*
 * Live cable identity is owned by app_cable_target.c, which pulls in the ST
 * VDM callbacks and is target-only.  The host tests drive APP_EPR_Ctx.cable_5a
 * directly, so this stub reports "no cable decoded yet" and lets
 * APP_EPR_RefreshCable() run without dragging in the target file.
 */
#include "app_cable.h"

const APP_CBL_Info_t *APP_CBL_GetLive(void)
{
  static const APP_CBL_Info_t none;
  return &none;      /* the real one is never NULL either */
}

uint8_t APP_CBL_IsLive(void)
{
  return 0u;         /* no Discover Identity on the host */
}

/*
 * app_pd.c is target-only (it pulls in the ST DPM headers), so the host build
 * stubs the two source-capability accessors the EPR verdict uses.  The
 * verdict text they feed is target behaviour and is not asserted on the host.
 */
uint32_t APP_PD_SrcMaxFixedMv(uint8_t port);
uint32_t APP_PD_SrcMaxFixedW(uint8_t port);

uint32_t APP_PD_SrcMaxFixedMv(uint8_t port)
{
  (void)port;
  return 0u;
}

uint32_t APP_PD_SrcMaxFixedW(uint8_t port)
{
  (void)port;
  return 0u;
}
