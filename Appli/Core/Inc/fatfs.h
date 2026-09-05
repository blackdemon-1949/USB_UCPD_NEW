/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    fatfs.h
  * @brief   Header for fatfs.c (CubeMX FatFS middleware integration for SDMMC1).
  *
  * Uses the real ChaN FatFS middleware under Middlewares/Third_Party/FatFs/src
  * (fetched from ST's stm32-mw-fatfs repo, master branch, matching the HAL
  * version in this project).  USER CODE sections are preserved so a future
  * CubeMX regeneration can re-take ownership of this header without dropping
  * our glue.
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __FATFS_H__
#define __FATFS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "sdmmc.h"

/* USER CODE BEGIN Includes */
/* Pull in the real FatFS API (ff.h) instead of the previous placeholder
 * forward-declarations.  The Middlewares/Third_Party/FatFs/src directory and
 * its drivers/sd subdirectory are on the include path (-I) in both the
 * CubeIDE project and tools/check_syntax.sh. */
#include "ff.h"
#include "ff_gen_drv.h"
#include "sd_diskio.h"
/* USER CODE END Includes */

extern FATFS SDFatFS;     /* FatFS filesystem object for "0:/" (SD card) */
extern FIL   SDFile;      /* Cached FIL handle (we never hold it open;
                             see EXT_SD_AppendLine() which opens/writes/closes
                             per call). */
extern char  SDPath[4];   /* SD logical drive path ("0:/") */

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_FATFS_Init(void);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __FATFS_H__ */
