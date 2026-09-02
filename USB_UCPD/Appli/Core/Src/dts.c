/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dts.c
  * @brief   This file provides code for the configuration
  *          of the DTS instances.
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
#include "dts.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

DTS_HandleTypeDef hdts;

/* DTS init function */
void MX_DTS_Init(void)
{

  /* USER CODE BEGIN DTS_Init 0 */

  /* USER CODE END DTS_Init 0 */

  /* USER CODE BEGIN DTS_Init 1 */

  /* USER CODE END DTS_Init 1 */
  hdts.Instance = DTS;
  hdts.Init.QuickMeasure = DTS_QUICKMEAS_DISABLE;
  /* HARDWARE FIX: reference clock must be PCLK, not LSE.
   *
   * 'temp' reported "DTS not started" on every run.  The Appli never
   * configures any oscillator (the Boot project owns the clock tree) and this
   * board has no 32.768 kHz crystal fitted, so LSE never becomes ready and
   * the DTS reference clock is dead - HAL_DTS_Start() can never succeed.
   * PCLK is always running.  With PCLK the driver requires a valid divider
   * (IS_DTS_DIVIDER_RATIO_NUMBER, 0..127); a non-zero value is needed because
   * the frequency measurement divides by it. */
  hdts.Init.RefClock = DTS_REFCLKSEL_PCLK;
  hdts.Init.TriggerInput = DTS_TRIGGER_HW_NONE;
  hdts.Init.SamplingTime = DTS_SMP_TIME_15_CYCLE;
  hdts.Init.Divider = 63;
  hdts.Init.HighThreshold = 0x0;
  hdts.Init.LowThreshold = 0x0;
  if (HAL_DTS_Init(&hdts) != HAL_OK)
  {
    /* A dead temperature sensor must not brick the bench.  Error_Handler()
     * disables interrupts and blinks for ever, which would take PD, CDC and
     * the CLI down with it.  Report and continue; 'temp' says it is off. */
    hdts.Instance = NULL;
  }
  /* USER CODE BEGIN DTS_Init 2 */

  /* USER CODE END DTS_Init 2 */

}

void HAL_DTS_MspInit(DTS_HandleTypeDef* dtsHandle)
{

  if(dtsHandle->Instance==DTS)
  {
  /* USER CODE BEGIN DTS_MspInit 0 */

  /* USER CODE END DTS_MspInit 0 */
    /* DTS clock enable */
    __HAL_RCC_DTS_CLK_ENABLE();
  /* USER CODE BEGIN DTS_MspInit 1 */

  /* USER CODE END DTS_MspInit 1 */
  }
}

void HAL_DTS_MspDeInit(DTS_HandleTypeDef* dtsHandle)
{

  if(dtsHandle->Instance==DTS)
  {
  /* USER CODE BEGIN DTS_MspDeInit 0 */

  /* USER CODE END DTS_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_DTS_CLK_DISABLE();
  /* USER CODE BEGIN DTS_MspDeInit 1 */

  /* USER CODE END DTS_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

