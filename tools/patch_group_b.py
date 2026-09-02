#!/usr/bin/env python3
"""Repair group B: the USB-PD application layer (DPM run loop, error handling,
sink PDO table incl. PPS, CC-only VBUS policy)."""
import sys, os

ROOT = sys.argv[1]

def patch(rel, pairs):
    p = os.path.join(ROOT, rel)
    raw = open(p, 'rb').read()
    crlf = b'\r\n' in raw
    t = raw.decode('utf-8')
    if crlf:
        t = t.replace('\r\n', '\n')
    for old, new in pairs:
        n = t.count(old)
        if n == 0 and t.count(new) == 1:
            print('  (already applied)', rel)
            continue
        assert n == 1, '%s: expected 1 occurrence, found %d of:\n%r' % (rel, n, old[:240])
        t = t.replace(old, new)
    if crlf:
        t = t.replace('\n', '\r\n')
    open(p, 'wb').write(t.encode('utf-8'))
    print('patched', rel)

# ------------------------------------------------------------------ usbpd.c
patch('Appli/USBPD/App/usbpd.c', [
("""  /* Initialize the Device Policy Manager */
  if (USBPD_OK != USBPD_DPM_InitCore())
  {
    while(1);
  }

  /* Initialise the DPM application */
  if (USBPD_OK != USBPD_DPM_UserInit())
  {
    while(1);
  }""",
 """  /* Initialize the Device Policy Manager.
     The silent while(1) hangs are gone: a failed PD bring-up now shows a
     visible PB2 blink code (8) instead of freezing the board with the LED
     frozen in a random state. */
  if (USBPD_OK != USBPD_DPM_InitCore())
  {
    APP_LOG_Write("usbpd: DPM_InitCore failed\\r\\n");
    Appli_Fatal(8);
  }

  /* Initialise the DPM application */
  if (USBPD_OK != USBPD_DPM_UserInit())
  {
    APP_LOG_Write("usbpd: DPM_UserInit failed\\r\\n");
    Appli_Fatal(8);
  }"""),
("""  if (USBPD_OK != USBPD_DPM_InitOS())
  {
    while(1);
  }""",
 """  if (USBPD_OK != USBPD_DPM_InitOS())
  {
    APP_LOG_Write("usbpd: DPM_InitOS failed\\r\\n");
    Appli_Fatal(8);
  }"""),
])

# ------------------------------------------------------------ usbpd_dpm_core.c
patch('Appli/USBPD/App/usbpd_dpm_core.c', [
("""#include "usbpd_dpm_user.h"

#if defined(_LOW_POWER)""",
 """#include "usbpd_dpm_user.h"
#include "main.h"

#if defined(_LOW_POWER)"""),
("""  /* Initialise the TRACE */
  USBPD_TRACE_Init();""",
 """  /* Initialise the TRACE (TRACER_EMB over USART1 -> STM32CubeMonitor-UCPD) */
  USBPD_TRACE_Init();"""),
("""#if defined(USE_STM32_UTILITY_OS)
  UTIL_SEQ_RegTask(TASK_CAD,  0, USBPD_CAD_Task);
  UTIL_SEQ_SetTask(TASK_CAD,  0);""",
 """#if defined(USE_STM32_UTILITY_OS)
  /* DPM_Run is called from the application super loop.  Registration and
   * timer creation are one-time operations; running them on every pass
   * leaks/replaces the utility-OS objects. */
  static uint8_t os_started;
  if (os_started == 0U)
  {
    os_started = 1U;
    UTIL_SEQ_RegTask(TASK_CAD,  0, USBPD_CAD_Task);
  UTIL_SEQ_SetTask(TASK_CAD,  0);"""),
("""  UTIL_SEQ_RegTask(TASK_USER, 0, USBPD_TaskUser);
  UTIL_SEQ_SetTask(TASK_USER,  0);

  do
  {
    UTIL_SEQ_Run(~0);
  } while (1u == 1u);
#else /* !USE_STM32_UTILITY_OS */
  do
  {

    if ((HAL_GetTick() - DPM_Sleep_start[USBPD_PORT_COUNT]) >= DPM_Sleep_time[USBPD_PORT_COUNT])
    {
      DPM_Sleep_time[USBPD_PORT_COUNT] = USBPD_CAD_Process();
      DPM_Sleep_start[USBPD_PORT_COUNT] = HAL_GetTick();
    }

    uint32_t port = 0;

    for (port = 0; port < USBPD_PORT_COUNT; port++)
    {
      if ((HAL_GetTick() - DPM_Sleep_start[port]) >= DPM_Sleep_time[port])
      {
        DPM_Sleep_time[port] =
          USBPD_PE_StateMachine_SNK(port);
        DPM_Sleep_start[port] = HAL_GetTick();
      }
    }

    USBPD_DPM_UserExecute(NULL);

  } while (1u == 1u);
#endif /* USE_STM32_UTILITY_OS */
}""",
 """    UTIL_SEQ_RegTask(TASK_USER, 0, USBPD_TaskUser);
    UTIL_SEQ_SetTask(TASK_USER,  0);
  }

  /* Run pending cooperative work, then return to service USB/CLI/LED. */
  UTIL_SEQ_Run(~0);
#else /* !USE_STM32_UTILITY_OS */
  /*
   * This function is called from the application's super loop.  The
   * generated ST example normally owns the super loop here and therefore
   * uses a do/while(1).  Keeping that loop in this application starves every
   * function after USBPD_DPM_Run() - APP_PD_Task() (the fixed-PDO / PPS
   * request engine), USB CDC RX processing, CDC TX flushing and the LED task
   * would never run.  Run one cooperative slice instead.
   */
  if ((HAL_GetTick() - DPM_Sleep_start[USBPD_PORT_COUNT]) >= DPM_Sleep_time[USBPD_PORT_COUNT])
  {
    DPM_Sleep_time[USBPD_PORT_COUNT] = USBPD_CAD_Process();
    DPM_Sleep_start[USBPD_PORT_COUNT] = HAL_GetTick();
  }

  for (uint32_t port = 0; port < USBPD_PORT_COUNT; port++)
  {
    if ((HAL_GetTick() - DPM_Sleep_start[port]) >= DPM_Sleep_time[port])
    {
      DPM_Sleep_time[port] = USBPD_PE_StateMachine_SNK(port);
      DPM_Sleep_start[port] = HAL_GetTick();
    }
  }

  USBPD_DPM_UserExecute(NULL);
#endif /* USE_STM32_UTILITY_OS */
}"""),
("""  USBPD_TRACE_Add(USBPD_TRACE_CADEVENT, PortNum, (uint8_t)State, NULL, 0);
 /* _TRACE */
  (void)(Cc);
  switch (State)""",
 """  USBPD_TRACE_Add(USBPD_TRACE_CADEVENT, PortNum, (uint8_t)State, NULL, 0);
  /* _TRACE */
  /* app_pd.c reads DPM_Params[port].ActiveCCIs to report the CC pin in use. */
  DPM_Params[PortNum].ActiveCCIs = Cc;
  switch (State)"""),
("""__WEAK void USBPD_DPM_ErrorHandler(void)
{
  /* This function is called to block application execution
     in case of an unexpected behavior
     another solution could be to reset application */
  while (1u == 1u) {};
}""",
 """__WEAK void USBPD_DPM_ErrorHandler(void)
{
  /* This function is called to block application execution
     in case of an unexpected behavior.  The stock implementation spins in
     while(1) which freezes the board silently (solid PB2, dead USB
     console).  Make the failure visible instead: LED blink code 8. */
  Appli_Fatal(8);
}"""),
])

# ---------------------------------------------------------- usbpd_pdo_defs.h
patch('Appli/USBPD/App/usbpd_pdo_defs.h', [
("#define PORT0_NB_SINKPDO           1U   /* Number of Sink PDOs (applicable for port 0)     */",
 "#define PORT0_NB_SINKPDO           6U   /* 5/9/12/15/20 V fixed + PPS APDO */"),
("""  /* PDO 1 */
  (
    USBPD_PDO_TYPE_FIXED                 | /* Fixed supply PDO            */

    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(5000U)         | /* Voltage in mV               */
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)     | /* Operating current in  mA            */

    /* Common definitions applicable to all PDOs, defined only in PDO 1 */
    USBPD_PDO_SNK_FIXED_FRS_NOT_SUPPORTED          | /* Fast Role Swap\t\t\t\t */
    USBPD_PDO_SNK_FIXED_DRD_SUPPORTED          | /* Dual-Role Data              */
    USBPD_PDO_SNK_FIXED_USBCOMM_SUPPORTED      | /* USB Communications          */
    USBPD_PDO_SNK_FIXED_EXT_POWER_AVAILABLE    | /* External Power              */
    USBPD_PDO_SNK_FIXED_HIGHERCAPAB_SUPPORTED   | /* Higher Capability           */
    USBPD_PDO_SNK_FIXED_DRP_NOT_SUPPORTED            /* Dual-Role Power             */
  ),

  /* PDO 2 */ (0x00000000U),

  /* PDO 3 */ (0x00000000U),

  /* PDO 4 */ (0x00000000U),

  /* PDO 5 */ (0x00000000U),

  /* PDO 6 */ (0x00000000U),

  /* PDO 7 */ (0x00000000U),
};""",
 """  /* PDO 1 - 5 V / 3 A, higher-capability so the source offers > 5 V.
   *
   * NOTE: CubeMX only models USBPD_PORT0_PDO_SNK_NB = 1 PDO in the .ioc, so a
   * regeneration collapses this table back to a single 5 V entry.  PDOs 2-6
   * below are the project's fixed-PDO / PPS request capability list (see
   * app_pd.c) and must survive; re-add them after any code generation.
   */
  (
    USBPD_PDO_TYPE_FIXED                 | /* Fixed supply PDO            */

    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(5000U)         | /* Voltage in mV               */
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)     | /* Operating current in  mA            */

    /* Common definitions applicable to all PDOs, defined only in PDO 1 */
    USBPD_PDO_SNK_FIXED_FRS_NOT_SUPPORTED          | /* Fast Role Swap\t\t\t\t */
    USBPD_PDO_SNK_FIXED_DRD_SUPPORTED          | /* Dual-Role Data              */
    USBPD_PDO_SNK_FIXED_USBCOMM_SUPPORTED      | /* USB Communications          */
    USBPD_PDO_SNK_FIXED_EXT_POWER_AVAILABLE    | /* External Power              */
    USBPD_PDO_SNK_FIXED_HIGHERCAPAB_SUPPORTED   | /* Higher Capability           */
    USBPD_PDO_SNK_FIXED_DRP_NOT_SUPPORTED            /* Dual-Role Power             */
  ),

  /* PDO 2 - 9 V / 3 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(9000U)         |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)
  ),

  /* PDO 3 - 12 V / 3 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(12000U)        |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)
  ),

  /* PDO 4 - 15 V / 3 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(15000U)        |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)
  ),

  /* PDO 5 - 20 V / 5 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(20000U)        |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(5000U)
  ),

  /* PDO 6 - PPS APDO 3.3-21 V / 3 A */
  (
    USBPD_PDO_TYPE_APDO                            |
    USBPD_PDO_SNK_APDO_PPS                         |
    USBPD_PDO_SNK_APDO_SET_MIN_VOLTAGE(3300U)      |
    USBPD_PDO_SNK_APDO_SET_MAX_VOLTAGE(21000U)     |
    USBPD_PDO_SNK_APDO_SET_MAX_CURRENT(3000U)
  ),

  /* PDO 7 */ (0x00000000U),
};"""),
])

# ----------------------------------------------------------- usbpd_pwr_if.c
patch('Appli/USBPD/App/usbpd_pwr_if.c', [
("""#include "string.h"
/* USER CODE BEGIN Include */

/* USER CODE END Include */
""",
 """#include "string.h"
/* USER CODE BEGIN Include */
/* This design has no VBUS sense input: the external source supplies VBUS
 * on its own connector, while this board is connected to the source only
 * through CC and GND.  CAD still needs logical VBUS state to transition
 * from AttachedWait to Attached.
 *
 * Kept in the USER CODE block so a CubeMX regeneration does not silently
 * re-enable the ADC-based VBUS path below. */
#define USBPD_CC_ONLY_TESTER 1
/* USER CODE END Include */
"""),
("""void USBPD_PWR_IF_GetPortPDOs(uint8_t PortNum, USBPD_CORE_DataInfoType_TypeDef DataId, uint8_t *Ptr, uint32_t *Size)
{
    {
      *Size = PORT0_NB_SINKPDO;
      memcpy(Ptr,PORT0_PDO_ListSNK, sizeof(uint32_t) * PORT0_NB_SINKPDO);
    }""",
 """void USBPD_PWR_IF_GetPortPDOs(uint8_t PortNum, USBPD_CORE_DataInfoType_TypeDef DataId, uint8_t *Ptr, uint32_t *Size)
{
    /* This board is a pure sink (PORT0_NB_SOURCEPDO == 0); the source PDO
       branch exists only so the CLI can still dump a source table. */
    UNUSED(PortNum);
    if (DataId == USBPD_CORE_DATATYPE_SNK_PDO)
    {
      *Size = PORT0_NB_SINKPDO;
      memcpy(Ptr, PORT0_PDO_ListSNK, sizeof(uint32_t) * PORT0_NB_SINKPDO);
    }
    else
    {
      *Size = PORT0_NB_SOURCEPDO;
      memcpy(Ptr, PORT0_PDO_ListSRC, sizeof(uint32_t) * PORT0_NB_SOURCEPDO);
    }"""),
])

print('OK')
