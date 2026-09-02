/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx_hal.h"

#include "stm32h7rsxx_ll_ucpd.h"
#include "stm32h7rsxx_ll_bus.h"
#include "stm32h7rsxx_ll_cortex.h"
#include "stm32h7rsxx_ll_rcc.h"
#include "stm32h7rsxx_ll_system.h"
#include "stm32h7rsxx_ll_utils.h"
#include "stm32h7rsxx_ll_pwr.h"
#include "stm32h7rsxx_ll_gpio.h"
#include "stm32h7rsxx_ll_dma.h"

#include "stm32h7rsxx_ll_exti.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
/* Latch fault registers and blink a fault code (2=Hard 3=MemManage 4=Bus 5=Usage). */
void APP_FaultReport(uint32_t code);

/* USER CODE BEGIN EFP */
/* Visible fatal error: blink PB2 `code` times, pause, repeat (never returns).
 * Codes 1-5 boot, 6 USB, 7 generic, 8 USBPD init.  See README. */
void Appli_Fatal(uint8_t code);

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define User_Button_Pin GPIO_PIN_13
#define User_Button_GPIO_Port GPIOC
#define INA226_SCL_Pin GPIO_PIN_10
#define INA226_SCL_GPIO_Port GPIOB
#define INA226_SDA_Pin GPIO_PIN_11
#define INA226_SDA_GPIO_Port GPIOB
#define Built_IN_LED_Pin GPIO_PIN_2
#define Built_IN_LED_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
#define LED_Pin                 GPIO_PIN_2
#define LED_GPIO_Port           GPIOB
#define KEY_Pin                 GPIO_PIN_13
#define KEY_GPIO_Port           GPIOC
#define UCPD_CC1_Pin            GPIO_PIN_0
#define UCPD_CC1_GPIO_Port      GPIOM
#define UCPD_CC2_Pin            GPIO_PIN_1
#define UCPD_CC2_GPIO_Port      GPIOM
#define APP_XIP_BASE            (0x90000000UL)
#define APP_XIP_SIZE            (0x00800000UL)
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
