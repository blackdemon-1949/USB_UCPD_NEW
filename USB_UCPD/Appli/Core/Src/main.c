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

/* EPR-freeze telemetry: latch + raw trace-UART writer live in the USBPD
 * device layer (usbpd_hw_if_it.c).  The main loop below emits a 1 Hz >L
 * alive tick while the latch is armed, so a freeze that leaves the loop
 * running is distinguishable from a total stop on the trace terminal. */
extern volatile uint8_t g_usbpd_tele;
extern void USBPD_HW_IF_Tele(const char *s);
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
      for (volatile uint32_t d = 0; d < 24000000UL; d++) { __NOP(); }
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
      for (volatile uint32_t d = 0; d < 24000000UL; d++) { __NOP(); }
    }
    for (volatile uint32_t d = 0; d < 120000000UL; d++) { __NOP(); }
  }
}

/**
  * @brief  Report a CPU fault: latch the fault registers and blink @p code.
  *
  * Called from the fault handlers before their while(1).  Without this a
  * fault is a silent lock-up that looks exactly like dead hardware - the
  * bench symptom of "bricked until I press reset".  The globals are kept
  * volatile and non-static so a debugger can read them after the fact.
  *   2 = HardFault  3 = MemManage  4 = BusFault  5 = UsageFault
  */
volatile uint32_t APP_FaultCFSR;
volatile uint32_t APP_FaultHFSR;
volatile uint32_t APP_FaultMMAR;
volatile uint32_t APP_FaultBFAR;
volatile uint32_t APP_FaultCode;

/* ------------------------------------------------------------------ */
/* Fault record in backup SRAM (4 KiB bank at 0x38800000).             */
/*                                                                     */
/* Address plan of the bank - two owners, they must not overlap:       */
/*   [0x000, 0x100)  APP_STORE_Cfg_t  (app_store.c, APP_STORE_BKPSRAM) */
/*   [0x200, 0x240)  this fault record (below)                         */
/*                                                                     */
/* The record used to sit at offset 0, on top of the store config: a   */
/* crash destroyed the saved profile, and a 'store save' erased a      */
/* crash record.  Neither owner may write the other's area.            */
/* ------------------------------------------------------------------ */
#define APP_FAULT_BKPSRAM_BASE   0x38800000uL
#define APP_FAULT_RECORD_OFFSET  0x200u
#define APP_FAULT_RECORD_MAGIC   0xFA017EDDuL

/* ------------------------------------------------------------------ */
/* Fault vector trampolines (naked)                                     */
/*                                                                      */
/* The vector table in the startup file calls these symbols directly    */
/* from exception entry.  They MUST run before any C prologue so that:  */
/*   - SP still points at the 8-word hardware exception frame that the  */
/*     CPU pushed (R0,R1,R2,R3,R12,LR,PC,xPSR): PC is frame[6], LR is   */
/*     frame[5];                                                        */
/*   - LR still holds EXC_RETURN (bit 2 selects MSP vs PSP), which a C  */
/*     compiler-generated `bl` would have clobbered.                    */
/* Each trampoline tail-branches so no return address is pushed and no  */
/* register is clobbered.  The stock C bodies of the same names live in */
/* stm32h7rsxx_it.c but are #define-renamed away, so a CubeMX           */
/* regeneration cannot reintroduce a duplicate symbol. */
#define APP_FAULT_TRAMPOLINE(name, codev)                                 \
  void name(void) __attribute__((naked, used));                           \
  void name(void)                                                         \
  {                                                                       \
    __asm volatile(                                                       \
        "  tst   lr, #4            \n"                                   \
        "  ite   eq                \n"                                   \
        "  mrseq r0, msp           \n"                                   \
        "  mrsne r0, psp           \n"                                   \
        "  ldr   r1, =%[cd]        \n"                                   \
        "  b     APP_FaultReportCore\n"                                  \
        : : [cd] "i" (codev));                                           \
  }

/* The `b` is relocatable by the linker.  The PC so captured is the
 * faulting instruction in the Appli XIP image (0x90000000) or in the
 * Boot image. */
APP_FAULT_TRAMPOLINE(HardFault_Handler, 2u)
APP_FAULT_TRAMPOLINE(MemManage_Handler, 3u)
APP_FAULT_TRAMPOLINE(BusFault_Handler, 4u)
APP_FAULT_TRAMPOLINE(UsageFault_Handler, 5u)

/* Common tail for the trampolines above: r0 = exception frame base,     */
/* r1 = fault code.                                                      */
void APP_FaultReportCore(uint32_t *frame, uint32_t code);


/* Bounded, interrupt-free, register-level USART1 output for the crash
 * record.  This is the PD trace UART (PB6/PB7 @ 921600, TRACER_EMB/DMA
 * owned).  After a fault the DMA/IRQ machinery may be dead, so this talks
 * to the peripheral registers directly and never waits forever: if the
 * UART clock is gone the LED blink must still happen.  The dump survives a
 * full power cycle that would clear backup SRAM. */
#define APP_FAULT_UART_SPIN 200000uL

static void FaultUartByte(USART_TypeDef *u, uint8_t c)
{
  uint32_t guard = APP_FAULT_UART_SPIN;

  while (((u->ISR & USART_ISR_TXE) == 0u) && (guard != 0u))
  {
    guard--;
  }
  if (guard != 0u)
  {
    u->TDR = c;
  }
}

static void FaultUartPuts(USART_TypeDef *u, const char *s)
{
  while (*s != '\0')
  {
    FaultUartByte(u, (uint8_t)*s);
    s++;
  }
}

static void FaultUartHex32(USART_TypeDef *u, uint32_t v)
{
  static const char hx[16] = { '0','1','2','3','4','5','6','7',
                               '8','9','A','B','C','D','E','F' };
  char tmp[10];
  uint32_t i;

  tmp[0] = '0';
  tmp[1] = 'x';
  for (i = 0u; i < 8u; i++)
  {
    tmp[2u + i] = hx[(v >> (28u - (4u * i))) & 0xFu];
  }
  for (i = 0u; i < 10u; i++)
  {
    FaultUartByte(u, (uint8_t)tmp[i]);
  }
}

static void FaultUartFlush(USART_TypeDef *u)
{
  uint32_t guard = APP_FAULT_UART_SPIN;

  while (((u->ISR & USART_ISR_TC) == 0u) && (guard != 0u))
  {
    guard--;
  }
}

void APP_FaultReportCore(uint32_t *frame, uint32_t code)
{
  volatile uint32_t *rec =
    (volatile uint32_t *)(APP_FAULT_BKPSRAM_BASE + APP_FAULT_RECORD_OFFSET);
  uint32_t pc = 0u;
  uint32_t lr = 0u;
  uint32_t xpsr = 0u;

  APP_FaultCode = code;
  APP_FaultCFSR = SCB->CFSR;
  APP_FaultHFSR = SCB->HFSR;
  APP_FaultMMAR = SCB->MMFAR;
  APP_FaultBFAR = SCB->BFAR;

  if (frame != NULL)
  {
    /* Hardware exception frame: R0,R1,R2,R3,R12,LR,PC,xPSR (8 words). */
    pc   = frame[6];
    lr   = frame[5];
    xpsr = frame[7];
  }

  /* Survive the reset.  A fault that only blinks an LED tells us almost
   * nothing; BKPSRAM is retained across a warm reset, so stash the fault
   * registers and the faulting PC there and let the next boot print them.
   * That turns "it bricks" into an exact fault type and faulting address.
   * The bank is mapped non-cacheable (MPU region 5), so these stores reach
   * the RAM itself and are not lost when the reset drops the D-cache. */
  rec[0] = APP_FAULT_RECORD_MAGIC;
  rec[1] = code;
  rec[2] = APP_FaultCFSR;
  rec[3] = APP_FaultHFSR;
  rec[4] = APP_FaultMMAR;
  rec[5] = APP_FaultBFAR;
  rec[6] = pc;
  rec[7] = lr;
  rec[8] = xpsr;
  __DSB();
  __ISB();

  /* Live one-shot record on the trace UART (survives power-off). */
  __disable_irq();
  FaultUartPuts(USART1, "\r\n***FAULT code=");
  FaultUartHex32(USART1, code);
  FaultUartPuts(USART1, " CFSR=");
  FaultUartHex32(USART1, APP_FaultCFSR);
  FaultUartPuts(USART1, " HFSR=");
  FaultUartHex32(USART1, APP_FaultHFSR);
  if (pc != 0u)
  {
    FaultUartPuts(USART1, " PC=");
    FaultUartHex32(USART1, pc);
    FaultUartPuts(USART1, " LR=");
    FaultUartHex32(USART1, lr);
    FaultUartPuts(USART1, " xPSR=");
    FaultUartHex32(USART1, xpsr);
  }
  FaultUartPuts(USART1, "\r\n");
  FaultUartFlush(USART1);

  Appli_Fail((uint8_t)code);
}

/* Legacy wrapper: no exception frame available (not called from a fault
 * handler entry), so PC/LR are recorded as 0. */
void APP_FaultReport(uint32_t code)
{
  APP_FaultReportCore(NULL, code);
}

/**
  * @brief  Print and clear a fault record left in BKPSRAM by a previous run.
  */
void APP_FaultReportBoot(void)
{
  volatile uint32_t *rec =
    (volatile uint32_t *)(APP_FAULT_BKPSRAM_BASE + APP_FAULT_RECORD_OFFSET);
  const char *name;
  uint32_t pc;

  if (rec[0] != APP_FAULT_RECORD_MAGIC)
  {
    return;
  }
  name = (rec[1] == 2u) ? "HardFault" :
         (rec[1] == 3u) ? "MemManage" :
         (rec[1] == 4u) ? "BusFault"  :
         (rec[1] == 5u) ? "UsageFault" :
         (rec[1] == 7u) ? "Error_Handler (init fatal)" :
         (rec[1] == 8u) ? "DPM / init fatal" : "fatal code";
  pc = rec[6];

  APP_LOG_Printf("\r\n*** PREVIOUS RUN FAULTED: %s (code %lu)\r\n",
                 name, (unsigned long)rec[1]);
  if (rec[1] >= 6u)
  {
    /* Non-vector fatal record: no PC/exception frame to report. */
    APP_LOG_Printf("    no PC/exception frame (non-vector fatal path)\r\n");
    rec[0] = 0u;
    return;
  }
  if (pc != 0u)
  {
    APP_LOG_Printf("    PC   = 0x%08lX   LR = 0x%08lX   xPSR = 0x%08lX\r\n",
                   (unsigned long)pc, (unsigned long)rec[7],
                   (unsigned long)rec[8]);
    /* The Appli image executes from XIP NOR at 0x90000000, so the PC is a
     * link-time virtual address in Appli_*.elf.  Give the exact decode
     * command instead of leaving the user to guess the tool. */
    if ((pc >= 0x90000000uL) && (pc < 0x90080000uL))
    {
      APP_LOG_Printf("    decode: arm-none-eabi-addr2line -e "
                     "Appli/Release/Appli_Release.elf -f -C 0x%08lX\r\n",
                     (unsigned long)pc);
    }
    else if ((pc >= 0x08000000uL) && (pc < 0x08010000uL))
    {
      APP_LOG_Printf("    PC is in the internal-flash Boot image\r\n");
    }
    else
    {
      APP_LOG_Printf("    PC is outside the Appli XIP window "
                     "(0x90000000..0x90080000)\r\n");
    }
  }
  else
  {
    APP_LOG_Printf("    PC/LR not captured (record from a non-handler "
                   "APP_FaultReport call?)\r\n");
  }
  APP_LOG_Printf("    CFSR=0x%08lX HFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX\r\n",
                 (unsigned long)rec[2], (unsigned long)rec[3],
                 (unsigned long)rec[4], (unsigned long)rec[5]);
  if ((rec[2] & 0x80u) != 0u)
  {
    APP_LOG_Printf("    (MMFAR valid: 0x%08lX)\r\n", (unsigned long)rec[4]);
  }
  if ((rec[2] & 0x8000u) != 0u)
  {
    APP_LOG_Printf("    (BFAR valid: 0x%08lX)\r\n", (unsigned long)rec[5]);
  }
  if ((rec[3] & 0x40000000uL) != 0uL)
  {
    APP_LOG_Printf("    (HFSR FORCED: escalated from a configurable fault "
                   "- see CFSR bits)\r\n");
  }
  rec[0] = 0u;
}

/** Public wrapper so middleware (usbpd.c) can report a fatal init error
 *  with a visible LED code instead of hanging silently in while(1). */
void Appli_Fatal(uint8_t code)
{
  /* Non-vector fatal (Error_Handler=7, DPM/init fatal=8, ...): leave a
   * BKPSRAM record too, so the next boot's CDC banner reports which fatal
   * path ran even if the live trace/console died with it. */
  volatile uint32_t *rec =
    (volatile uint32_t *)(APP_FAULT_BKPSRAM_BASE + APP_FAULT_RECORD_OFFSET);
  rec[0] = APP_FAULT_RECORD_MAGIC;
  rec[1] = code;
  rec[2] = 0u; rec[3] = 0u; rec[4] = 0u; rec[5] = 0u;
  rec[6] = 0u; rec[7] = 0u; rec[8] = 0u;
  __DSB();
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
  /* DTS: always started.  'temp' is a requested bench feature; only the
   * periodic statistics accumulation belongs to APP_ENG_ANALYTICS. */
  APP_TEMP_Init();
#if APP_ENG_STORE
  APP_STORE_Init();
#endif
  APP_LED_Set(APP_LED_HEARTBEAT);

  /* Backup SRAM: keeps the fault record across a warm reset so a crash can
   * be diagnosed on the next boot instead of only blinking an LED.
   * The clock alone is not enough - without the backup regulator the
   * contents are not retained, which is why the first attempt at this
   * printed nothing after the EPR lock-up. */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_BKPRAM_CLK_ENABLE();
  (void)HAL_PWREx_EnableBkUpReg();

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

    /* EPR-freeze telemetry alive tick: one >L per second while armed,
     * auto-disarm after 20 s so a hung EPR attempt cannot spam the trace
     * UART for ever. */
    if (g_usbpd_tele != 0u)
    {
      static uint32_t s_tele_last;
      static uint32_t s_tele_age;
      uint32_t t = HAL_GetTick();

      if ((uint32_t)(t - s_tele_last) >= 1000u)
      {
        s_tele_last = t;
        s_tele_age++;
        USBPD_HW_IF_Tele("\r\n>L\r\n");
        if (s_tele_age >= 20u)
        {
          g_usbpd_tele = 0u;
        }
      }
    }
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

  /** Region 5: backup SRAM 0x38800000 (4 KiB) - NON-CACHEABLE.
   *
   * Region 0 (4 GB no-access background) disables its subregions 0, 1, 2
   * and 7 (0x87), but a disabled subregion is *not* protected - it falls
   * through to the Cortex-M7 default memory map.  Subregion 1 is
   * 0x20000000..0x3FFFFFFF, so 0x38800000 was left on the default map:
   * Normal, write-back, cacheable.  Every BKPSRAM access - the fault
   * record in APP_FaultReportCore() and the 'store' configuration in
   * app_store.c - was then served from the D-cache and never reached the
   * RAM itself.  A warm reset discards dirty cache lines, which is exactly
   * why "PREVIOUS RUN FAULTED" never printed and saved profiles did not
   * survive a reset.
   *
   * This region makes the whole 4 KiB bank non-cacheable, so writes become
   * visible to the backup domain immediately and survive NRST (and VDD
   * loss while the backup regulator holds the bank).  It overlaps nothing:
   * region 0 does not cover subregion 1, and no other region matches this
   * address.  The bank is 4 KiB, so a 4 KiB region at 0x38800000 is
   * correctly aligned.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER5;
  MPU_InitStruct.BaseAddress = 0x38800000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
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
