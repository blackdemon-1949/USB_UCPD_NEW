/**
 * @file    app_pps.c
 * @brief   PPS analysis and request validation (see app_pps.h).
 */
#include "app_pps.h"
#include "app_dec.h"
#include "app_log.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static APP_PPS_Set_t s_set;

static void pps_snprintf(char *out, size_t outsz, const char *fmt, ...)
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

int APP_PPS_IsApdo(uint32_t pdo)
{
  /* Object type 11b is the Augmented PDO; subtype 00b is PPS. */
  return ((APP_DEC_PDO_KIND(pdo) == APP_DEC_PDO_APDO) &&
          (APP_DEC_APDO_SUBTYPE(pdo) == APP_DEC_APDO_PPS)) ? 1 : 0;
}

int APP_PPS_Parse(uint32_t pdo, uint8_t pos, APP_PPS_Window_t *out)
{
  if ((out == NULL) || (pos == 0u))
  {
    return 0;
  }
  if (APP_PPS_IsApdo(pdo) == 0)
  {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  out->pos = pos;
  out->max_ma = APP_DEC_APDO_PPS_MAXCURR(pdo) * 50u;   /* 50 mA units  */
  out->min_mv = APP_DEC_APDO_PPS_MINVOLT(pdo) * 100u;  /* 100 mV units */
  out->max_mv = APP_DEC_APDO_PPS_MAXVOLT(pdo) * 100u;
  out->power_limited = APP_DEC_APDO_PPS_PPS_POWER_LIMITED(pdo);
  return 1;
}

void APP_PPS_Analyse(const uint32_t *pdo, uint8_t count, APP_PPS_Set_t *out)
{
  uint8_t i;

  if (out == NULL)
  {
    return;
  }
  memset(out, 0, sizeof(*out));
  if ((pdo == NULL) || (count == 0u))
  {
    return;
  }
  if (count > APP_PPS_MAX_WINDOWS)
  {
    count = APP_PPS_MAX_WINDOWS;
  }

  for (i = 0u; i < count; i++)
  {
    APP_PPS_Window_t w;
    uint32_t pdp_mw;

    if (APP_PPS_Parse(pdo[i], (uint8_t)(i + 1u), &w) == 0)
    {
      continue;
    }
    if (w.max_mv < w.min_mv)
    {
      continue;                 /* malformed APDO - do not publish it */
    }

    out->w[out->n] = w;
    out->n++;

    if ((out->span_min_mv == 0u) || (w.min_mv < out->span_min_mv))
    {
      out->span_min_mv = w.min_mv;
    }
    if (w.max_mv > out->span_max_mv)
    {
      out->span_max_mv = w.max_mv;
    }
    if (w.max_ma > out->span_max_ma)
    {
      out->span_max_ma = w.max_ma;
    }

    pdp_mw = w.max_mv * (w.max_ma / 1000u);   /* mV * A = mW */
    if (pdp_mw > out->max_pdp_mw)
    {
      out->max_pdp_mw = pdp_mw;
    }
  }
}

uint8_t APP_PPS_Validate(const APP_PPS_Window_t *w, uint32_t mv, uint32_t ma)
{
  uint32_t pdp_mw;

  if (w == NULL)
  {
    return APP_PPS_NO_WINDOW;
  }
  if (mv < w->min_mv)
  {
    return APP_PPS_BELOW_MIN;
  }
  if (mv > w->max_mv)
  {
    return APP_PPS_ABOVE_MAX;
  }
  if (ma > w->max_ma)
  {
    return APP_PPS_OVER_CURR;
  }

  /* A power-limited APDO can still refuse a V*I product beyond its rating. */
  pdp_mw = mv * (ma / 1000u);
  if ((w->power_limited != 0u) && (w->max_mv != 0u))
  {
    uint32_t limit_mw = w->max_mv * (w->max_ma / 1000u);

    if (pdp_mw > limit_mw)
    {
      return APP_PPS_POWER_LIMIT;
    }
  }
  return APP_PPS_OK;
}

uint32_t APP_PPS_BuildRdo(uint8_t pos, uint32_t mv, uint32_t ma,
                          uint8_t unchunked, uint8_t usb_comm)
{
  uint32_t rdo;
  uint32_t v;
  uint32_t c;

  if ((pos == 0u) || (pos > 15u))
  {
    return 0u;
  }
  if ((mv == 0u) || (ma == 0u))
  {
    return 0u;
  }

  v = mv / 20u;      /* 20 mV units, 12 bits */
  c = ma / 50u;      /* 50 mA units, 7 bits  */
  if ((v == 0u) || (v > 0xFFFu) || (c == 0u) || (c > 0x7Fu))
  {
    return 0u;
  }

  rdo = ((uint32_t)pos & 0xFu) << 28;
  rdo |= ((unchunked != 0u) ? 1u : 0u) << 27;
  rdo |= ((usb_comm != 0u) ? 1u : 0u) << 26;
  rdo |= (v & 0xFFFu) << 9;
  rdo |= (c & 0x7Fu);
  return rdo;
}

const char *APP_PPS_VerdictName(uint8_t verdict)
{
  switch (verdict)
  {
    case APP_PPS_OK:          return "ok";
    case APP_PPS_NO_WINDOW:   return "no PPS window";
    case APP_PPS_BELOW_MIN:   return "voltage below window minimum";
    case APP_PPS_ABOVE_MAX:   return "voltage above window maximum";
    case APP_PPS_OVER_CURR:   return "current above window maximum";
    case APP_PPS_POWER_LIMIT: return "V*I exceeds the APDO rating";
    default:                  return "?";
  }
}

void APP_PPS_FormatWindow(const APP_PPS_Window_t *w, char *out, size_t outsz)
{
  if ((out == NULL) || (outsz == 0u) || (w == NULL))
  {
    return;
  }
  pps_snprintf(out, outsz, "%lu.%lu-%lu.%luV %lu mA%s",
               (unsigned long)(w->min_mv / 1000u),
               (unsigned long)((w->min_mv / 100u) % 10u),
               (unsigned long)(w->max_mv / 1000u),
               (unsigned long)((w->max_mv / 100u) % 10u),
               (unsigned long)w->max_ma,
               (w->power_limited != 0u) ? " power-limited" : "");
}

/* ------------------------------------------------------------------ */
/* Target glue                                                         */
/* ------------------------------------------------------------------ */

void APP_PPS_OnSrcPdo(const uint8_t *ptr, uint32_t size)
{
  uint32_t pdo[APP_PPS_MAX_WINDOWS];
  uint32_t n;
  uint32_t i;

  if ((ptr == NULL) || (size < 4u))
  {
    return;
  }
  n = size / 4u;
  if (n > APP_PPS_MAX_WINDOWS)
  {
    n = APP_PPS_MAX_WINDOWS;
  }

  for (i = 0u; i < n; i++)
  {
    pdo[i] = (uint32_t)ptr[i * 4u] | ((uint32_t)ptr[i * 4u + 1u] << 8) |
             ((uint32_t)ptr[i * 4u + 2u] << 16) | ((uint32_t)ptr[i * 4u + 3u] << 24);
  }
  APP_PPS_Analyse(pdo, (uint8_t)n, &s_set);
}

const APP_PPS_Set_t *APP_PPS_Get(void)
{
  return &s_set;
}

int APP_PPS_Cmd(int argc, char *argv[])
{
  char line[80];
  uint32_t i;

  if ((argc >= 4) && (strcmp(argv[1], "check") == 0))
  {
    unsigned mv = 0u;
    unsigned ma = 0u;
    uint8_t v;

    if ((sscanf(argv[2], "%u", &mv) != 1) || (sscanf(argv[3], "%u", &ma) != 1))
    {
      APP_LOG_Write("usage: pps check <mv> <ma>\r\n");
      return 1;
    }
    v = APP_PPS_NO_WINDOW;
    for (i = 0u; i < s_set.n; i++)
    {
      v = APP_PPS_Validate(&s_set.w[i], mv, ma);
      if (v == APP_PPS_OK)
      {
        break;
      }
    }
    APP_LOG_Printf("pps check %u mV / %u mA -> %s\r\n", mv, ma,
                   APP_PPS_VerdictName(v));
    return 1;
  }

  if ((argc >= 4) && (strcmp(argv[1], "rdo") == 0))
  {
    unsigned pos = 0u;
    unsigned mv = 0u;
    unsigned ma = 0u;
    uint32_t rdo;

    if ((sscanf(argv[2], "%u", &pos) != 1) ||
        (sscanf(argv[3], "%u", &mv) != 1) ||
        ((argc >= 5) && (sscanf(argv[4], "%u", &ma) != 1)))
    {
      APP_LOG_Write("usage: pps rdo <pos> <mv> [ma]\r\n");
      return 1;
    }
    if (ma == 0u) { ma = 1000u; }
    rdo = APP_PPS_BuildRdo((uint8_t)pos, mv, ma, 1u, 1u);
    if (rdo == 0u)
    {
      APP_LOG_Write("invalid PPS RDO parameters\r\n");
      return 1;
    }
    APP_LOG_Printf("PPS RDO 0x%08lX  pos %lu  %lu mV  %lu mA\r\n",
                   (unsigned long)rdo, (unsigned long)APP_PPS_RDO_POS(rdo),
                   (unsigned long)APP_PPS_RDO_VOLT(rdo),
                   (unsigned long)APP_PPS_RDO_CURR(rdo));
    return 1;
  }

  if ((argc >= 2) && (strcmp(argv[1], "status") != 0))
  {
    APP_LOG_Write("usage: pps [status|check <mv> <ma>|rdo <pos> <mv> [ma]]\r\n");
    return 1;
  }

  APP_LOG_Printf("PPS windows advertised: %u\r\n", (unsigned)s_set.n);
  if (s_set.n == 0u)
  {
    APP_LOG_Write("  the source advertises no PPS APDO\r\n");
    return 1;
  }
  for (i = 0u; i < s_set.n; i++)
  {
    APP_PPS_FormatWindow(&s_set.w[i], line, sizeof(line));
    APP_LOG_Printf("  [%lu] %s\r\n", (unsigned long)s_set.w[i].pos, line);
  }
  APP_LOG_Printf("  reachable: %lu-%lu mV, up to %lu mA, %lu mW peak\r\n",
                 (unsigned long)s_set.span_min_mv,
                 (unsigned long)s_set.span_max_mv,
                 (unsigned long)s_set.span_max_ma,
                 (unsigned long)s_set.max_pdp_mw);
  return 1;
}
