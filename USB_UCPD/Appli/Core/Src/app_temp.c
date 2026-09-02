/**
 * @file    app_temp.c
 * @brief   DTS temperature engine (see app_temp.h).
 */
#include "app_temp.h"
#include "main.h"          /* HAL, DTS_HandleTypeDef hdts */
#include "dts.h"
#include "app_log.h"
#include "app_cap.h"
#include "app_pdcap.h"   /* APP_PDCAP_Cycles */
#include "app_diag.h"

#include <string.h>
#include <stdio.h>

static APP_TEMP_Stat_t s_t;
static uint32_t s_last;

void APP_TEMP_Init(void)
{
  memset(&s_t, 0, sizeof(s_t));

  /* HAL_DTS_GetTemperature() returns degrees C; keep milli-degrees so that the
   * CLI can show one decimal place without floating point. */
  s_t.mc = 0;
  s_t.min_mc = 0;
  s_t.max_mc = 0;
  s_t.sum_mc = 0;
  s_t.started = 0u;
  s_t.error = 0u;

  if (HAL_DTS_Start(&hdts) == HAL_OK)
  {
    s_t.started = 1u;
  }
  else
  {
    s_t.error = 1u;
  }
}

void APP_TEMP_Poll(void)
{
  uint32_t now;
  uint32_t dt;
  int32_t  raw = 0;

  if (s_t.started == 0u)
  {
    return;
  }

  now = APP_PDCAP_Cycles();
  dt = APP_CAP_ElapsedUs(s_last, now, SystemCoreClock);
  /* dt is MICROSECONDS (APP_CAP_ElapsedUs), so it must be compared against
   * APP_TEMP_POLL_US directly.  Dividing the threshold by 1000 compared
   * microseconds against milliseconds and ran the DTS conversion 1000x
   * too often - roughly 10 kHz out of the main loop instead of 10 Hz -
   * which starved USBPD_DPM_Run() and was a hard-reset contributor. */
  if (dt < APP_TEMP_POLL_US)
  {
    return;
  }
  s_last = now;

  if (HAL_DTS_GetTemperature(&hdts, &raw) != HAL_OK)
  {
    s_t.error = 1u;
    return;
  }
  s_t.error = 0u;

  {
    int32_t mc = raw * 1000;

    s_t.mc = mc;
    if (s_t.n == 0u)
    {
      s_t.min_mc = mc;
      s_t.max_mc = mc;
    }
    else
    {
      if (mc < s_t.min_mc) { s_t.min_mc = mc; }
      if (mc > s_t.max_mc) { s_t.max_mc = mc; }
    }
    s_t.sum_mc += mc;
    s_t.n++;
    APP_DIAG_Inc(APP_DIAG_TEMP_SAMPLES);

    if (mc >= APP_TEMP_CRIT_MC)
    {
      s_t.n_alert++;
      APP_DIAG_Inc(APP_DIAG_TEMP_ALERT);
    }
  }
}

int32_t APP_TEMP_Get(void)
{
  return (s_t.n != 0u) ? s_t.mc : -1;
}

void APP_TEMP_GetStats(APP_TEMP_Stat_t *out)
{
  if (out != NULL)
  {
    *out = s_t;
  }
}

void APP_TEMP_Clear(void)
{
  uint8_t started = s_t.started;
  uint8_t error = s_t.error;

  memset(&s_t, 0, sizeof(s_t));
  s_t.started = started;
  s_t.error = error;
}

static void pr_mc(int32_t mc)
{
  APP_LOG_Printf("%ld.%03ld C", (long)(mc / 1000), (long)((mc < 0) ? -(mc % 1000) : (mc % 1000)));
}

int APP_TEMP_Cmd(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "status";
  int32_t avg;

  if (strcmp(sub, "clear") == 0)
  {
    APP_TEMP_Clear();
    APP_LOG_Write("temperature statistics cleared\r\n");
    return 1;
  }
  if (strcmp(sub, "status") != 0)
  {
    APP_LOG_Write("usage: temp [status|clear]\r\n");
    return 1;
  }

  if (s_t.started == 0u)
  {
    APP_LOG_Write("DTS not started (MX_DTS_Init failed?)\r\n");
    return 1;
  }

  avg = (s_t.n != 0u) ? (int32_t)(s_t.sum_mc / (int64_t)s_t.n) : 0;

  APP_LOG_Write("temperature (DTS, LSE reference)\r\n");
  APP_LOG_Write("  current : "); pr_mc(s_t.mc);     APP_LOG_Write("\r\n");
  APP_LOG_Write("  min     : "); pr_mc(s_t.min_mc); APP_LOG_Write("\r\n");
  APP_LOG_Write("  max     : "); pr_mc(s_t.max_mc); APP_LOG_Write("\r\n");
  APP_LOG_Write("  average : "); pr_mc(avg);        APP_LOG_Write("\r\n");
  APP_LOG_Printf("  samples : %lu\r\n", (unsigned long)s_t.n);
  APP_LOG_Printf("  alerts  : %lu (warn %ld.%03ld, crit %ld.%03ld)\r\n",
                 (unsigned long)s_t.n_alert,
                 (long)(APP_TEMP_WARN_MC / 1000), (long)(APP_TEMP_WARN_MC % 1000),
                 (long)(APP_TEMP_CRIT_MC / 1000), (long)(APP_TEMP_CRIT_MC % 1000));
  APP_LOG_Printf("  health  : %s\r\n", s_t.error ? "SENSOR ERROR" : "ok");
  return 1;
}
