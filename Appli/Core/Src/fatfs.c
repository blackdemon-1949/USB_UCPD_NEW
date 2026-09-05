/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    fatfs.c
  * @brief   This file provides code for the FatFS integration on SDMMC1.
  *
  * Uses the real ChaN FatFS + ST's sd_diskio.c glue (polling mode for bring-up;
  * DMA/IT can be enabled later by swapping the driver).  Registers the SD disk
  * driver at boot; f_mount() itself is deferred to ext_sd.c so that a card
  * yanked between MX_FATFS_Init() and the first open cannot leave a stale
  * mounted handle.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "fatfs.h"

/* USER CODE BEGIN Includes */
#include "app_log.h"
/* USER CODE END Includes */

FATFS SDFatFS;     /* FatFS filesystem object for "0:/" (SD card) */
FIL   SDFile;      /* Cached FIL handle kept by CubeMX - we never hold it open;
                      see EXT_SD_AppendLine() (open/write/close per call). */
char  SDPath[4];   /* SD logical drive path ("0:/") */

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* FatFS initialization */
void MX_FATFS_Init(void)
{
  /* USER CODE BEGIN FATFS_Init 0 */

  /* USER CODE END FATFS_Init 0 */

  /* USER CODE BEGIN FATFS_Init 1 */
  /* USER CODE END FATFS_Init 1 */

  /* USER CODE BEGIN FATFS_Init 2 */
  /* Register the SD disk I/O driver with FatFS.  FATFS_LinkDriver fills
   * SDPath with the logical-drive string ("0:") and returns the assigned
   * drive number.  On failure we fall back to a hard-coded "0:/" path so
   * that ext_sd.c's hot-plug mount can retry later (non-fatal). */
  SDPath[0] = '\0';
  if (FATFS_LinkDriver(&SD_Driver, SDPath) == 0U)
  {
    APP_LOG_Printf("fatfs: SD disk driver linked as '%s'\r\n", SDPath);
  }
  else
  {
    SDPath[0] = '0';
    SDPath[1] = ':';
    SDPath[2] = '\0';
    APP_LOG_Write("fatfs: SD disk driver link failed (will retry on hot-plug)\r\n");
  }
  /* USER CODE END FATFS_Init 2 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
