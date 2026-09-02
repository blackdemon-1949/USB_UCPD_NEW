/**
 * @file    app_temp.h
 * @brief   DTS temperature engine.
 *
 * CubeMX initialises the digital temperature sensor in MX_DTS_Init() with the
 * LSE reference clock.  This engine owns the sampling policy: it is polled from
 * the main loop only, never from an ISR, because HAL_DTS_GetTemperature() runs
 * a blocking conversion.  The poll is rate limited so the main loop stays
 * responsive to the PD stack.
 *
 * Statistics are kept in RAM and correlated with PD/power activity by
 * stamping each sample with the transaction counter and the VBUS voltage, so
 * a thermal excursion can be tied back to a contract change.
 */
#ifndef APP_TEMP_H
#define APP_TEMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Minimum interval between two conversions, in main-loop passes. */
#define APP_TEMP_POLL_US 100000u   /* 10 Hz */

/** Thermal alert thresholds, in milli-degrees Celsius. */
#define APP_TEMP_WARN_MC 75000
#define APP_TEMP_CRIT_MC 95000

typedef struct
{
  int32_t  mc;              /* latest reading, milli-degrees C            */
  int32_t  min_mc;
  int32_t  max_mc;
  int64_t  sum_mc;          /* raw sum, divided on read to avoid drift    */
  uint32_t n;
  uint32_t n_alert;
  int32_t  at_contract_mv;  /* VBUS when the alert fired                  */
  uint32_t at_contracts;    /* contract count when the alert fired        */
  uint8_t  started;
  uint8_t  error;
} APP_TEMP_Stat_t;

void APP_TEMP_Init(void);

/**
 * Main-loop poll.  Performs at most one conversion per APP_TEMP_POLL_US.
 * Non-blocking apart from the conversion itself (~microseconds).
 */
void APP_TEMP_Poll(void);

/** Latest reading in milli-degrees C, or INT32_MIN when not yet sampled. */
int32_t APP_TEMP_Get(void);

void APP_TEMP_GetStats(APP_TEMP_Stat_t *out);
void APP_TEMP_Clear(void);

int APP_TEMP_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_TEMP_H */
