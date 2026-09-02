/**
 * @file    app_pwr.h
 * @brief   Power / energy analytics over INA226 samples.
 *
 * The INA226 driver already owns the I2C bus and keeps the working
 * measurement path untouched.  This module only consumes samples: it keeps
 * instantaneous / min / max / average values, integrates energy and charge,
 * and correlates a sample with whatever PD event was in flight.
 *
 * Pure and integer-only, so it is exact and host testable.  Accumulators are
 * 64-bit; a 240 W load sampled every millisecond overflows nothing realistic.
 */
#ifndef APP_PWR_H
#define APP_PWR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct
{
  uint32_t n;              /* samples integrated                       */

  int32_t  mv_last, mv_min, mv_max;
  int32_t  ua_last, ua_min, ua_max;
  int32_t  mw_last, mw_min, mw_max;

  int64_t  mv_sum;         /* for the average                          */
  int64_t  ua_sum;
  int64_t  mw_sum;

  /* Raw integrals.  Energy and charge are derived from these rather than
   * accumulated per sample, otherwise every sample truncates and the error
   * grows with the sample count. */
  int64_t  mw_us;          /* sum of P[mW] * dt[us]                     */
  int64_t  ua_us;          /* sum of I[uA] * dt[us]                     */

  int64_t  uwh;            /* = mw_us / 3.6e6, micro-watt-hours (signed) */
  int64_t  uah;            /* = ua_us / 3.6e9, micro-amp-hours           */
  uint32_t span_us;        /* integration window                       */

  /* Operating point the PD contract asked for, for correlation */
  uint32_t contract_mv;
  uint32_t contract_ma;
  uint8_t  contract_epr;
  uint32_t n_events;       /* PD events observed during integration    */
  int32_t  worst_dev_mv;   /* largest |measured - contracted| excursion */
} APP_PWR_Stat_t;

void APP_PWR_Init(APP_PWR_Stat_t *s);

/**
 * Integrate one measurement.
 * @param dt_us  time since the previous sample, in microseconds
 * @param mv     bus voltage in millivolts
 * @param ua     shunt current in microamps, signed (negative = reverse flow)
 */
void APP_PWR_Sample(APP_PWR_Stat_t *s, uint32_t dt_us, int32_t mv, int32_t ua);

/** Record the operating point the PD contract currently asks for. */
void APP_PWR_SetContract(APP_PWR_Stat_t *s, uint32_t mv, uint32_t ma,
                         uint8_t epr);

/** Note a PD event so the power timeline can be correlated. */
void APP_PWR_NoteEvent(APP_PWR_Stat_t *s);

int32_t APP_PWR_AvgMv(const APP_PWR_Stat_t *s);
int32_t APP_PWR_AvgUa(const APP_PWR_Stat_t *s);
int32_t APP_PWR_AvgMw(const APP_PWR_Stat_t *s);

/* Formatting: bounded and always NUL terminated. */
void APP_PWR_Format(const APP_PWR_Stat_t *s, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* APP_PWR_H */
