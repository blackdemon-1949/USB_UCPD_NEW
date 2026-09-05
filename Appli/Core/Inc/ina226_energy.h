/**
  ******************************************************************************
  * @file    ina226_energy.h
  * @brief   INA226 energy accumulator (mAh / mWh) with SD-card checkpointing.
  *
  * Integrates the instantaneous bus-voltage / current readings that
  * ina226.c already samples every 250 ms into monotonic mAh / mWh totals,
  * weighted by HAL_GetTick() deltas.  Mirrors the design used in
  * WeAct's PowerMonitorMiniV1 (CMD_MAH_MWH).
  *
  * Checkpoints every INA226_ENERGY_CHECKPOINT_MS to the SD card via
  * EXT_SD_AppendLine() (never NOR flash - see FLASH_ENDURANCE.md).  The
  * SD module is required to be mounted for checkpoints; if the card is
  * absent the accumulator keeps running in RAM and checkpoints are
  * skipped (silently - one lost log line is not worth blocking the loop).
  ******************************************************************************
  */
#ifndef INA226_ENERGY_H
#define INA226_ENERGY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void    INA226_Energy_Init(void);
void    INA226_Energy_Reset(void);
void    INA226_Energy_OnSample(int32_t bus_mv, int32_t cur_ua);  /* called by ina226.c after each sample */
int32_t INA226_Energy_GetMah(void);    /* signed (negative = reverse current) */
int32_t INA226_Energy_GetMwh(void);
uint32_t INA226_Energy_GetMs(void);    /* total integrated session ms */

void    INA226_Energy_Poll(void);      /* super-loop poll: handles checkpointing */
void    INA226_Energy_Print(void);
void    INA226_Energy_PrintStatus(void);
void    INA226_Energy_Cli(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* INA226_ENERGY_H */
