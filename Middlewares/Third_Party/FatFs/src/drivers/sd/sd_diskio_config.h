/**
  ******************************************************************************
  * @file    sd_diskio_config.h
  * @brief   Tells ST's sd_diskio.c glue which SD handle, init routine, and
  *          transfer timeout to use.
  *
  * sd_diskio.c takes the address of `sdmmc_handle` (e.g. &sdmmc_handle), so
  * it has to be a true lvalue / extern object — NOT a macro that expands
  * to a function call (the previous "EXT_SD_GetHandle()" version was not
  * addressable).  We declare an extern pointer here and provide the real
  * storage in ext_sd.c (pointed at hsd1 from sdmmc.c).
  ******************************************************************************
  */
#ifndef SD_DISKIO_CONFIG_H_DEFINED
#define SD_DISKIO_CONFIG_H_DEFINED

#include "stm32h7rsxx_hal.h"
#include "ext_sd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENABLE_SD_INIT     1U
#define SD_TIMEOUT         2000U

extern SD_HandleTypeDef *sdmmc_handle;

/* Re-init hook for sd_diskio.c SD_initialize() - calls ext_sd.c's EXT_SD_HardInit()
 * which does HAL_SD_Init + 4-bit bus switch + card info log. */
void EXT_SD_HardInit(void);
#define sdmmc_sd_init()    EXT_SD_HardInit()

#ifdef __cplusplus
}
#endif

#endif /* SD_DISKIO_CONFIG_H_DEFINED */
