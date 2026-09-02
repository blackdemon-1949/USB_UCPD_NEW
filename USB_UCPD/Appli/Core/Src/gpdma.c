/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpdma.c
  * @brief   This file provides code for the configuration
  *          of the GPDMA instances.
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
#include "gpdma.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* GPDMA1 init function */
void MX_GPDMA1_Init(void)
{

  /* USER CODE BEGIN GPDMA1_Init 0 */

  /* USER CODE END GPDMA1_Init 0 */

  /* Peripheral clock enable */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* GPDMA1 interrupt Init
   *
   * Channel map for this configuration (.ioc):
   *   CH0 = UCPD1_RX, CH1 = UCPD1_TX  -> serviced by the UCPD1 ISR, the channel
   *                                      IRQs stay masked; keep them at the
   *                                      UCPD priority (5), below USB OTG_HS (4)
   *   CH2 = USART1_RX                 -> spare, tracer only transmits
   *   CH3 = USART1_TX                 -> USBPD trace TX (TRACER_EMB)
   */
    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);

  /* USER CODE BEGIN GPDMA1_Init 1 */
  /* USBPD trace TX (TRACER_EMB over USART1 DMA) uses GPDMA1 Channel 3 in this
     configuration - see TRACER_EMB_TX_DMA_CHANNEL in tracer_emb_conf.h.
     Tracing must never delay USB enumeration (4) or PD message handling (5),
     so the trace channels sit one level below both. */
    HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);
  /* USER CODE END GPDMA1_Init 1 */
  /* USER CODE BEGIN GPDMA1_Init 2 */

  /* USER CODE END GPDMA1_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

