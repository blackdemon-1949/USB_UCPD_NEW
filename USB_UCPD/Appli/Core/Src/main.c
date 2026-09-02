/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : PD Bench application — USB CDC CLI + UCPD sink (XiP)
  *
  * Runs from external NOR at 0x90000000 after the 64 KB internal bootloader
  * memory-maps Puya PY25Q64HA. Clocks are already up (Boot SystemClock_Config).
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "crc.h"
#include "dts.h"
#include "gpdma.h"
#include "hash.h"
#include "i2c.h"
#include "rng.h"
#include "ucpd.h"
#include "usart.h"
#include "usbpd.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usb_device.h"
#include "app_log.h"
#include "app_cli.h"
#include "app_pdcap.h"
#include "app_diag.h"
#include "app_epr.h"
#include "app_temp.h"
#include "app_store.h"
#include "app_cable.h"
#include "app_vdm.h"
#include "app_cmd.h"
#include "app_engines.h"
#include "app_pd.h"
#include "app_board.h"
#include "ext_i2c.h"
#include "ext_spi.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
static void Appli_Fail(uint8_t code);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  Fatal appli error: blink PB2 `code` times, pause, repeat.
  *         6 = USB init, 7 = generic Error_Handler
  */
static void Appli_Fail(uint8_t code)
{
  __disable_irq();
  while (1)
  {
    for (uint8_t i = 0; i < code; i++)
    {
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_SET);
      for (volatile uint32_t d = 0; d < 400000UL; d++) { __NOP(); }
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
      for (volatile uint32_t d = 0; d < 400000UL; d++) { __NOP(); }
    }
    for (volatile uint32_t d = 0; d < 1600000UL; d++) { __NOP(); }
  }
}

/** Public wrapper so middleware (usbpd.c) can report a fatal init error
 *  with a visible LED code instead of hanging silently in while(1). */
void Appli_Fatal(uint8_t code)
{
  Appli_Fail(code);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Update SystemCoreClock variable according to RCC registers values. */
  SystemCoreClockUpdate();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_UCPD1_Init();
  MX_I2C2_Init();
  MX_DTS_Init();
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  MX_CRC_Init();
  MX_HASH_Init();
  MX_RNG_Init();
  /* USER CODE BEGIN 2 */
  APP_LOG_Init();
  APP_CLI_Init();
#if APP_ENG_DIAG
  APP_DIAG_Init();
#endif
#if APP_ENG_EPR
  APP_EPR_Init();
#endif
#if APP_ENG_ANALYTICS
  APP_TEMP_Init();
#endif
#if APP_ENG_STORE
  APP_STORE_Init();
#endif
  APP_LED_Set(APP_LED_HEARTBEAT);

  /* I2C2 / SPI extension footprints (see ext_i2c.c / ext_spi.c) */
  EXT_I2C_Init();
  EXT_SPI_Init();

  MX_USB_DEVICE_Init();
  /* USER CODE END 2 */

  /* USBPD initialisation ---------------------------------*/
  MX_USBPD_Init();

  /* Analyzer: record every PD event in the RAM capture ring.  Must run after
   * MX_USBPD_Init() because it re-registers the trace entry point that
   * USBPD_DPM_InitCore() installed via USBPD_TRACE_Init(). */
  /* Say which engines are compiled in, once, so the bench log identifies
   * the build.  Without this a bisect build is indistinguishable on the
   * console from a full one. */
  APP_LOG_Write("engines: " APP_ENG_SUMMARY "\r\n");
#if APP_ENG_CAPTURE
  APP_PDCAP_Init();
#else
  /* No capture engine in this profile: install the minimal PD frame counter
   * funnel so 'epr diag' / 'diag' report real TX/RX/GoodCRC counts instead
   * of permanent zeros.  Must run after MX_USBPD_Init() for the same reason
   * APP_PDCAP_Init() does - it re-registers the trace entry point. */
  APP_EPR_InstallTraceFunnel();
#endif

  /* Install the ST VDM callbacks.  USBPD_PE_Init() does not take them and
   * nothing else registers them, so without this call the firmware can never
   * receive a Discover Identity response and no live cable identity exists. */
#if APP_ENG_VDM
  APP_VDM_Init();
#endif
#if APP_ENG_CABLE_VDM
  if (APP_CBL_RegisterVdm(0) == 0)
  {
    APP_LOG_Write("warning: VDM callbacks not registered\r\n");
  }
#endif

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    USBPD_DPM_Run();

    /* USER CODE BEGIN 3 */
    /* Fixed-PDO / PPS request engine and the PD state polling.  Without this
       the CLI can still read the stack but no Request is ever sent. */
    APP_PD_Task();

    /* I2C2 / SPI extension polling (see ext_i2c.c / ext_spi.c) */
    EXT_I2C_Poll();
    EXT_SPI_Poll();

    /* Engine polling: INA226 -> power analytics, transaction state machine.
     * Non-blocking; does no I2C, no formatting, no allocation. */
#if APP_ENG_ANALYTICS
    APP_CMD_Poll();
#endif

    APP_CLI_Poll();
    APP_LOG_Flush();
    APP_LED_Task();
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /* Disables all MPU regions */
  for(uint8_t i=0; i<__MPU_REGIONCOUNT; i++)
  {
    HAL_MPU_DisableRegion(i);
  }

  /** Region 0: 4 GB background, no access (subregions 0,1,2,7 disabled = 0x87)
   *
   * CubeMX only emits the XSPI1 window, so a regeneration drops everything
   * below.  Without region 0 the default memory map is left enabled and the
   * MPU stops protecting anything outside the regions that follow.
   */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 1: 8 MB XSPI1 NOR (PY25Q64HA) - XiP code + const
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 2: AXI SRAM (cacheable) - .data/.bss/heap
   *
   * The region 4 override below carves the DMA window out of the top of it.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 3: DTCM (always non-cacheable on M7, tightly coupled)
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x20000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_64KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 4: USB/CDC + USB-PD tracer DMA buffers (32 KiB, non-cacheable AXI SRAM)
   *
   * The linker places the CDC RX/TX buffers, the log TX buffer and the
   * TRACER_EMB context in `noncacheable_buffer`, which STM32H7R3Z8JX_ROMxspi1.ld
   * maps to __RAM_BEGIN + __RAM_SIZE = 0x24068000 for
   * __RAM_NONCACHEABLEBUFFER_SIZE = 0x8000.  MPU regions are matched by
   * number, so this region overrides the cacheable AXI SRAM region above.
   * Without the override the USB DMA and the CM7 D-cache observe different
   * contents and enumeration / transfers fail.
   *
   * An ARMv7 MPU region is aligned to its own size, so the base address and
   * the size here must stay in step with the two linker symbols above.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress = 0x24068000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  Appli_Fail(7);
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  UNUSED(file);
  UNUSED(line);
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
