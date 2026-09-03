/**
 * @file    usbpd_dpm_user_pdsink.c
 * @brief   PDEngine (pdsink) implementation of the USBPD_DPM_Request* API
 *          the application still calls (CLI commands and SysTick).
 *
 * On this build the closed ST Policy Engine is NOT the running engine: the
 * pdsink object graph is (Middlewares/PDEngine/port, see pdport_app.h).
 * The ST DPM files (USBPD/App/usbpd_dpm_core.c and
 * USBPD/Target/usbpd_dpm_user.c) are therefore excluded from the build,
 * and this file re-provides the entry points the application modules
 * reference, routed through the pdport seam:
 *
 *  - the queries the pdsink PE can send are queued there (getcaps,
 *    getstatus, getpps, srcext, manuinfo, battery, countrycodes,
 *    countryinfo, hardreset, softreset).  "Queued" means the PRL will
 *    transmit it when the channel is idle; the reply is a normal PD
 *    message that shows up on the trace;
 *  - the VDM discovery wrappers refuse truthfully, exactly like
 *    app_vdm_target.c does for the VDM engine: the pdsink PE has no
 *    SVDM client, so reporting "sent" here would be a lie;
 *  - USBPD_DPM_TimerCounter() is a no-op: the pdsink engine is pumped
 *    from the main loop by pdport_service() and its timers run on the
 *    driver's time provider, so nothing ticks from SysTick.
 *
 * Return values keep the ST contract: USBPD_OK when the message was
 * queued, USBPD_ERROR when refused (the CLI prints the reason).
 */

#include "main.h"
#include "usbpd_core.h"
#include "usbpd_dpm_user.h"
#include "usbpd_dpm_core.h"

#if defined(PDENGINE_PDSINK)
#include "pdport_app.h"
#include "app_log.h"
#endif

#if defined(PDENGINE_PDSINK)

/* pdport_*() return 0 when the message was queued, -1 when refused. */
#define DPM_Q(r)  (((r) == 0) ? USBPD_OK : USBPD_ERROR)

USBPD_StatusTypeDef USBPD_DPM_RequestGetSourceCapability(uint8_t PortNum)
{
  (void)PortNum; /* single-port board */
  return DPM_Q(pdport_send_ctrl(PDPORT_CTRL_GET_SOURCE_CAP));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetStatus(uint8_t PortNum)
{
  (void)PortNum;
  return DPM_Q(pdport_send_ctrl(PDPORT_CTRL_GET_STATUS));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetPPS_Status(uint8_t PortNum)
{
  (void)PortNum;
  return DPM_Q(pdport_send_ctrl(PDPORT_CTRL_GET_PPS_STATUS));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetSourceCapabilityExt(uint8_t PortNum)
{
  (void)PortNum;
  return DPM_Q(pdport_send_ctrl(PDPORT_CTRL_GET_SOURCE_CAP_EXT));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetManufacturerInfo(uint8_t PortNum,
                                                         USBPD_SOPType_TypeDef SOPType,
                                                         uint8_t *pManuInfoData)
{
  (void)PortNum; (void)SOPType;

  if (pManuInfoData == NULL)
  {
    return USBPD_ERROR;
  }

  /* GMIDB data object: bit1:0 ManufacturerInfoTarget, bit31:2 Ref. */
  const USBPD_GMIDB_TypeDef *gmidb = (const USBPD_GMIDB_TypeDef *)pManuInfoData;
  const uint32_t do_ = (uint32_t)gmidb->ManufacturerInfoTarget |
                       ((uint32_t)gmidb->ManufacturerInfoRef << 2U);
  return DPM_Q(pdport_send_ext(PDPORT_EXT_GET_MANUFACTURER_INFO, &do_, 1U));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetBatteryCapability(uint8_t PortNum,
                                                          uint8_t *pBatteryCapRef)
{
  (void)PortNum; (void)pBatteryCapRef;
  return DPM_Q(pdport_send_ext(PDPORT_EXT_GET_BATTERY_CAP, NULL, 0U));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetBatteryStatus(uint8_t PortNum,
                                                      uint8_t *pBatteryStatusRef)
{
  (void)PortNum; (void)pBatteryStatusRef;
  return DPM_Q(pdport_send_ext(PDPORT_EXT_GET_BATTERY_STATUS, NULL, 0U));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetCountryCodes(uint8_t PortNum)
{
  (void)PortNum;
  return DPM_Q(pdport_send_ctrl(PDPORT_CTRL_GET_COUNTRY_CODES));
}

USBPD_StatusTypeDef USBPD_DPM_RequestGetCountryInfo(uint8_t PortNum,
                                                    uint16_t CountryCode)
{
  (void)PortNum;

  /* CODB data object: bit15:0 country code, rest reserved. */
  const uint32_t do_ = (uint32_t)CountryCode;
  return DPM_Q(pdport_send_data(PDPORT_DATA_GET_COUNTRY_INFO, &do_, 1U));
}

USBPD_StatusTypeDef USBPD_DPM_RequestVDM_DiscoveryIdentify(uint8_t PortNum,
                                                           USBPD_SOPType_TypeDef SOPType)
{
  (void)PortNum; (void)SOPType;
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return USBPD_ERROR;
}

USBPD_StatusTypeDef USBPD_DPM_RequestVDM_DiscoverySVID(uint8_t PortNum,
                                                       USBPD_SOPType_TypeDef SOPType)
{
  (void)PortNum; (void)SOPType;
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return USBPD_ERROR;
}

USBPD_StatusTypeDef USBPD_DPM_RequestVDM_DiscoveryMode(uint8_t PortNum,
                                                       USBPD_SOPType_TypeDef SOPType,
                                                       uint16_t SVID)
{
  (void)PortNum; (void)SOPType; (void)SVID;
  APP_LOG_Write("VDM: no initiator on the pdsink path (the pdsink PE has no SVDM client)\r\n");
  return USBPD_ERROR;
}

USBPD_StatusTypeDef USBPD_DPM_RequestHardReset(uint8_t PortNum)
{
  (void)PortNum;
  return DPM_Q(pdport_hard_reset());
}

USBPD_StatusTypeDef USBPD_DPM_RequestSoftReset(uint8_t PortNum,
                                               USBPD_SOPType_TypeDef SOPType)
{
  (void)PortNum; (void)SOPType; /* SOP only on this single-port board */
  return DPM_Q(pdport_send_ctrl(PDPORT_CTRL_SOFT_RESET));
}

void USBPD_DPM_TimerCounter(void)
{
  /*
   * Nothing to do.  On the ST stack this 1 kHz tick drove the PE timer
   * server (USBPD_PE_TimerEvent); on the pdsink path the whole engine is
   * pumped from the main loop by pdport_service() and its timers run on
   * the driver's time provider, so the SysTick handler has no work here.
   */
}

#endif /* PDENGINE_PDSINK */
