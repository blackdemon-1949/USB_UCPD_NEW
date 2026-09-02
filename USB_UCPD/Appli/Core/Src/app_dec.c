/**
 * @file    app_dec.c
 * @brief   USB Power Delivery message decoder (see app_dec.h).
 *
 * Message type tables are taken from
 * Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_def.h:
 *   - USBPD_ControlMsg_TypeDef  is 1-based internally (0 means "none"), so the
 *     wire value is the enum value minus one.
 *   - USBPD_DataMsg_TypeDef and USBPD_ExtendedMsg_TypeDef are wire-valued.
 */
#include "app_dec.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Static name tables                                                 */
/* ------------------------------------------------------------------ */

/* Wire value == index.  USB PD 3.1 Table 6-5 (Control Messages), which
 * USBPD_ControlMsg_TypeDef in usbpd_def.h also follows exactly: GoodCRC is
 * 0x01, Accept 0x03, PS_RDY 0x06, Wait 0x0C, Soft_Reset 0x0D.  Index 0 is not
 * a legal control message type, so it is a placeholder.  An earlier version of
 * this table started at GoodCRC = 0 and therefore named every control message
 * one type too low. */
static const char *const s_ctrl[] =
{
  "RESERVED_0",           /* 0  - not a legal control type */
  "GoodCRC",              /* 1  */
  "GotoMin",              /* 1  */
  "Accept",               /* 2  */
  "Reject",               /* 3  */
  "Ping",                 /* 4  */
  "PS_RDY",               /* 5  */
  "Get_Source_Cap",       /* 6  */
  "Get_Sink_Cap",         /* 7  */
  "DR_Swap",              /* 8  */
  "PR_Swap",              /* 9  */
  "VCONN_Swap",           /* 10 */
  "Wait",                 /* 11 */
  "Soft_Reset",           /* 12 */
  "Data_Reset",           /* 13 */
  "Data_Reset_Complete",  /* 14 */
  "Not_Supported",        /* 15 */
  "Get_Source_Cap_Ext",   /* 16 */
  "Get_Status",           /* 17 */
  "FR_Swap",              /* 18 */
  "Get_PPS_Status",       /* 19 */
  "Get_Country_Codes",    /* 20 */
  "Get_Sink_Cap_Ext",     /* 21 */
  "Get_Source_Info",      /* 22 */
  "Get_Revision"          /* 23 */
};
#define APP_DEC_CTRL_MAX  (sizeof(s_ctrl) / sizeof(s_ctrl[0]))

/* Wire value == index.  USB PD 3.1 Table 6-6 (Data Messages). */
static const char *const s_data[] =
{
  "Reserved(0)",          /* 0  */
  "Source_Capabilities",  /* 1  */
  "Request",              /* 2  */
  "BIST",                 /* 3  */
  "Sink_Capabilities",    /* 4  */
  "Battery_Status",       /* 5  */
  "Alert",                /* 6  */
  "Get_Country_Info",     /* 7  */
  "Enter_USB",            /* 8  */
  "EPR_Request",          /* 9  */
  "EPR_Mode",             /* 10 */
  "Source_Info",          /* 11 */
  "Revision",             /* 12 */
  "Reserved(13)",         /* 13 */
  "Reserved(14)",         /* 14 */
  "Vendor_Defined"        /* 15 */
};
#define APP_DEC_DATA_MAX  (sizeof(s_data) / sizeof(s_data[0]))

/* Wire value == index.  USBPD_ExtendedMsg_TypeDef (wire-valued). */
static const char *const s_ext[] =
{
  "Reserved(0)",              /* 0  */
  "Source_Capabilities_Ext",  /* 1  */
  "Status",                   /* 2  */
  "Get_Battery_Cap",          /* 3  */
  "Get_Battery_Status",       /* 4  */
  "Battery_Capabilities",     /* 5  */
  "Get_Manufacturer_Info",    /* 6  */
  "Manufacturer_Info",        /* 7  */
  "Security_Request",         /* 8  */
  "Security_Response",        /* 9  */
  "Firm_Update_Request",      /* 10 */
  "Firm_Update_Response",     /* 11 */
  "PPS_Status",               /* 12 */
  "Country_Info",             /* 13 */
  "Country_Codes",            /* 14 */
  "Sink_Capabilities_Ext",    /* 15 */
  "Control",                  /* 16 */
  "EPR_Source_Capabilities",  /* 17 */
  "EPR_Sink_Capabilities"     /* 18 */
};
#define APP_DEC_EXT_MAX  (sizeof(s_ext) / sizeof(s_ext[0]))

static const char *const s_spec[] = { "1.0", "2.0", "3.0", "3.x" };

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void safe_snprintf(char *out, size_t outsz, const char *fmt, ...)
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

/** Like safe_snprintf() but returns how many characters are now in @p out,
 *  so callers can append without walking the string again. */
static size_t safe_snprintf_len(char *out, size_t outsz, const char *fmt, ...)
{
  va_list ap;
  if (outsz == 0u)
  {
    return 0u;
  }
  va_start(ap, fmt);
  (void)vsnprintf(out, outsz, fmt, ap);
  va_end(ap);
  out[outsz - 1u] = '\0';
  return strlen(out);
}

/* ------------------------------------------------------------------ */
/* Decoding                                                           */
/* ------------------------------------------------------------------ */

int APP_DEC_Decode(const uint8_t *msg, uint16_t len, APP_DEC_Msg_t *out)
{
  uint16_t need;

  if ((msg == NULL) || (out == NULL))
  {
    return -1;
  }
  memset(out, 0, sizeof(*out));

  if (len < 2u)
  {
    out->flags = APP_DEC_F_SHORT;
    out->msg_class = APP_DEC_CLASS_INVALID;
    return 0;
  }

  out->raw_header = rd16(msg);
  out->msg_type    = APP_DEC_HDR_TYPE(out->raw_header);
  out->power_role  = APP_DEC_HDR_POWER_ROLE(out->raw_header);
  out->spec_rev    = APP_DEC_HDR_SPEC_REV(out->raw_header);
  out->data_role   = APP_DEC_HDR_DATA_ROLE(out->raw_header);
  out->msg_id      = APP_DEC_HDR_MSG_ID(out->raw_header);
  out->num_obj     = APP_DEC_HDR_NUM_OBJ(out->raw_header);
  out->extended    = APP_DEC_HDR_EXTENDED(out->raw_header);
  out->data_offset = 2u;

  if (out->extended != 0u)
  {
    if (len < 4u)
    {
      out->flags |= APP_DEC_F_EXT_SHORT;
      out->msg_class = APP_DEC_CLASS_EXTENDED;
      return 0;
    }
    out->ext_header    = rd16(&msg[2]);
    out->ext_data_size = APP_DEC_EXTHDR_SIZE(out->ext_header);
    out->ext_chunked   = APP_DEC_EXTHDR_CHUNKED(out->ext_header);
    out->ext_chunk_num = APP_DEC_EXTHDR_CHUNK(out->ext_header);
    out->ext_req_chunk = APP_DEC_EXTHDR_REQCHUNK(out->ext_header);
    out->data_offset   = 4u;
    out->msg_class     = APP_DEC_CLASS_EXTENDED;
  }
  else if (out->num_obj == 0u)
  {
    out->msg_class = APP_DEC_CLASS_CONTROL;
    if (out->msg_type >= (uint8_t)APP_DEC_CTRL_MAX)
    {
      out->flags |= APP_DEC_F_TYPE_RSV;
    }
  }
  else
  {
    out->msg_class = APP_DEC_CLASS_DATA;
    if ((out->msg_type >= (uint8_t)APP_DEC_DATA_MAX) ||
        (out->msg_type == 0u) || (out->msg_type == 13u) || (out->msg_type == 14u))
    {
      out->flags |= APP_DEC_F_TYPE_RSV;
    }
  }

  /* how many payload bytes are available after the (extended) header */
  if (len > out->data_offset)
  {
    uint32_t avail = (uint32_t)len - (uint32_t)out->data_offset;
    out->data_len = (uint8_t)((avail > 0xFFu) ? 0xFFu : avail);
    out->data = &msg[out->data_offset];
  }

  /* structural consistency ------------------------------------------- */
  (void)need;
  if (out->extended != 0u)
  {
    if ((uint16_t)out->data_len < out->ext_data_size)
    {
      /* Either the frame is genuinely short, or the capture clipped it. */
      out->flags |= APP_DEC_F_TRUNCATED;
    }
    if (out->ext_data_size > 260u)
    {
      out->flags |= APP_DEC_F_EXT_SIZE;
    }
  }
  else
  {
    need = (uint16_t)((uint16_t)out->num_obj * 4u);
    if (out->msg_class == APP_DEC_CLASS_CONTROL)
    {
      if (out->num_obj != 0u)
      {
        out->flags |= APP_DEC_F_CTRL_DO;
      }
    }
    else
    {
      if (out->data_len < need)
      {
        out->flags |= APP_DEC_F_NUMOBJ;
      }
    }
  }

  return 0;
}

uint16_t APP_DEC_FrameSize(const uint8_t *msg, uint16_t len)
{
  if (len < 2u)
  {
    return 0u;
  }
  if (APP_DEC_HDR_EXTENDED(rd16(msg)) != 0u)
  {
    if (len < 4u)
    {
      return 0u;
    }
    /* header + extended header + declared data size */
    return (uint16_t)(4u + APP_DEC_EXTHDR_SIZE(rd16(&msg[2])));
  }
  return (uint16_t)(2u + ((uint16_t)APP_DEC_HDR_NUM_OBJ(rd16(msg)) * 4u));
}

/* ------------------------------------------------------------------ */
/* Naming                                                             */
/* ------------------------------------------------------------------ */

const char *APP_DEC_SopName(uint8_t sop)
{
  switch (sop)
  {
    case 0u:   return "SOP";
    case 1u:   return "SOP'";
    case 2u:   return "SOP''";
    case 3u:   return "SOP'-Debug";
    case 4u:   return "SOP''-Debug";
    case 0xFFu: return "-";
    default:   return "SOP?";
  }
}

const char *APP_DEC_ClassName(uint8_t msg_class)
{
  switch (msg_class)
  {
    case APP_DEC_CLASS_CONTROL:  return "CTRL";
    case APP_DEC_CLASS_DATA:     return "DATA";
    case APP_DEC_CLASS_EXTENDED: return "EXT";
    default:                     return "INV";
  }
}

const char *APP_DEC_SpecRevName(uint8_t rev)
{
  return (rev < 4u) ? s_spec[rev] : "?";
}

const char *APP_DEC_ControlName(uint8_t type)
{
  return (type < (uint8_t)APP_DEC_CTRL_MAX) ? s_ctrl[type] : "Reserved";
}

const char *APP_DEC_DataName(uint8_t type)
{
  return (type < (uint8_t)APP_DEC_DATA_MAX) ? s_data[type] : "Reserved";
}

const char *APP_DEC_ExtendedName(uint8_t type)
{
  return (type < (uint8_t)APP_DEC_EXT_MAX) ? s_ext[type] : "Reserved";
}

const char *APP_DEC_MsgName(const APP_DEC_Msg_t *m)
{
  if (m == NULL)
  {
    return "?";
  }
  switch (m->msg_class)
  {
    case APP_DEC_CLASS_CONTROL:  return APP_DEC_ControlName(m->msg_type);
    case APP_DEC_CLASS_DATA:     return APP_DEC_DataName(m->msg_type);
    case APP_DEC_CLASS_EXTENDED: return APP_DEC_ExtendedName(m->msg_type);
    default:                     return "Malformed";
  }
}

/* ------------------------------------------------------------------ */
/* PDO / RDO formatting                                               */
/* ------------------------------------------------------------------ */

int APP_DEC_PdoFixedToMvMa(uint32_t pdo, uint32_t *mv, uint32_t *ma)
{
  if (APP_DEC_PDO_KIND(pdo) != APP_DEC_PDO_FIXED)
  {
    return 0;
  }
  if (mv != NULL) { *mv = APP_DEC_PDO_FIXED_VOLT(pdo) * 50u; }
  if (ma != NULL) { *ma = APP_DEC_PDO_FIXED_CURR(pdo) * 10u; }
  return 1;
}

int APP_DEC_PdoPpsToRange(uint32_t pdo, uint32_t *min_mv, uint32_t *max_mv,
                          uint32_t *max_ma)
{
  if ((APP_DEC_PDO_KIND(pdo) != APP_DEC_PDO_APDO) ||
      (APP_DEC_APDO_SUBTYPE(pdo) != APP_DEC_APDO_PPS))
  {
    return 0;
  }
  if (min_mv != NULL) { *min_mv = APP_DEC_APDO_PPS_MINVOLT(pdo) * 100u; }
  if (max_mv != NULL) { *max_mv = APP_DEC_APDO_PPS_MAXVOLT(pdo) * 100u; }
  if (max_ma != NULL) { *max_ma = APP_DEC_APDO_PPS_MAXCURR(pdo) * 50u; }
  return 1;
}

void APP_DEC_FormatPdo(uint32_t pdo, char *out, size_t outsz)
{
  char caps[40];
  size_t n = 0u;

  if (outsz == 0u)
  {
    return;
  }
  caps[0] = '\0';

  switch (APP_DEC_PDO_KIND(pdo))
  {
    case APP_DEC_PDO_FIXED:
      if (APP_DEC_PDO_FIXED_DRP(pdo))     { caps[n++] = 'D'; }  /* dual-role power */
      if (APP_DEC_PDO_FIXED_SUSPEND(pdo)) { caps[n++] = 'S'; }  /* USB suspend     */
      if (APP_DEC_PDO_FIXED_EXTPWR(pdo))  { caps[n++] = 'E'; }  /* unconstrained   */
      if (APP_DEC_PDO_FIXED_USBCOMM(pdo)) { caps[n++] = 'C'; }  /* USB comms       */
      if (APP_DEC_PDO_FIXED_DRD(pdo))     { caps[n++] = 'd'; }  /* dual-role data  */
      if (APP_DEC_PDO_FIXED_UNCHUNK(pdo)) { caps[n++] = 'U'; }  /* unchunked ext   */
      if (APP_DEC_PDO_FIXED_EPR(pdo))     { caps[n++] = 'P'; }  /* EPR capable     */
      caps[n] = '\0';
      safe_snprintf(out, outsz, "fixed %u.%02uV %u.%02uA%s%s",
                    (unsigned)(APP_DEC_PDO_FIXED_VOLT(pdo) * 50u / 1000u),
                    (unsigned)((APP_DEC_PDO_FIXED_VOLT(pdo) * 50u / 10u) % 100u),
                    (unsigned)(APP_DEC_PDO_FIXED_CURR(pdo) * 10u / 1000u),
                    (unsigned)((APP_DEC_PDO_FIXED_CURR(pdo) * 10u / 10u) % 100u),
                    (caps[0] != '\0') ? " " : "", caps);
      break;

    case APP_DEC_PDO_BATTERY:
      safe_snprintf(out, outsz, "battery %u.%02u-%u.%02uV %u.%02uW",
                    (unsigned)(APP_DEC_PDO_VAR_MINVOLT(pdo) * 50u / 1000u),
                    (unsigned)((APP_DEC_PDO_VAR_MINVOLT(pdo) * 50u / 10u) % 100u),
                    (unsigned)(APP_DEC_PDO_VAR_MAXVOLT(pdo) * 50u / 1000u),
                    (unsigned)((APP_DEC_PDO_VAR_MAXVOLT(pdo) * 50u / 10u) % 100u),
                    (unsigned)(APP_DEC_PDO_BATT_MAXPWR(pdo) * 250u / 1000u),
                    (unsigned)((APP_DEC_PDO_BATT_MAXPWR(pdo) * 250u / 100u) % 10u));
      break;

    case APP_DEC_PDO_VARIABLE:
      safe_snprintf(out, outsz, "variable %u.%02u-%u.%02uV %u.%02uA",
                    (unsigned)(APP_DEC_PDO_VAR_MINVOLT(pdo) * 50u / 1000u),
                    (unsigned)((APP_DEC_PDO_VAR_MINVOLT(pdo) * 50u / 10u) % 100u),
                    (unsigned)(APP_DEC_PDO_VAR_MAXVOLT(pdo) * 50u / 1000u),
                    (unsigned)((APP_DEC_PDO_VAR_MAXVOLT(pdo) * 50u / 10u) % 100u),
                    (unsigned)(APP_DEC_PDO_FIXED_CURR(pdo) * 10u / 1000u),
                    (unsigned)((APP_DEC_PDO_FIXED_CURR(pdo) * 10u / 10u) % 100u));
      break;

    default:  /* APDO */
      if (APP_DEC_APDO_SUBTYPE(pdo) == APP_DEC_APDO_PPS)
      {
        safe_snprintf(out, outsz, "pps %u.%u-%u.%uV %u.%02uA%s",
                      (unsigned)(APP_DEC_APDO_PPS_MINVOLT(pdo) / 10u),
                      (unsigned)(APP_DEC_APDO_PPS_MINVOLT(pdo) % 10u),
                      (unsigned)(APP_DEC_APDO_PPS_MAXVOLT(pdo) / 10u),
                      (unsigned)(APP_DEC_APDO_PPS_MAXVOLT(pdo) % 10u),
                      (unsigned)(APP_DEC_APDO_PPS_MAXCURR(pdo) * 50u / 1000u),
                      (unsigned)((APP_DEC_APDO_PPS_MAXCURR(pdo) * 50u / 10u) % 100u),
                      APP_DEC_APDO_PPS_PPS_POWER_LIMITED(pdo) ? " pwr-ltd" : "");
      }
      else
      {
        safe_snprintf(out, outsz, "apdo subtype %u raw 0x%08lX",
                      (unsigned)APP_DEC_APDO_SUBTYPE(pdo),
                      (unsigned long)pdo);
      }
      break;
  }
}

void APP_DEC_FormatRdo(uint32_t rdo, char *out, size_t outsz)
{
  if (outsz == 0u)
  {
    return;
  }
  if (APP_DEC_RDO_BATTERY(rdo) != 0u)
  {
    safe_snprintf(out, outsz, "req pos%u op %u.%uW max %u.%uW%s%s%s",
                  (unsigned)APP_DEC_RDO_POS(rdo),
                  (unsigned)(APP_DEC_RDO_OP_PWR(rdo) * 250u / 1000u),
                  (unsigned)((APP_DEC_RDO_OP_PWR(rdo) * 250u / 100u) % 10u),
                  (unsigned)(APP_DEC_RDO_MAX_PWR(rdo) * 250u / 1000u),
                  (unsigned)((APP_DEC_RDO_MAX_PWR(rdo) * 250u / 100u) % 10u),
                  APP_DEC_RDO_GIVEBACK(rdo) ? " giveback" : "",
                  APP_DEC_RDO_EPR_MODE(rdo) ? " epr" : "",
                  APP_DEC_RDO_CAP_MISMATCH(rdo) ? " mismatch" : "");
  }
  else
  {
    safe_snprintf(out, outsz, "req pos%u op %u.%02uA max %u.%02uA%s%s%s",
                  (unsigned)APP_DEC_RDO_POS(rdo),
                  (unsigned)(APP_DEC_RDO_OP_CURR(rdo) * 10u / 1000u),
                  (unsigned)((APP_DEC_RDO_OP_CURR(rdo) * 10u / 10u) % 100u),
                  (unsigned)(APP_DEC_RDO_MAX_CURR(rdo) * 10u / 1000u),
                  (unsigned)((APP_DEC_RDO_MAX_CURR(rdo) * 10u / 10u) % 100u),
                  APP_DEC_RDO_GIVEBACK(rdo) ? " giveback" : "",
                  APP_DEC_RDO_EPR_MODE(rdo) ? " epr" : "",
                  APP_DEC_RDO_NO_SUSPEND(rdo) ? " nosusp" : "");
  }
}

void APP_DEC_FormatFlags(uint8_t flags, char *out, size_t outsz)
{
  static const struct { uint8_t bit; const char *tag; } map[] =
  {
    { APP_DEC_F_SHORT,      "short"      },
    { APP_DEC_F_NUMOBJ,     "numobj"     },
    { APP_DEC_F_EXT_SHORT,  "extshort"   },
    { APP_DEC_F_EXT_SIZE,   "extsize"    },
    { APP_DEC_F_CTRL_DO,    "ctrl+do"    },
    { APP_DEC_F_DATA_NODO,  "data-nodo"  },
    { APP_DEC_F_TYPE_RSV,   "rsvd-type"  },
    { APP_DEC_F_TRUNCATED,  "truncated"  }
  };
  size_t n = 0u;
  size_t i;

  if (outsz == 0u)
  {
    return;
  }
  out[0] = '\0';
  for (i = 0u; i < (sizeof(map) / sizeof(map[0])); i++)
  {
    if ((flags & map[i].bit) != 0u)
    {
      size_t t = strlen(map[i].tag);
      if ((n + t + 2u) >= outsz)
      {
        break;
      }
      if (n != 0u) { out[n++] = ','; }
      memcpy(&out[n], map[i].tag, t);
      n += t;
    }
  }
  out[n] = '\0';
}

void APP_DEC_FormatFrame(const uint8_t *msg, uint16_t len, char *out, size_t outsz)
{
  APP_DEC_Msg_t m;
  char fl[48];
  size_t n;

  if (outsz == 0u)
  {
    return;
  }
  if (APP_DEC_Decode(msg, len, &m) != 0)
  {
    safe_snprintf(out, outsz, "?");
    return;
  }

  APP_DEC_FormatFlags(m.flags, fl, sizeof(fl));

  n = (size_t)safe_snprintf_len(out, outsz, "%s id%u %s rev%s ndo%u",
                                APP_DEC_ClassName(m.msg_class),
                                (unsigned)m.msg_id,
                                APP_DEC_MsgName(&m),
                                APP_DEC_SpecRevName(m.spec_rev),
                                (unsigned)m.num_obj);
  (void)n;

  if (m.extended != 0u)
  {
    n += (size_t)safe_snprintf_len(out + n, (n < outsz) ? (outsz - n) : 1u,
                                   " dsz%u%s%u%s",
                                   (unsigned)m.ext_data_size,
                                   m.ext_chunked ? " ch" : " unchunked",
                                   (unsigned)m.ext_chunk_num,
                                   m.ext_req_chunk ? " req" : "");
  }

  /* decode the first data object for the most interesting messages */
  if ((m.msg_class == APP_DEC_CLASS_DATA) && (m.data_len >= 4u))
  {
    uint32_t o0 = (uint32_t)m.data[0] | ((uint32_t)m.data[1] << 8) |
                  ((uint32_t)m.data[2] << 16) | ((uint32_t)m.data[3] << 24);
    char sub[96];
    if (m.msg_type == 1u)       { APP_DEC_FormatPdo(o0, sub, sizeof(sub)); }
    else if (m.msg_type == 2u)  { APP_DEC_FormatRdo(o0, sub, sizeof(sub)); }
    else if (m.msg_type == 4u)  { APP_DEC_FormatPdo(o0, sub, sizeof(sub)); }
    else                        { safe_snprintf(sub, sizeof(sub), "do0 0x%08lX",
                                                (unsigned long)o0); }
    n += (size_t)safe_snprintf_len(out + n, (n < outsz) ? (outsz - n) : 1u,
                                   " | %s", sub);
  }

  if (fl[0] != '\0')
  {
    n += (size_t)safe_snprintf_len(out + n, (n < outsz) ? (outsz - n) : 1u,
                                   " !%s", fl);
  }
  if (n < outsz)
  {
    out[n] = '\0';
  }
}
