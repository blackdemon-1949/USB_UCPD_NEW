/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbpd.c
  * @author  MCD Application Team
  * @brief   This file contains the device define.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbpd.h"

/* USER CODE BEGIN 0 */
#include "app_log.h"
#include "main.h"
#if defined(PDENGINE_PDSINK)
#include "app_pd.h"       /* registers the pdsink event callback (APP_PD_Init) */
#include "pdport_app.h"   /* pdsink C seam: init once before the main loop */
#endif
/* USER CODE END 0 */

/* USER CODE BEGIN 1 */
/* USER CODE END 1 */

/* Private variables ---------------------------------------------------------*/

/* Private functions ---------------------------------------------------------*/

/* USER CODE BEGIN 2 */
/* USER CODE END 2 */
/* USBPD init function */
void MX_USBPD_Init(void)
{

#if defined(PDENGINE_PDSINK)
  /* pdsink path: no closed USBPD core is linked, so the ST DPM init chain
   * does not exist.  pdport_init() brings up the open ST device layer
   * (PHY + CAD) and starts the pdsink graph; the 1 ms pump is called from
   * the main loop (see main.c). */
  /* APP_PD_Init() is normally reached through the closed core's
   * USBPD_DPM_InitCore() -> USBPD_DPM_UserInit() chain.  No closed core is
   * linked on this path, so run it here before the engine starts: it arms
   * the pdsink event callback (pdport_set_event_cb) that feeds cable /
   * contract / EPR state into the app. */
  APP_PD_Init();
  if (pdport_init() != 0)
  {
    APP_LOG_Write("usbpd: pdport_init failed\r\n");
    Appli_Fatal(8);
  }
  __enable_irq();
#else
  /* Global Init of USBPD HW */
  USBPD_HW_IF_GlobalHwInit();

  /* Initialize the Device Policy Manager.
     The silent while(1) hangs are gone: a failed PD bring-up now shows a
     visible PB2 blink code (8) instead of freezing the board with the LED
     frozen in a random state. */
  if (USBPD_OK != USBPD_DPM_InitCore())
  {
    APP_LOG_Write("usbpd: DPM_InitCore failed\r\n");
    Appli_Fatal(8);
  }

  /* Initialise the DPM application */
  if (USBPD_OK != USBPD_DPM_UserInit())
  {
    APP_LOG_Write("usbpd: DPM_UserInit failed\r\n");
    Appli_Fatal(8);
  }

  /* USER CODE BEGIN 3 */
  /* USER CODE END 3 */

  if (USBPD_OK != USBPD_DPM_InitOS())
  {
    APP_LOG_Write("usbpd: DPM_InitOS failed\r\n");
    Appli_Fatal(8);
  }

  /* USER CODE BEGIN EnableIRQ */
  /* Enable IRQ which has been disabled by FreeRTOS services */
  __enable_irq();
  /* USER CODE END EnableIRQ */
#endif /* PDENGINE_PDSINK */

}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

/**
  * @}
  */

/**
  * @}
  */
