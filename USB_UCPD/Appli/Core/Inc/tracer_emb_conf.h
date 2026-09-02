/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    tracer_emb_conf.h
  * @author  MCD Application Team, adapted for the WeAct H7R3Z8 board
  * @brief   Trace HW related defines (TRACER_EMB).
  *
  * Wiring on the WeAct STM32H7R3Z8J6 board:
  *     PB6  = USART1_TX  ->  USB-UART adapter RX   (AF7)
  *     PB7  = USART1_RX  ->  USB-UART adapter TX   (AF7)
  *     GND  = GND        ->  USB-UART adapter GND  (3.3 V logic!)
  * The trace TX DMA is GPDMA1 channel 3; in STM32CubeMonitor-UCPD add a board
  * node, select the TRACER interface and pick the adapter's COM port
  * (921600 baud, 8N1).
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

#ifndef __TRACER_EMB_CONF_H
#define __TRACER_EMB_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx_ll_bus.h"
#include "stm32h7rsxx_ll_dma.h"
#include "stm32h7rsxx_ll_gpio.h"
#include "stm32h7rsxx_ll_rcc.h"
#include "stm32h7rsxx_ll_usart.h"
/* Private typedef -----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* -----------------------------------------------------------------------------
      Definitions for TRACE feature
-------------------------------------------------------------------------------*/

#define TRACER_EMB_BAUDRATE                          921600UL

#define TRACER_EMB_DMA_MODE                          1UL
#define TRACER_EMB_IT_MODE                           0UL

#define TRACER_EMB_BUFFER_SIZE                       1024UL

/* -----------------------------------------------------------------------------
      Definitions for TRACE Hw information
-------------------------------------------------------------------------------*/

#define TRACER_EMB_IS_INSTANCE_LPUART_TYPE           0UL /* set to 0UL if USART is used */

#define TRACER_EMB_USART_INSTANCE                    USART1

#define TRACER_EMB_TX_GPIO                           GPIOB
#define TRACER_EMB_TX_PIN                            LL_GPIO_PIN_6
#define TRACER_EMB_TX_AF                             LL_GPIO_AF_7
#define TRACER_EMB_TX_GPIO_ENABLE_CLOCK()            LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOB)
#define TRACER_EMB_RX_GPIO                           GPIOB
#define TRACER_EMB_RX_PIN                            LL_GPIO_PIN_7
#define TRACER_EMB_RX_AF                             LL_GPIO_AF_7
#define TRACER_EMB_RX_GPIO_ENABLE_CLOCK()            LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOB)

#define TRACER_EMB_ENABLE_CLK_USART()                LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1)
#define TRACER_EMB_DISABLE_CLK_USART()               LL_APB2_GRP1_DisableClock(LL_APB2_GRP1_PERIPH_USART1)
#define TRACER_EMB_SET_CLK_SOURCE_USART()            /* No need for clock source selection in case of USARTUSART1 */
#define TRACER_EMB_USART_IRQ                         USART1_IRQn
#define TRACER_EMB_USART_IRQHANDLER                  USART1_IRQHandler
/* USART1 trace IRQ priority.  6 = just below USB OTG (4) and UCPD (5):
 * tracing must never delay USB enumeration or PD message handling. */
#define TRACER_EMB_TX_IRQ_PRIORITY                   6

#define TRACER_EMB_TX_AF_FUNCTION                    LL_GPIO_SetAFPin_0_7
#define TRACER_EMB_RX_AF_FUNCTION                    LL_GPIO_SetAFPin_0_7

#define TRACER_EMB_DMA_INSTANCE                      GPDMA1
#define TRACER_EMB_ENABLE_CLK_DMA()                  do {                                                       \
                                                       LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);                               \
                                                       } while(0)
#define TRACER_EMB_TX_DMA_REQUEST                    LL_GPDMA1_REQUEST_USART1_TX
#define TRACER_EMB_TX_DMA_CHANNEL                    LL_DMA_CHANNEL_3
#define TRACER_EMB_ENABLECHANNEL                     LL_DMA_ResumeChannel
#define TRACER_EMB_DISABLECHANNEL                    LL_DMA_SuspendChannel
#define TRACER_EMB_TX_DMA_IRQ                        GPDMA1_Channel3_IRQn
#define TRACER_EMB_TX_DMA_IRQHANDLER                 GPDMA1_Channel3_IRQHandler
#define TRACER_EMB_TX_DMA_ACTIVE_FLAG(_DMA_)         LL_DMA_IsActiveFlag_TC(_DMA_, TRACER_EMB_TX_DMA_CHANNEL)
#define TRACER_EMB_TX_DMA_CLEAR_FLAG(_DMA_)          LL_DMA_ClearFlag_TC(_DMA_, TRACER_EMB_TX_DMA_CHANNEL)

#define TRACER_EMB_STRUCTURE_MEMORY_LOCATION         "noncacheable_buffer"

#ifdef __cplusplus
}
#endif

#endif /* __TRACER_EMB_CONF_H */
