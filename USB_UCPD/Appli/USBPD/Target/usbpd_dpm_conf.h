/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbpd_dpm_conf.h
  * @author  MCD Application Team
  * @brief   Header file for stack/application settings file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __USBPD_DPM_CONF_H
#define __USBPD_DPM_CONF_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "usbpd_pdo_defs.h"
#include "usbpd_dpm_user.h"
#include "usbpd_vdm_user.h"

/* USER CODE BEGIN Includes */
/* Section where include file can be added */

/* USER CODE END Includes */

/* Define   ------------------------------------------------------------------*/
/* Define VID, PID,... manufacturer parameters */
#define USBPD_VID (0x0483u)     /*!< Vendor ID (assigned by the USB-IF)                     */
#define USBPD_PID (0x0002u)     /*!< Product ID (assigned by the manufacturer)              */
#define USBPD_XID (0xF0000003u) /*!< Value provided by the USB-IF assigned to the product   */

/* USER CODE BEGIN Define */
/* Section where Define can be added */

/* USER CODE END Define */

/* Exported typedef ----------------------------------------------------------*/
/* USER CODE BEGIN Typedef */
/* Section where Typedef can be added */

/* USER CODE END Typedef */

/* Private variables ---------------------------------------------------------*/
#ifndef __USBPD_DPM_CORE_C
extern USBPD_SettingsTypeDef            DPM_Settings[USBPD_PORT_COUNT];
extern USBPD_IdSettingsTypeDef          DPM_ID_Settings[USBPD_PORT_COUNT];
extern USBPD_USER_SettingsTypeDef       DPM_USER_Settings[USBPD_PORT_COUNT];
#else /* __USBPD_DPM_CORE_C */
USBPD_SettingsTypeDef       DPM_Settings[USBPD_PORT_COUNT] =
{
  {
    .PE_SupportedSOP = USBPD_SUPPORTED_SOP_SOP|USBPD_SUPPORTED_SOP_SOP1|USBPD_SUPPORTED_SOP_SOP2    , /* Supported SOP : SOP, SOP' SOP" SOP'Debug SOP"Debug */
    .PE_SpecRevision = USBPD_SPECIFICATION_REV3,/* spec revision value                                     */
    .PE_DefaultRole = USBPD_PORTPOWERROLE_SNK,  /* Default port role                                       */
    .PE_RoleSwap = USBPD_FALSE,                  /* support port role swap                                  */
    .PE_VDMSupport = USBPD_TRUE,
    .PE_RespondsToDiscovSOP = USBPD_TRUE,      /*!< Can respond successfully to a Discover Identity */
    .PE_AttemptsDiscovSOP = USBPD_TRUE,        /*!< Can send a Discover Identity */
    .PE_PingSupport = USBPD_FALSE,              /* support Ping (only for PD3.0)                                            */
    .PE_CapscounterSupport = USBPD_TRUE,       /* support caps counter                                    */
    .CAD_RoleToggle = USBPD_FALSE,               /* CAD role toggle                                         */
    .CAD_TryFeature = 0,              /* CAD try feature                                         */
    .CAD_AccesorySupport = USBPD_TRUE,         /* CAD accessory support                                   */
    .PE_PD3_Support.d =                           /*!< PD3 SUPPORT FEATURE                                              */
    {
      .PE_UnchunkSupport                = USBPD_TRUE,  /* support Unchunked mode (valid only spec revision 3.0)   */
      .PE_FastRoleSwapSupport           = USBPD_FALSE,   /* support fast role swap only spec revision 3.0            */
      .Is_GetPPSStatus_Supported        = USBPD_TRUE,  /*!< PPS message NOT supported by PE stack */
      .Is_SrcCapaExt_Supported          = USBPD_TRUE,  /*!< Source_Capabilities_Extended message supported or not by DPM */
      .Is_Alert_Supported               = USBPD_TRUE,   /*!< Alert message supported or not by DPM */
      .Is_GetStatus_Supported           = USBPD_TRUE,   /*!< Status message supported or not by DPM (Is_Alert_Supported should be enabled) */
      .Is_GetManufacturerInfo_Supported = USBPD_TRUE,  /*!< Manufacturer_Info message supported or not by DPM */
      .Is_GetCountryCodes_Supported     = USBPD_TRUE,  /*!< Country_Codes message supported or not by DPM */
      .Is_GetCountryInfo_Supported      = USBPD_TRUE,  /*!< Country_Info message supported or not by DPM */
      .Is_SecurityRequest_Supported     = USBPD_TRUE,  /*!< Security_Response message supported or not by DPM */
      .Is_FirmUpdateRequest_Supported   = USBPD_TRUE,  /*!< Firmware update response message supported by PE */
      .Is_GetBattery_Supported          = USBPD_TRUE,  /*!< Get Battery Capabitity and Status messages supported by PE */
      /* EPR (USB PD 3.1 Extended Power Range).
       *
       * THIS FLAG IS THE ROOT CAUSE OF THE MISSING EPR WIRE TRAFFIC.
       * Verified by disassembling the prebuilt ST PD3_FULL core library
       * (Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a, usbpd_pe.o) rather than by
       * reading headers.  USBPD_PE_Send_ExtendeControlMessage() at +0x682,
       * on the EPR_GETSRCCAPA branch, does:
       *
       *   ldr   r1, [r3]        ; r3 = per-port ctx -> DPM_Settings
       *   ldrh  r2, [r1, #8]    ; offset 8 = PE_PD3_Support (confirmed by
       *                         ;   offsetof(USBPD_SettingsTypeDef,...) = 8)
       *   ubfx  r1, r2, #0xb, #1; bit 11 = Is_EPR_Supported_SNK
       *   cbz   r1, skip        ; -> falls through WITHOUT queueing anything
       *
       * With the bit clear the library silently skipped the message queue and
       * still returned, so EPR_Get_Source_Cap never reached PRL/UCPD and
       * nothing appeared on CC.  That is exactly the observed hardware
       * behaviour: normal SPR traffic and no EPR at all.
       * (Bit 12, Is_EPR_Supported_SRC, gates the EPR_GETSNKCAPA branch the
       * same way; this board is a sink so it stays FALSE.)
       *
       * The flag is also what makes the stack set the EPR_Capable bit in the
       * RDO, which per PD3.1 6.4.10.1 the source Shall have seen in the most
       * recent Request before it will accept EPR_Mode(Enter).
       *
       * Note: USBPD_PE_Request_EPRModeEnter() itself does NOT test this bit.
       * Its own gates (usbpd_pe.o +0x48e) are PE_IsConnected, an SPR explicit
       * contract in the sink role (Params & 0x704 == 0x300) and spec rev 3 -
       * which is why entry is driven from the task loop after SNK_READY. */
      .Is_EPR_Supported_SNK             = USBPD_TRUE,  /*!< PD3.1 EPR sink support */
      .Is_EPR_Supported_SRC             = USBPD_FALSE, /*!< this board is sink-only */
    },

    .CAD_SRCToggleTime = 0,                    /* uint8_t CAD_SRCToggleTime; */
    .CAD_SNKToggleTime = 0,                    /* uint8_t CAD_SNKToggleTime; */
  }
};

USBPD_IdSettingsTypeDef          DPM_ID_Settings[USBPD_PORT_COUNT] =
{
  {
    .XID = USBPD_XID,     /*!< Value provided by the USB-IF assigned to the product   */
    .VID = USBPD_VID,     /*!< Vendor ID (assigned by the USB-IF)                     */
    .PID = USBPD_PID,     /*!< Product ID (assigned by the manufacturer)              */
  },
};

USBPD_USER_SettingsTypeDef       DPM_USER_Settings[USBPD_PORT_COUNT] =
{
  {
    .PE_DataSwap = USBPD_TRUE,                  /* support data swap                                       */
    /* VCONN swap: REVERTED to TRUE after the bench proved the change broke
     * PD entirely.
     *
     * Setting this FALSE made USBPD_DPM_EvaluateVconnSwap() answer REJECT.
     * The source needs VCONN to power the cable e-marker, and this source
     * asks the sink for a VCONN swap during discovery; rejecting it made
     * the source hard-reset in a loop, so PE_Power never reached
     * EXPLICITCONTRACT.  USBPD_PE_Send_Request() (usbpd_pe.o +0x526) gates
     * on (Params & 0x704) == 0x300 exactly like the EPR entry points, so
     * EVERY request - 9 V, 12 V, 20 V, PPS and EPR alike - returned
     * USBPD_BUSY(3).  That is the 'REQUEST not accepted by stack (3)'
     * regression, and it was mine.
     *
     * VCONN here is supplied by the source through the cable, not by this
     * board's own rail, so accepting the swap is correct.  If a swap ever
     * needs refusing it must be done in USBPD_DPM_EvaluateVconnSwap() on
     * real evidence, not by disabling the capability wholesale. */
    .PE_VconnSwap = USBPD_TRUE,                 /* support VCONN swap                  */
    .PE_DR_Swap_To_DFP = USBPD_TRUE,                  /*  Support of DR Swap to DFP                                  */
    .PE_DR_Swap_To_UFP = USBPD_TRUE,                  /*  Support of DR Swap to UFP                                  */
     .DPM_SNKExtendedCapa =                        /*!< SNK Extended Capability        */
	 {
	   .VID                    = USBPD_VID,  /*!< Vendor ID (assigned by the USB-IF)                      */
	   .PID                    = USBPD_PID,  /*!< Product ID (assigned by the manufacturer)               */
	   .XID                    = USBPD_XID,  /*!< Value provided by the USB-IF assigned to the product    */
	   .FW_revision            = 1,          /*!< Firmware version number                                 */
	   .HW_revision            = 2,          /*!< Hardware version number                                 */
       .SKEDB_Version          = USBPD_SKEDB_VERSION_1P0,  /*!<SKEDV Version (not the specification Version)
                                                               based on @ref USBPD_SKEDB_VERSION          */
       .LoadStep               = USBPD_SKEDB_LOADSTEP_150MA,/*!< Load Step based on @ref USBPD_SKEDB_LOADSTEP */
       .SinkLoadCharac.b       =             /*!< Sink Load Characteristics                               */
        {
           .PercentOverload    = 0,          /*!< Percent overload in 10% increments Values higher than 25
                                                  (11001b) are clipped to 250%. 00000b is the default.    */
           .OverloadPeriod     = 0,          /*!< Overload period in 20ms when bits 0-4 non-zero          */
           .DutyCycle          = 0,          /*!< Duty Cycle in 5% increments when bits 0-4 are non-zero  */
           .VBusVoltageDrop    = 0,          /*!< Can tolerate VBUS Voltage drop                          */
        },
       .Compliance             = 0,          /*!< Compliance based on combination of @ref USBPD_SKEDB_COMPLIANCE */
       .Touchtemp              = USBPD_SKEDB_TOUCHTEMP_NA,  /*< Touch Temp based on @ref USBPD_SKEDB_TOUCHTEMP   */
       .BatteryInfo            = 0,          /*!< Battery info                                                   */
       .SinkModes              = 0,          /*!< Sink Modes based on combination of @ref USBPD_SKEDB_SINKMODES  */
       .SinkMinimumPDP        = 0,          /*!< The Minimum PDP required by the Sink to operate without
                                                  consuming any power from its Battery(s) should it have one     */
       .SinkOperationalPDP     = 0,          /*!< The PDP the Sink requires to operate normally. For Sinks with
                                                  a Battery, it is the PDP rating of the charger supplied with
                                                  it or recommended for it.                                      */
       .SinkMaximumPDP         = 0,          /*!< The Maximum PDP the Sink can consume to operate and
                                                  charge its Battery(s) should it have one                       */
      },
#if _MANU_INFO
    .DPM_ManuInfoPort =                      /*!< Manufacturer information used for the port            */
    {
      .VID = USBPD_VID,                      /*!< Vendor ID (assigned by the USB-IF)        */
      .PID = USBPD_PID,                      /*!< Product ID (assigned by the manufacturer) */
      .ManuString = "STMicroelectronics",    /*!< Vendor defined byte array                 */
    },
#endif /* _MANU_INFO */
  },
};

#endif /* !__USBPD_DPM_CORE_C */

/* USER CODE BEGIN Variable */
/* Section where Variable can be added */

/* USER CODE END Variable */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN Constant */
/* Section where Constant can be added */

/* USER CODE END Constant */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN Macro */
/* Section where Macro can be added */

/* USER CODE END Macro */

#ifdef __cplusplus
}
#endif

#endif /* __USBPD_DPM_CONF_H */
