/**
  ******************************************************************************
  * @file    ina226_energy.c
  * @brief   INA226 mAh / mWh energy accumulator.
  *
  * Every time the INA226 driver produces a fresh sample (ina_sample() in
  * ina226.c), INA226_Energy_OnSample() is called with the bus voltage and
  * current.  The elapsed time since the previous sample is used to weight
  * the contribution to mAh and mWh, so accumulation is correct even if
  * the sample interval jitters or the chip drops out briefly.
  *
  * The accumulator uses 64-bit math for the running uAs/uWs totals so the
  * running sums cannot overflow at practical currents/voltages over
  * multi-hour sessions (at 5 A / 28 V the 64-bit uAs counter wraps after
  * ~90 million years).
  *
  * Checkpoints every INA226_ENERGY_CHECKPOINT_MS to the SD card path
  * "0:/energy.log" using EXT_SD_AppendLine(); the file is opened/appended/
  * closed per checkpoint.  Never touches NOR flash (FLASH_ENDURANCE.md).
  ******************************************************************************
  */
#include "ina226_energy.h"
#include "ina226.h"
#include "ext_sd.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 *  Tunables
 * ----------------------------------------------------------------------- */
#define INA226_ENERGY_CHECKPOINT_MS    60000U  /* one CSV line per minute */
#define INA226_ENERGY_PATH             "0:/energy.log"

/* -------------------------------------------------------------------------
 *  Running state
 * ----------------------------------------------------------------------- */
typedef struct
{
  int64_t  uas;            /* microampere-seconds (signed) */
  int64_t  uws;            /* microwatt-seconds (signed) */
  uint32_t total_ms;       /* integrated wall-clock ms */
  uint32_t last_tick;      /* HAL_GetTick() of last integrated sample */
  uint8_t  have_sample;    /* 1 after first OnSample() */
  uint32_t next_checkpoint;
  uint32_t checkpoint_lines;
} EnergyState_t;

static EnergyState_t s;

void INA226_Energy_Init(void)
{
  memset(&s, 0, sizeof(s));
  s.next_checkpoint = HAL_GetTick() + INA226_ENERGY_CHECKPOINT_MS;
}

void INA226_Energy_Reset(void)
{
  APP_LOG_Printf("[energy] reset   prev=%.3f mAh  %.3f mWh  %.1f s\r\n",
                 (double)INA226_Energy_GetMah() / 1000.0,
                 (double)INA226_Energy_GetMwh() / 1000.0,
                 (double)s.total_ms / 1000.0);
  s.uas = 0;
  s.uws = 0;
  s.total_ms = 0;
  s.have_sample = 0U;
  s.last_tick = 0U;
  s.next_checkpoint = HAL_GetTick() + INA226_ENERGY_CHECKPOINT_MS;
}

/**
  * @brief  Integrate a new instantaneous sample.  Called from ina226.c
  *         (ina_sample()) right after bus_mv/cur_ua/pwr_mw are refreshed.
  *         dt comes from HAL_GetTick(); the first call just seeds the
  *         timestamp with no integration (no known previous value).
  */
void INA226_Energy_OnSample(int32_t bus_mv, int32_t cur_ua)
{
  uint32_t now = HAL_GetTick();

  if (s.have_sample != 0U)
  {
    uint32_t dt_ms = now - s.last_tick;
    /* Guard against backwards tick jumps / very long stalls by capping dt
     * to one sample interval (250 ms) - a watchdog stall or debugger halt
     * must not inject a huge phantom contribution. */
    if (dt_ms > 500U) { dt_ms = 250U; }
    /* uAs = uA * s = uA * ms / 1000.  Microwatt-seconds likewise.
     * 64-bit math; bus_mv*cur_ua/1000 = uW exactly. */
    s.uas += ((int64_t)cur_ua * (int64_t)dt_ms);
    s.uws += ((int64_t)bus_mv * (int64_t)cur_ua / 1000LL) * (int64_t)dt_ms;
    s.total_ms += dt_ms;
  }
  s.last_tick = now;
  s.have_sample = 1U;
}

/* Return mAh accumulated, scaled in milli-units (1 = 0.001 mAh = 3.6 mAs) */
int32_t INA226_Energy_GetMah(void)
{
  /* uAs / 3_600_000 = mAh.  Scale through integer-friendly form:
   *   mAh = uAs / 3_600_000 = uAs * 1000 / 3_600_000_000
   * We expose mAh as m (1e-3) mAh * 1000, i.e. thousandths of a mAh,
   * which is "mAh * 1000" for fixed-point display.
   *   mAh*1e3 = uAs * 1000 / 3_600_000 = uAs / 3600.
   */
  return (int32_t)(s.uas / 3600LL);
}

int32_t INA226_Energy_GetMwh(void)
{
  /* mWh*1e3 = uWs / 3600. */
  return (int32_t)(s.uws / 3600LL);
}

uint32_t INA226_Energy_GetMs(void)
{
  return s.total_ms;
}

/* -------------------------------------------------------------------------
 *  SD checkpoint
 * ----------------------------------------------------------------------- */
static void energy_checkpoint(void)
{
  char line[96];
  int  n;
  uint8_t ok;

  if (EXT_SD_IsMounted() == 0U)
  {
    return;       /* no card - don't spam the log; just skip */
  }

  n = snprintf(line, sizeof(line),
               "%lu,%ld,%ld,%lu\r\n",
               (unsigned long)HAL_GetTick(),
               (long)INA226_Energy_GetMah(),
               (long)INA226_Energy_GetMwh(),
               (unsigned long)s.total_ms);
  if ((n <= 0) || ((size_t)n >= sizeof(line))) { return; }
  ok = (EXT_SD_AppendLine(INA226_ENERGY_PATH, (const uint8_t *)line, (uint16_t)n) == HAL_OK);
  if (ok)
  {
    s.checkpoint_lines++;
  }
}

/* -------------------------------------------------------------------------
 *  Poll - runs every super-loop pass from ext_i2c feature hook
 * ----------------------------------------------------------------------- */
void INA226_Energy_Poll(void)
{
  uint32_t now = HAL_GetTick();
  if ((int32_t)(now - s.next_checkpoint) >= 0)
  {
    s.next_checkpoint = now + INA226_ENERGY_CHECKPOINT_MS;
    energy_checkpoint();
  }
}

void INA226_Energy_Print(void)
{
  int32_t mah = INA226_Energy_GetMah();
  int32_t mwh = INA226_Energy_GetMwh();
  uint32_t ms = s.total_ms;
  APP_LOG_Printf("[energy] %.3f mAh   %.3f mWh   %.1f s   checkpoints=%lu\r\n",
                 (double)mah / 1000.0,
                 (double)mwh / 1000.0,
                 (double)ms / 1000.0,
                 (unsigned long)s.checkpoint_lines);
}

void INA226_Energy_PrintStatus(void)
{
  int32_t mah = INA226_Energy_GetMah();
  int32_t mwh = INA226_Energy_GetMwh();
  uint32_t ms = s.total_ms;
  APP_LOG_Printf("ina226-energy: %.3f mAh  %.3f mWh  (%.1f s)  checkpoints=%lu\r\n",
                 (double)mah / 1000.0,
                 (double)mwh / 1000.0,
                 (double)ms / 1000.0,
                 (unsigned long)s.checkpoint_lines);
}

void INA226_Energy_Cli(int argc, char *argv[])
{
  if ((argc >= 3) && (strcmp(argv[1], "energy") == 0) &&
      (strcmp(argv[2], "reset") == 0))
  {
    INA226_Energy_Reset();
    return;
  }
  if ((argc >= 2) && (strcmp(argv[1], "energy") == 0))
  {
    INA226_Energy_Print();
    APP_LOG_Printf("           sd card: %s   path '%s'\r\n",
                   EXT_SD_IsMounted() ? "mounted" : "not mounted",
                   INA226_ENERGY_PATH);
    return;
  }
}
