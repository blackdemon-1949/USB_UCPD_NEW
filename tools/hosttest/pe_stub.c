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
