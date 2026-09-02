/**
 * @file    app_cable.c
 * @brief   Cable / E-marker decoding (see app_cable.h).
 *
 * ID Header VDO layout (USB PD 3.1, matching USBPD_IDHeaderVDO_TypeDef b30):
 *   B15..0  USB Vendor ID      B20..16 Reserved
 *   B22..21 Connector Type     B25..23 Product Type (DFP)
 *   B26     Reserved           B29..27 Product Type (UFP)
 *   B30     USB comms as host  B31     Reserved
 * A cable plug reports its product type in the UFP field: 5 active, 6 passive.
 */
#include "app_cable.h"
#include "app_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define APP_CBL_VDM_CMD_DISCOVER_IDENTITY  0x01u

static void csnprintf(char *out, size_t outsz, const char *fmt, ...)
{
  va_list ap;
  if (outsz == 0u)
  {
    return;
  }
  va_start(ap, fmt);
  (void)vsnprintf(out, outsz, fmt, ap);
  va_end(ap);
  out[outsz - 1u] = '\0';
}

static uint32_t rd32le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* ID header                                                           */
/* ------------------------------------------------------------------ */

void APP_CBL_DecodeIdentity(const uint32_t *vdo, uint8_t count,
                            APP_CBL_Identity_t *out)
{
  uint8_t pt;

  if (out == NULL)
  {
    return;
  }
  memset(out, 0, sizeof(*out));
  if ((vdo == NULL) || (count == 0u))
  {
    return;
  }

  out->vdo_count = count;
  out->id_header = vdo[0];
  if (count > 1u) { out->cert_stat = vdo[1]; }
  if (count > 2u) { out->product   = vdo[2]; }
  if (count > 3u) { out->cable_vdo = vdo[3]; }

  pt = (uint8_t)((out->id_header >> 27) & 0x7u);
  out->active  = (pt == APP_CBL_PT_ACTIVE_CABLE) ? 1u : 0u;
  out->passive = (pt == APP_CBL_PT_PASSIVE_CABLE) ? 1u : 0u;
  out->vid_valid = ((out->id_header & 0xFFFFu) != 0u) ? 1u : 0u;
}

void APP_CBL_DecodeVdo(uint32_t vdo, uint8_t active, APP_CBL_Info_t *out)
{
  if (out == NULL)
  {
    return;
  }
  memset(out, 0, sizeof(*out));

  out->valid      = 1u;
  out->active     = active ? 1u : 0u;
  out->ss_support = APP_CBL_SS_SUPPORT(vdo);
  out->current_cap= APP_CBL_CURRENT_CAP(vdo);
  out->max_vbus   = APP_CBL_MAX_VBUS(vdo);
  out->term_type  = APP_CBL_TERM_TYPE(vdo);
  out->latency    = APP_CBL_LATENCY(vdo);
  out->epr_capable= APP_CBL_EPR_CAPABLE(vdo);
  out->to_type    = APP_CBL_TO_TYPE(vdo);
  out->fw_ver     = APP_CBL_FW_VER(vdo);
  out->hw_ver     = APP_CBL_HW_VER(vdo);

  if (out->active != 0u)
  {
    out->sop2_present = APP_CBL_ACT_SOP2_PRESENT(vdo);
    out->vbus_through = APP_CBL_ACT_VBUS_THRU(vdo);
    out->sbu_support  = APP_CBL_ACT_SBU_SUPPORT(vdo);
  }
}

int APP_CBL_DecodeDiscoverIdentityAck(const uint8_t *payload, uint16_t len,
                                      APP_CBL_Info_t *info)
{
  APP_CBL_Identity_t id;
  uint32_t vdo[7];
  uint8_t  n = 0u;
  uint16_t off;
  uint32_t vdm;
  uint8_t  cmd;

  if ((payload == NULL) || (info == NULL))
  {
    return -1;
  }
  memset(info, 0, sizeof(*info));

  /* payload = VDM header + up to 6 VDOs */
  if (len < 8u)
  {
    return -2;
  }

  vdm = rd32le(payload);
  cmd = (uint8_t)(vdm & 0x1Fu);
  if (cmd != APP_CBL_VDM_CMD_DISCOVER_IDENTITY)
  {
    return -3;                    /* not a Discover Identity response */
  }

  for (off = 4u; (off + 4u) <= len && (n < 7u); off = (uint16_t)(off + 4u))
  {
    vdo[n++] = rd32le(&payload[off]);
  }
  if (n < 4u)
  {
    return -4;                    /* a cable answer always has a VDO4 */
  }

  APP_CBL_DecodeIdentity(vdo, n, &id);

  APP_CBL_DecodeVdo(id.cable_vdo, id.active, info);
  info->vid = (uint16_t)(id.product >> 16);       /* Product VDO B31..16 */
  info->pid = (uint32_t)(id.product & 0xFFFFu);   /* Product VDO B15..0  */
  return (int)n;
}

/* ------------------------------------------------------------------ */
/* Capability translation and compatibility                            */
/* ------------------------------------------------------------------ */

uint32_t APP_CBL_MaxVoltageMv(uint8_t max_vbus)
{
  switch (max_vbus)
  {
    case APP_CBL_VBUS_20V: return 20000u;
    case APP_CBL_VBUS_30V: return 30000u;
    case APP_CBL_VBUS_40V: return 40000u;
    case APP_CBL_VBUS_50V: return 50000u;
    default:               return 0u;
  }
}

uint32_t APP_CBL_MaxCurrentMa(uint8_t current_cap)
{
  switch (current_cap)
  {
    case APP_CBL_CUR_3A: return 3000u;
    case APP_CBL_CUR_5A: return 5000u;
    case APP_CBL_CUR_DEFAULT: return 0u;  /* Type-C default, not a PD limit */
    default:             return 0u;
  }
}

uint8_t APP_CBL_Check(const APP_CBL_Info_t *info, uint32_t mv, uint32_t ma,
                      uint8_t want_epr)
{
  uint32_t cap_mv;
  uint32_t cap_ma;

  if ((info == NULL) || (info->valid == 0u))
  {
    return APP_CBL_NO_CABLE;
  }

  cap_mv = APP_CBL_MaxVoltageMv(info->max_vbus);
  if ((mv > cap_mv) && (cap_mv != 0u))
  {
    return APP_CBL_VOLT_LIMIT;
  }

  cap_ma = APP_CBL_MaxCurrentMa(info->current_cap);
  if ((ma > cap_ma) && (cap_ma != 0u))
  {
    return APP_CBL_CURR_LIMIT;
  }

  if ((want_epr != 0u) && (info->epr_capable == 0u))
  {
    return APP_CBL_NOT_EPR;
  }

  return APP_CBL_OK;
}

/* ------------------------------------------------------------------ */
/* Naming                                                              */
/* ------------------------------------------------------------------ */

const char *APP_CBL_SsName(uint8_t ss)
{
  switch (ss)
  {
    case APP_CBL_SS_USB2: return "USB2.0";
    case APP_CBL_SS_GEN1: return "USB3.2 Gen1";
    case APP_CBL_SS_GEN2: return "USB3.2/USB4 Gen2";
    case APP_CBL_SS_GEN3: return "USB4 Gen3";
    default:              return "PROPRIETARY";
  }
}

const char *APP_CBL_TermName(uint8_t term)
{
  switch (term)
  {
    case APP_CBL_TERM_PASSIVE_NOVCONN: return "passive, no VCONN";
    case APP_CBL_TERM_PASSIVE_VCONN:   return "passive, VCONN";
    case APP_CBL_TERM_ONE_EACH:        return "one active end, VCONN";
    case APP_CBL_TERM_BOTH_ACTIVE:     return "both ends active, VCONN";
    default:                           return "UNKNOWN";
  }
}

const char *APP_CBL_ToTypeName(uint8_t to)
{
  switch (to)
  {
    case APP_CBL_TO_A:       return "Type-A";
    case APP_CBL_TO_B:       return "Type-B";
    case APP_CBL_TO_C:       return "Type-C";
    case APP_CBL_TO_CAPTIVE: return "captive";
    default:                 return "UNKNOWN";
  }
}

const char *APP_CBL_ProductTypeName(uint8_t pt)
{
  switch (pt)
  {
    case APP_CBL_PT_UNDEFINED:     return "undefined";
    case APP_CBL_PT_HUB:           return "hub";
    case APP_CBL_PT_PERIPHERAL:    return "peripheral";
    case APP_CBL_PT_PSD:           return "portable power source";
    case APP_CBL_PT_AMA:           return "alt-mode adapter";
    case APP_CBL_PT_ACTIVE_CABLE:  return "active cable";
    case APP_CBL_PT_PASSIVE_CABLE: return "passive cable";
    case APP_CBL_PT_VPD:           return "VCONN powered device";
    default:                       return "UNKNOWN";
  }
}

const char *APP_CBL_VerdictName(uint8_t verdict)
{
  switch (verdict)
  {
    case APP_CBL_OK:         return "ok";
    case APP_CBL_NO_CABLE:   return "no cable identity";
    case APP_CBL_VOLT_LIMIT: return "cable voltage limit";
    case APP_CBL_CURR_LIMIT: return "cable current limit";
    case APP_CBL_NOT_EPR:    return "cable not EPR capable";
    default:                 return "UNKNOWN";
  }
}

void APP_CBL_FormatInfo(const APP_CBL_Info_t *info, char *out, size_t outsz)
{
  if (outsz == 0u)
  {
    return;
  }
  if ((info == NULL) || (info->valid == 0u))
  {
    csnprintf(out, outsz, "no cable identity discovered");
    return;
  }

  csnprintf(out, outsz,
            "%s, %luV, %s, %s, %s%s, latency code %u, fw%u hw%u, vid 0x%04X pid 0x%04X",
            info->active ? "active" : "passive",
            (unsigned long)(APP_CBL_MaxVoltageMv(info->max_vbus) / 1000u),
            (info->current_cap == APP_CBL_CUR_3A) ? "3A" :
            (info->current_cap == APP_CBL_CUR_5A) ? "5A" : "type-C default",
            APP_CBL_SsName(info->ss_support),
            APP_CBL_ToTypeName(info->to_type),
            info->epr_capable ? ", EPR" : "",
            (unsigned)info->latency,
            (unsigned)info->fw_ver, (unsigned)info->hw_ver,
            (unsigned)info->vid, (unsigned)info->pid);
}

