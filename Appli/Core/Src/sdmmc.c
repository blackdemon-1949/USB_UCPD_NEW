/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.c
  * @brief   This file provides code for the configuration
  *          of the SDMMC instances.
  *
  * SDMMC1 pins (WeActStudio.STM32H7R3Zx_CoreBoard):
  *   CLK PC12  CMD PD2  D0 PC8  D1 PC9  D2 PC10  D3 PC11
  *   CD  PA8   (active-low card-detect input, pulled up externally by the socket)
  *
  * Hand-written placeholder matching the standard CubeMX-generated sdmmc.c
  * layout.  Regenerate from the .ioc once SDMMC1 + FatFS are selected in
  * CubeMX and the real HAL_SD driver is present; USER CODE sections survive.
  *
  * NOTE ON DMA: STM32H7R3/H7S3 uses GPDMA1 (not classic DMA1/DMA2).  The
  * project already calls MX_GPDMA1_Init() in main.c.  CubeMX should assign
  * a free GPDMA1 channel for SDMMC1 TX/RX when it regenerates; do NOT copy
  * DMA-stream based example code from older H743/H750 tutorials.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "sdmmc.h"

/* USER CODE BEGIN 0 */
/* SDMMC_CLOCK_* / SDMMC_BUS_WIDE_* / SDMMC_HARDWARE_FLOW_CONTROL_* are now
 * provided by the real stm32h7rsxx_ll_sdmmc.h pulled in via stm32h7rsxx_hal_sd.h.
 * The fallback #defines from the stub-era have been removed to avoid any
 * drift between our copy and the vendor's layout. */
/* USER CODE END 0 */

SD_HandleTypeDef hsd1;

/* SDMMC1 init function */
void MX_SDMMC1_SD_Init(void)
{

  /* USER CODE BEGIN SDMMC1_Init 0 */

  /* USER CODE END SDMMC1_Init 0 */

  /* USER CODE BEGIN SDMMC1_Init 1 */

  /* USER CODE END SDMMC1_Init 1 */

  hsd1.Instance                 = SDMMC1;
  hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv            = 0;
  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    /* A missing / unpowered card on initial bring-up is not a fatal error
     * (ext_sd.c will mount later on hot-plug).  We log and continue rather
     * than stalling in Error_Handler(). */
    /* USER CODE BEGIN SDMMC1_Init_Error */
    /* Initialisation without a card is expected at boot. */
    /* USER CODE END SDMMC1_Init_Error */
  }
  /* USER CODE BEGIN SDMMC1_Init 2 */

  /* USER CODE END SDMMC1_Init 2 */
}

void HAL_SD_MspInit(SD_HandleTypeDef* sdHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if (sdHandle->Instance == SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspInit 0 */

  /* USER CODE END SDMMC1_MspInit 0 */

  /** Initializes the peripherals clock
  */
    /* STM32H7R3/H7S3 groups SDMMC1+SDMMC2 under one RCC mux (SDMMC12).
     * The default PLL2S kernel clock (~200 MHz from the CubeMX clock tree)
     * is fine for an initial bring-up at the lowest divider (HS->init
     * clock).  CubeMX regenerates this block from the .ioc. */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_SDMMC12;
    PeriphClkInit.Sdmmc12ClockSelection = RCC_SDMMC12CLKSOURCE_PLL2S;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* SDMMC1 clock enable */
    __HAL_RCC_SDMMC1_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**SDMMC1 GPIO Configuration
    PC12 ------> SDMMC1_CK
    PC8  ------> SDMMC1_D0
    PC9  ------> SDMMC1_D1
    PC10 ------> SDMMC1_D2
    PC11 ------> SDMMC1_D3
    PD2  ------> SDMMC1_CMD
    PA8  ------> SDMMC1_CD  (card detect; configured as GPIO input, not AF)
    */
    GPIO_InitStruct.Pin = GPIO_PIN_12|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* Card detect (PA8): floating/pushed high by external socket pull-up,
     * pulled LOW by the shell-switch when a card is seated.  Input with no
     * internal pull so we don't fight the socket.  ext_sd.c reads this pin. */
    GPIO_InitStruct.Pin = SD_DETECT_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SD_DETECT_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN SDMMC1_MspInit 1 */
    /* DMA channel assignment is handled by CubeMX when GPDMA is configured
     * for SDMMC.  Left in USER CODE so a later regeneration does not lose
     * any channel/IRQ priority choices. */
  /* USER CODE END SDMMC1_MspInit 1 */
  }
}

void HAL_SD_MspDeInit(SD_HandleTypeDef* sdHandle)
{
  if (sdHandle->Instance == SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspDeInit 0 */

  /* USER CODE END SDMMC1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SDMMC1_CLK_DISABLE();

    /**SDMMC1 GPIO Configuration
    PC12     ------> SDMMC1_CK
    PC8      ------> SDMMC1_D0
    PC9      ------> SDMMC1_D1
    PC10     ------> SDMMC1_D2
    PC11     ------> SDMMC1_D3
    PD2      ------> SDMMC1_CMD
    PA8      ------> SDMMC1_CD
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_12|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11);
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);
    HAL_GPIO_DeInit(SD_DETECT_GPIO_Port, SD_DETECT_Pin);

  /* USER CODE BEGIN SDMMC1_MspDeInit 1 */

  /* USER CODE END SDMMC1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
/* USER CODE END 1 */
