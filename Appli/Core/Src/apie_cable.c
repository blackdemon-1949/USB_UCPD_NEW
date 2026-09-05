/**
  ******************************************************************************
  * @file    apie_cable.c
  * @brief   Cable (SOP'/SOP'') intelligence + EPR/AVS protocol awareness.
  *
  * Sources (SOP) and cable plugs (SOP'/SOP'') are tracked as SEPARATE entities.
  * Cable facts come only from Discover Identity replies received on SOP' or
  * SOP''; they are never attributed to the source. EPR/AVS capability is
  * detected by scanning Source_Capabilities for an AVSPDO, but the EPR power
  * request path is GATED by APIE_HW_EPR_POWER_ENABLED (see apie.h) until the
  * Phase 5 hardware checkpoint is signed off.
  *
  * VDO field decoding uses the ST library's named anonymous-bitfield members
  * (e.g. pIdentity->CableVDO.b.EPR_Mode_Capable). The compiler builds both
  * the header and this translation unit with the same layout, so this is safe
  * within a single firmware build; the alternative hand-rolled shift math in
  * the first version of this file was rejected because I could not
  * unambiguously verify the bit positions from my own notes.
  ******************************************************************************
  */
#include "apie_cable.h"
#include "apie.h"
#include "app_log.h"
#include "usbpd_def.h"
#include <string.h>
#include <stdio.h>

static APIE_CableProfile_t s_cable;
static APIE_EPR_Info_t s_epr;

/* ----------------------------- helpers ---------------------------------- */

const char *APIE_SopName(uint8_t sop)
{
  switch (sop)
  {
    case USBPD_SOPTYPE_SOP:  return "SOP";
    case USBPD_SOPTYPE_SOP1: return "SOP'";
    case USBPD_SOPTYPE_SOP2: return "SOP''";
    default:                 return "SOP?";
  }
}

static const char *current_cap_name(uint8_t c)
{
  switch (c)
  {
    case VBUS_DEFAULT: return "default USB (1.5 A)";
    case VBUS_3A:      return "3 A";
    case VBUS_5A:      return "5 A";
    default:           return "reserved";
  }
}

static const char *ss_name(uint8_t ss)
{
  switch (ss)
  {
    case USB2P0_ONLY:      return "USB 2.0 only";
    case USB3P2_GEN1:      return "USB 3.2 Gen1";
    case USB3P2_USB4_GEN2: return "USB 3.2/USB4 Gen2";
    case USB4_GEN3:        return "USB4 Gen3";
    default:               return "?";
  }
}

uint16_t APIE_Cable_MaxMvFromField(uint8_t tier)
{
  switch (tier)
  {
    case VBUS_MAX_20V: return 20000U;
    case VBUS_MAX_30V: return 30000U;
    case VBUS_MAX_40V: return 40000U;
    case VBUS_MAX_50V: return 50000U;
    default:           return 20000U;
  }
}

static const char *vmax_name(uint8_t tier)
{
  switch (tier)
  {
    case VBUS_MAX_20V: return "20 V";
    case VBUS_MAX_30V: return "30 V (EPR)";
    case VBUS_MAX_40V: return "40 V (EPR)";
    case VBUS_MAX_50V: return "50 V (EPR)";
    default:           return "?";
  }
}

static const char *epr_state_name(uint8_t s)
{
  switch ((APIE_EPR_State_t)s)
  {
    case APIE_EPR_STATE_NOT_IN_EPR:  return "not in EPR";
    case APIE_EPR_STATE_SRC_CAP:    return "source has EPR caps";
    case APIE_EPR_STATE_MODE_ENTRY: return "EPR mode entry pending";
    case APIE_EPR_STATE_MODE_ACTIVE:return "EPR mode active";
    default:                        return "?";
  }
}

/* ----------------------------- API -------------------------------------- */

void APIE_Cable_Init(void)
{
  memset(&s_cable, 0, sizeof(s_cable));
  memset(&s_epr,   0, sizeof(s_epr));
  s_epr.epr_snk_pdp_w = 100U;   /* sane default until SNK_CAPA_EXT populates it */
}

void APIE_Cable_ResetSession(uint8_t conn_id)
{
  (void)conn_id;
  memset(&s_cable, 0, sizeof(s_cable));
  s_epr.state = APIE_EPR_STATE_NOT_IN_EPR;
  s_epr.current_voltage_mv = 0U;
}

void APIE_Cable_OnIdentity(uint8_t sop,
                           uint32_t id_header_v,
                           uint32_t cert_v,
                           uint32_t product_v,
                           uint8_t  passive_presence,
                           uint32_t cable_vdo,
                           uint8_t  active_presence,
                           uint32_t active_vdo1)
{
  /* Reinterpret the raw u32 words we got from the VDM_InformIdentity callback
     through the ST bitfield unions, then read each field by its NAMED member.
     This is the access pattern the ST header itself documents and uses in its
     own sample code, and it avoids any drift between our shift math and the
     compiler's layout. */
  USBPD_IDHeaderVDO_TypeDef idh;
  USBPD_ProductVdo_TypeDef  pvd;
  USBPD_CableVdo_TypeDef    cvdo;
  USBPD_ActiveCableVdo1_TypeDef avdo;

  idh.d32 = id_header_v;
  pvd.d32 = product_v;
  cvdo.d32 = cable_vdo;
  avdo.d32 = active_vdo1;

  (void)cert_v;

  s_cable.present = 1U;
  s_cable.sop = sop;
  s_cable.vid = (uint16_t)idh.b20.VID;
  s_cable.pid = (uint16_t)pvd.b.USBProductId;
  s_cable.vconn = 1U;   /* SOP'/SOP'' replies can only arrive if VCONN is on */

  if (active_presence != 0U)
  {
    s_cable.active       = 1U;
    s_cable.current_cap  = (uint8_t)avdo.b.VBUS_CurrentHandCap;
    s_cable.max_voltage_tier = (uint8_t)avdo.b.CableMaxVoltage;
    s_cable.epr_capable  = (uint8_t)(avdo.b.EPR_Mode_Capable ? 1U : 0U);
    s_cable.ss_cap       = (uint8_t)avdo.b.USB_HighestSpeed;
    s_cable.fw_rev       = (uint32_t)avdo.b.CableFWVersion;
    s_cable.hw_rev       = (uint32_t)avdo.b.CableHWVersion;
  }
  else if (passive_presence != 0U)
  {
    s_cable.active       = 0U;
    s_cable.current_cap  = (uint8_t)cvdo.b.VBUS_CurrentHandCap;
    s_cable.max_voltage_tier = (uint8_t)cvdo.b.CableMaxVoltage;
    s_cable.epr_capable  = (uint8_t)(cvdo.b.EPR_Mode_Capable ? 1U : 0U);
    s_cable.ss_cap       = (uint8_t)cvdo.b.USB_SS_Support;
    s_cable.fw_rev       = (uint32_t)cvdo.b.CableFWVersion;
    s_cable.hw_rev       = (uint32_t)cvdo.b.CableHWVersion;
  }
  else
  {
    /* No cable VDO — e.g. a non-electronically-marked cable that replied with
       IDH only, or the response only carried identity but not a cable VDO.
       Assume USB 2.0 / 3 A / 20 V / no EPR — these are the safest defaults. */
    s_cable.active = 0U;
    s_cable.current_cap = VBUS_3A;
    s_cable.max_voltage_tier = VBUS_MAX_20V;
    s_cable.epr_capable = 0U;
    s_cable.ss_cap = USB2P0_ONLY;
    s_cable.fw_rev = 0U;
    s_cable.hw_rev = 0U;
  }

  APP_LOG_Printf("[APIE] cable identity (%s): VID 0x%04X PID 0x%04X  ptype=%u  %s\r\n",
                 APIE_SopName(sop),
                 (unsigned)s_cable.vid, (unsigned)s_cable.pid,
                 (unsigned)idh.b20.ProductTypeUFPorCP,
                 s_cable.active ? "ACTIVE cable VDO1" :
                                 (passive_presence ? "passive Cable VDO" : "no cable VDO"));
  APP_LOG_Printf("       current=%s  vmax=%s  EPR=%s  speed=%s  fw=%lu hw=%lu\r\n",
                 current_cap_name(s_cable.current_cap),
                 vmax_name(s_cable.max_voltage_tier),
                 s_cable.epr_capable ? "capable" : "no",
                 ss_name(s_cable.ss_cap),
                 (unsigned long)s_cable.fw_rev, (unsigned long)s_cable.hw_rev);

  if ((s_cable.epr_capable != 0U) && (s_epr.epr_capable != 0U))
  {
    APP_LOG_Write("[APIE] cable matches source EPR capability — EPR entry possible once hw gate is set.\r\n");
  }
  else if ((s_epr.epr_capable != 0U) && (s_cable.epr_capable == 0U))
  {
    APP_LOG_Write("[APIE] source advertises EPR but cable does not — EPR will remain blocked.\r\n");
  }
}

void APIE_Cable_OnSopData(uint8_t sop, const uint8_t *payload, uint16_t len)
{
  (void)sop;
  (void)payload;
  (void)len;
  /* Future hook for VDM Attention / extended-cable messages. */
}

void APIE_EPR_OnSourceCaps(uint8_t port, const uint32_t *pdo, uint8_t n)
{
  uint8_t i;
  uint8_t saw_avs = 0U;
  uint16_t avs_min = 0xFFFFU, avs_max = 0U;

  (void)port;
  if ((pdo == NULL) || (n == 0U))
  {
    return;
  }

  for (i = 0U; i < n; i++)
  {
    USBPD_PDO_TypeDef u;
    u.d32 = pdo[i];
    if (u.GenericPDO.PowerObject == USBPD_CORE_PDO_TYPE_APDO)
    {
#if defined(USBPDCORE_EPR)
      /* AVSPDO shares the APDO type code but sets EPRAdjustableVoltage = 1 in
         the SRC AVSPDO bitfield (2 bits, value 01b). PPS APDO uses the
         PPSPowerLimited bit at that position instead. */
      if (u.SRCAVSPDO.EPRAdjustableVoltage == 1U)
      {
        uint16_t mn = (uint16_t)(u.SRCAVSPDO.MinVoltageIn100mV * 100U);
        uint16_t mx = (uint16_t)(u.SRCAVSPDO.MaxVoltageIn100mV * 100U);
        saw_avs = 1U;
        if (mn < avs_min) { avs_min = mn; }
        if (mx > avs_max) { avs_max = mx; }
      }
#endif
    }
  }

  if (saw_avs != 0U)
  {
    s_epr.epr_capable = 1U;
    s_epr.avs_present = 1U;
    s_epr.avs_min_mv = avs_min;
    s_epr.avs_max_mv = avs_max;
    if (s_epr.state == APIE_EPR_STATE_NOT_IN_EPR)
    {
      s_epr.state = APIE_EPR_STATE_SRC_CAP;
    }
    APP_LOG_Printf("[APIE] EPR source caps found: AVS %u-%u mV%s\r\n",
                   (unsigned)avs_min, (unsigned)avs_max,
                   (s_cable.epr_capable != 0U) ? " (cable supports EPR)" :
                                                 " (cable NOT EPR capable)");
  }
}

void APIE_EPR_OnModeChange(uint8_t new_state)
{
  s_epr.state = new_state;
  APP_LOG_Printf("[APIE] EPR state -> %s\r\n", epr_state_name(s_epr.state));
}

void APIE_EPR_OnAvs(const uint8_t *payload, uint16_t len)
{
  (void)payload;
  (void)len;
  /* Hook for future EPR_Get_Source_Cap / Source_Info decoders. */
}

const APIE_CableProfile_t *APIE_Cable_Get(void)   { return &s_cable; }
const APIE_EPR_Info_t     *APIE_EPR_Get(void)     { return &s_epr; }

uint8_t APIE_EPR_PowerAllowed(void)
{
  if (APIE_HW_EPR_POWER_ENABLED == 0U) { return 0U; }
  if (s_epr.epr_capable   == 0U)      { return 0U; }
  if (s_cable.epr_capable == 0U)      { return 0U; }
  if (APIE_Cable_MaxMvFromField(s_cable.max_voltage_tier) < APIE_MAX_VOLTAGE_MV)
  {
    return 0U;
  }
  return 1U;
}

void APIE_Cable_Dump(void)
{
  if (s_cable.present == 0U)
  {
    APP_LOG_Write("No cable identity received yet (no Discover Identity reply on SOP'/SOP'').\r\n");
    APP_LOG_Write("On this rig (no VCONN FET, one CC line) that is the expected state.\r\n");
    APP_LOG_Write("A non-e-marked cable also cannot answer SOP' — that is normal for plain USB-C cables.\r\n");
    return;
  }
  APP_LOG_Printf("Cable plug %s:\r\n", APIE_SopName(s_cable.sop));
  APP_LOG_Printf("  VID/PID       : 0x%04X / 0x%04X\r\n",
                 (unsigned)s_cable.vid, (unsigned)s_cable.pid);
  APP_LOG_Printf("  Kind          : %s\r\n", s_cable.active ? "active (re-timer/re-driver)" : "passive e-marked");
  APP_LOG_Printf("  Current cap   : %s\r\n", current_cap_name(s_cable.current_cap));
  APP_LOG_Printf("  Vmax          : %s\r\n", vmax_name(s_cable.max_voltage_tier));
  APP_LOG_Printf("  EPR capable   : %s\r\n", s_cable.epr_capable ? "yes" : "no");
  APP_LOG_Printf("  Superspeed    : %s\r\n", ss_name(s_cable.ss_cap));
  APP_LOG_Printf("  FW/HW rev     : %lu / %lu\r\n",
                 (unsigned long)s_cable.fw_rev, (unsigned long)s_cable.hw_rev);
}

void APIE_EPR_Dump(void)
{
  APP_LOG_Printf("EPR awareness : %s\r\n", s_epr.epr_capable ? "source advertises EPR" : "no EPR caps seen");
  APP_LOG_Printf("State         : %s\r\n", epr_state_name(s_epr.state));
  if (s_epr.avs_present != 0U)
  {
    APP_LOG_Printf("AVS range     : %u - %u mV\r\n",
                   (unsigned)s_epr.avs_min_mv, (unsigned)s_epr.avs_max_mv);
  }
  APP_LOG_Printf("Power allowed : %s  (hw gate %s, cable-emarker %s)\r\n",
                 APIE_EPR_PowerAllowed() ? "YES" : "NO",
                 APIE_HW_EPR_POWER_ENABLED ? "on" : "off (Phase 5 checkpoint pending)",
                 APIE_HW_CABLE_EMARKER ? "readable" : "unreachable (no VCONN)");
}
