/**
 * @file    app_pwr.c
 * @brief   Power / energy analytics (see app_pwr.h).
 *
 * Unit conversions, all exact in integer arithmetic:
 *   P[mW]   = V[mV] * I[uA] / 1e6
 *   E[uWh]  = P[mW] * dt[us] / 3.6e6      (mW * us / 3.6e9 mWh, x1000)
 *   Q[uAh]  = I[uA] * dt[us] / 3.6e6
 */
#include "app_pwr.h"
#include "app_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define PWR_UWH_DIV  3600000LL        /* mW*us  -> uWh */
#define PWR_UAH_DIV  3600000000LL     /* uA*us  -> uAh */

static void pwr_snprintf(char *out, size_t outsz, const char *fmt, ...)
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

static int32_t min32(int32_t a, int32_t b) { return (a < b) ? a : b; }
static int32_t max32(int32_t a, int32_t b) { return (a > b) ? a : b; }

void APP_PWR_Init(APP_PWR_Stat_t *s)
{
  if (s == NULL)
  {
    return;
  }
  memset(s, 0, sizeof(*s));
}

void APP_PWR_Sample(APP_PWR_Stat_t *s, uint32_t dt_us, int32_t mv, int32_t ua)
{
  int32_t mw;
  int32_t dev;

  if (s == NULL)
  {
    return;
  }

  mw = (int32_t)(((int64_t)mv * (int64_t)ua) / 1000000LL);

  if (s->n == 0u)
  {
    s->mv_min = s->mv_max = mv;
    s->ua_min = s->ua_max = ua;
    s->mw_min = s->mw_max = mw;
  }
  else
  {
    s->mv_min = min32(s->mv_min, mv);  s->mv_max = max32(s->mv_max, mv);
    s->ua_min = min32(s->ua_min, ua);  s->ua_max = max32(s->ua_max, ua);
    s->mw_min = min32(s->mw_min, mw);  s->mw_max = max32(s->mw_max, mw);
  }

  s->mv_last = mv;
  s->ua_last = ua;
  s->mw_last = mw;
  s->mv_sum += mv;
  s->ua_sum += ua;
  s->mw_sum += mw;
  s->n++;
  s->span_us += dt_us;

  /* energy and charge integration */
  s->mw_us += (int64_t)mw * (int64_t)dt_us;
  s->ua_us += (int64_t)ua * (int64_t)dt_us;
  s->uwh = s->mw_us / PWR_UWH_DIV;
  s->uah = s->ua_us / PWR_UAH_DIV;

  /* correlation with the negotiated operating point */
  if (s->contract_mv != 0u)
  {
    dev = mv - (int32_t)s->contract_mv;
    if (dev < 0) { dev = -dev; }
    if (dev > s->worst_dev_mv) { s->worst_dev_mv = dev; }
  }
}

void APP_PWR_SetContract(APP_PWR_Stat_t *s, uint32_t mv, uint32_t ma,
                         uint8_t epr)
{
  if (s == NULL)
  {
    return;
  }
  s->contract_mv = mv;
  s->contract_ma = ma;
  s->contract_epr = epr ? 1u : 0u;
  s->worst_dev_mv = 0;
}

void APP_PWR_NoteEvent(APP_PWR_Stat_t *s)
{
  if (s != NULL)
  {
    s->n_events++;
  }
}

int32_t APP_PWR_AvgMv(const APP_PWR_Stat_t *s)
{
  return ((s == NULL) || (s->n == 0u)) ? 0 : (int32_t)(s->mv_sum / (int64_t)s->n);
}

int32_t APP_PWR_AvgUa(const APP_PWR_Stat_t *s)
{
  return ((s == NULL) || (s->n == 0u)) ? 0 : (int32_t)(s->ua_sum / (int64_t)s->n);
}

int32_t APP_PWR_AvgMw(const APP_PWR_Stat_t *s)
{
  return ((s == NULL) || (s->n == 0u)) ? 0 : (int32_t)(s->mw_sum / (int64_t)s->n);
}

void APP_PWR_Format(const APP_PWR_Stat_t *s, char *out, size_t outsz)
{
  if (outsz == 0u)
  {
    return;
  }
  if ((s == NULL) || (s->n == 0u))
  {
    pwr_snprintf(out, outsz, "no samples");
    return;
  }

  pwr_snprintf(out, outsz,
    "V %d.%03d (min %d.%03d max %d.%03d avg %d.%03d) mV; "
    "I %d.%03d (min %d.%03d max %d.%03d) mA; "
    "P %d.%03d (max %d.%03d) W; E %lu.%03lu Wh; Q %lu.%03lu Ah; %lu samples",
    (int)(s->mv_last / 1000), (int)((s->mv_last % 1000 < 0 ? -s->mv_last % 1000 : s->mv_last % 1000)),
    (int)(s->mv_min / 1000), (int)(s->mv_min % 1000),
    (int)(s->mv_max / 1000), (int)(s->mv_max % 1000),
    (int)(APP_PWR_AvgMv(s) / 1000), (int)(APP_PWR_AvgMv(s) % 1000),
    (int)(s->ua_last / 1000), (int)(s->ua_last % 1000),
    (int)(s->ua_min / 1000), (int)(s->ua_min % 1000),
    (int)(s->ua_max / 1000), (int)(s->ua_max % 1000),
    (int)(s->mw_last / 1000), (int)(s->mw_last % 1000),
    (int)(s->mw_max / 1000), (int)(s->mw_max % 1000),
    (unsigned long)(s->uwh / 1000000LL),
    (unsigned long)((s->uwh % 1000000LL) / 1000LL),
    (unsigned long)(s->uah / 1000000LL),
    (unsigned long)((s->uah % 1000000LL) / 1000LL),
    (unsigned long)s->n);
}

