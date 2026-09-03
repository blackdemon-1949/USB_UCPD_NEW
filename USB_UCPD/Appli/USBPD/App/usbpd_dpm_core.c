/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbpd_dpm_core.c
  * @author  MCD Application Team
  * @brief   USBPD dpm core file
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

#define __USBPD_DPM_CORE_C

/* Includes ------------------------------------------------------------------*/
#include "usbpd_core.h"
#include "usbpd_trace.h"
#include "usbpd_dpm_core.h"
#include "usbpd_dpm_conf.h"
#include "usbpd_dpm_user.h"
#include "usbpd_hw_if.h"
#include "main.h"

#if defined(_LOW_POWER)
#include "usbpd_lowpower.h"
#endif /* _LOW_POWER */

/* OS management */
#include "usbpd_os_port_mx.h"

/* Private definition -------------------------------------------------------*/
/* function import prototypes -----------------------------------------------*/
/* Generic STM32 prototypes */
extern uint32_t HAL_GetTick(void);

/* Private function prototypes -----------------------------------------------*/
void USBPD_CAD_Task(void);
void USBPD_TaskUser(void);

#if defined(USE_STM32_UTILITY_OS)
void TimerCADfunction(void *);
#endif /* USE_STM32_UTILITY_OS */

#if defined(USE_STM32_UTILITY_OS)
void USBPD_PE_Task_P0(void);
void USBPD_PE_Task_P1(void);
void TimerPE0function(void *pArg);
void TimerPE1function(void *pArg);
#endif /* USE_STM32_UTILITY_OS */

/* Private typedef -----------------------------------------------------------*/
#if defined(USE_STM32_UTILITY_OS)
UTIL_TIMER_Object_t TimerCAD;
UTIL_TIMER_Object_t TimerPE0, TimerPE1;
#endif /* USE_STM32_UTILITY_OS */

/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
#define CHECK_PE_FUNCTION_CALL(_function_)  do{                                     \
                                                _retr = _function_;                  \
                                               if(USBPD_OK != _retr) {goto error;}   \
                                              } while(0);

#define CHECK_CAD_FUNCTION_CALL(_function_) if(USBPD_CAD_OK != _function_)      \
  {                                   \
    _retr = USBPD_ERROR;              \
    goto error;                       \
  }

#if defined(_DEBUG_TRACE)
#define DPM_CORE_DEBUG_TRACE(_PORTNUM_, __MESSAGE__)  \
  USBPD_TRACE_Add(USBPD_TRACE_DEBUG, _PORTNUM_, 0u, (uint8_t *)(__MESSAGE__), sizeof(__MESSAGE__) - 1u);
#else
#define DPM_CORE_DEBUG_TRACE(_PORTNUM_, __MESSAGE__)
#endif /* _DEBUG_TRACE */

/* Private variables ---------------------------------------------------------*/
#if !defined(USE_STM32_UTILITY_OS)
#define OFFSET_CAD 1U
static uint32_t DPM_Sleep_time[USBPD_PORT_COUNT + OFFSET_CAD];
static uint32_t DPM_Sleep_start[USBPD_PORT_COUNT + OFFSET_CAD];
#endif /* !USE_STM32_UTILITY_OS */

USBPD_ParamsTypeDef   DPM_Params[USBPD_PORT_COUNT];
/* Private function prototypes -----------------------------------------------*/
static void USBPD_PE_TaskWakeUp(uint8_t PortNum);
static void DPM_StartPETask(uint8_t PortNum);

void USBPD_DPM_CADCallback(uint8_t PortNum, USBPD_CAD_EVENT State, CCxPin_TypeDef Cc);

static void USBPD_DPM_CADTaskWakeUp(void);

/**
  * @brief  Initialize the core stack (port power role, PWR_IF, CAD and PE Init procedures)
  * @retval USBPD status
  */
/* Safe stubs for callback slots this sink does not implement.
 *
 * A disassembly sweep of every object in the v5.4.1 core library found the
 * PE calls several callback slots with 'ldr rN,[cb,#off]; blx rN' and NO
 * null check.  Among them, reachable from USBPD_PE_StateMachine_SNK:
 *     +0x24 PowerRoleSwap        (usbpd_pe_snk.o +0x8aa/+0x910/+0x970)
 *     +0x2C EvaluateVconnSwap    (usbpd_pe_vconn.o +0x292)
 * A NULL in any of those branches to address 0 and hard-faults.
 *
 * Supplying inert handlers costs nothing and removes a whole class of
 * lock-ups.  Each one answers truthfully for a VCONN-less, sink-only
 * board rather than pretending the operation succeeded. */
static USBPD_StatusTypeDef DPM_NoSetupNewPower(uint8_t PortNum)
{
  (void)PortNum;
  return USBPD_OK;      /* sink: nothing to reconfigure */
}

static USBPD_StatusTypeDef DPM_NoPRSwap(uint8_t PortNum)
{
  (void)PortNum;
  return USBPD_REJECT;  /* sink-only board: never becomes a source */
}

static USBPD_StatusTypeDef DPM_NoSrcEvaluateRequest(uint8_t PortNum,
                                                    USBPD_CORE_PDO_Type_TypeDef *PtrPowerObject)
{
  (void)PortNum;
  if (PtrPowerObject != NULL)
  {
    *PtrPowerObject = USBPD_CORE_PDO_TYPE_FIXED;
  }
  return USBPD_REJECT;  /* we are not a source */
}

static void DPM_NoPowerRoleSwap(uint8_t PortNum,
                                USBPD_PortPowerRole_TypeDef CurrentRole,
                                USBPD_PRS_Status_TypeDef Status)
{
  (void)PortNum;
  (void)CurrentRole;
  (void)Status;         /* no power-role swap on this board */
}

/* cb +0x40 (USBPD_PE_RequestDPMWhatToDo) is only populated under
 * USBPDCORE_EPR.  The app's table used to stop at IsPowerReady, leaving this
 * slot NULL; the library calls it unconditionally from its EPR source and
 * USB-data paths (disassembly: usbpd_pe_epr.o +0x2da, usbpd_pe_usbdata.o
 * +0xc2/+0x1e4), which would jump to address 0 on any build that reaches
 * those paths.  This board is a sink-only, non-USB-data device: answer
 * NOTSUPPORTED so the PE never acts on an action this app cannot honour. */
static uint32_t DPM_NoWhatToDo(uint8_t PortNum, uint32_t IDAction)
{
  (void)PortNum;
  (void)IDAction;
  return (uint32_t)USBPD_NOTSUPPORTED;
}


USBPD_StatusTypeDef USBPD_DPM_InitCore(void)
{
  /* variable to get dynamique memory allocated by usbpd stack */
  uint32_t stack_dynamemsize;
  USBPD_StatusTypeDef _retr = USBPD_OK;

  /* CAD callback definition */
  static const USBPD_PE_Callbacks dpmCallbacks =
  {
    DPM_NoSetupNewPower,
    USBPD_DPM_HardReset,
    DPM_NoPRSwap,
    USBPD_DPM_Notification,
    USBPD_DPM_ExtendedMessageReceived,
    USBPD_DPM_GetDataInfo,
    USBPD_DPM_SetDataInfo,
    DPM_NoSrcEvaluateRequest,
    USBPD_DPM_SNK_EvaluateCapabilities,
    DPM_NoPowerRoleSwap,
    USBPD_PE_TaskWakeUp,
    /* THE EPR BRICK.  These two slots must NEVER be NULL.
     *
     * PE_SubStateMachine_VconnSwap (usbpd_pe_vconn.o +0x292) does:
     *     ldr  r2, [r1, #0x2c]   ; cb->USBPD_PE_EvaluateVconnSwap
     *     blx  r2                ; NO null check
     * It calls the pointer unconditionally.  This project is built
     * WITHOUT _VCONN_SUPPORT, so the generated code shipped NULL here
     * and the branch jumped to address 0 -> HardFault.
     *
     * It only ever bit with an EPR source: PD3.1 requires the SOURCE to
     * be VCONN Source before it discovers the cable for EPR entry, so an
     * EPR charger starts a VCONN_Swap immediately after EPR_Mode(Enter),
     * while an SPR charger never does.  That is exactly the observed
     * behaviour: SPR fine, EPR locks the board until a power cycle.
     *
     * The handlers are always supplied now.  They are safe without a
     * VCONN supply: EvaluateVconnSwap answers from real capability and
     * VconnPwr reports failure rather than pretending. */
    USBPD_DPM_EvaluateVconnSwap,
    USBPD_DPM_PE_VconnPwr,
    USBPD_DPM_EnterErrorRecovery,
    USBPD_DPM_EvaluateDataRoleSwap,
    USBPD_DPM_IsPowerReady,
    DPM_NoWhatToDo
  };

  static const USBPD_CAD_Callbacks CAD_cbs =
  {
    USBPD_DPM_CADCallback,
    USBPD_DPM_CADTaskWakeUp
  };

  /* Check the lib selected */
  if (USBPD_TRUE != USBPD_PE_CheckLIB(LIB_ID))
  {
    _retr = USBPD_ERROR;
    goto error;
  }

  /* to get how much memory are dynamically allocated by the stack
     the memory return is corresponding to 2 ports so if the application
     managed only one port divide the value return by 2                   */
  stack_dynamemsize = USBPD_PE_GetMemoryConsumption();

  /* done to avoid warning */
  (void)stack_dynamemsize;

  /* Initialise the TRACE (TRACER_EMB over USART1 -> STM32CubeMonitor-UCPD) */
  USBPD_TRACE_Init();

  for (uint8_t _port_index = 0; _port_index < USBPD_PORT_COUNT; ++_port_index)
  {
    /* Variable to be sure that DPM is correctly initialized */
    DPM_Params[_port_index].DPM_Initialized = USBPD_FALSE;

    /* check the stack settings */
    DPM_Params[_port_index].PE_SpecRevision  = DPM_Settings[_port_index].PE_SpecRevision;
    DPM_Params[_port_index].PE_PowerRole     = DPM_Settings[_port_index].PE_DefaultRole;
    DPM_Params[_port_index].PE_SwapOngoing   = USBPD_FALSE;
    DPM_Params[_port_index].ActiveCCIs       = CCNONE;
    DPM_Params[_port_index].VconnCCIs        = CCNONE;
    DPM_Params[_port_index].VconnStatus      = USBPD_FALSE;

    /* CAD SET UP : Port 0 */
    CHECK_CAD_FUNCTION_CALL(USBPD_CAD_Init(_port_index,
                                           &CAD_cbs,
                                           &DPM_Settings[_port_index],
                                           &DPM_Params[_port_index]));

    /* PE SET UP : Port 0 */
    CHECK_PE_FUNCTION_CALL(USBPD_PE_Init(_port_index, (USBPD_SettingsTypeDef *)&DPM_Settings[_port_index],
                                         &DPM_Params[_port_index], &dpmCallbacks));

    /* DPM is correctly initialized */
    DPM_Params[_port_index].DPM_Initialized = USBPD_TRUE;

    /* Enable CAD on Port 0 */
    USBPD_CAD_PortEnable(_port_index, USBPD_CAD_ENABLE);
  }

#if defined(USE_STM32_UTILITY_OS)
  /* initialise timer server */
  UTIL_TIMER_Init();

  /* initialize the sequencer */
  UTIL_SEQ_Init();
#endif /* USE_STM32_UTILITY_OS */

#ifdef _LOW_POWER
  USBPD_LOWPOWER_Init();
#endif /* _LOW_POWER */

error :
  return _retr;
}

/**
  * @brief  Initialize the OS parts (task, queue,... )
  * @retval USBPD status
  */
USBPD_StatusTypeDef USBPD_DPM_InitOS(void)
{
  OS_INIT();
  return _retr;
}

/**
  * @brief  Initialize the OS parts (port power role, PWR_IF, CAD and PE Init procedures)
  * @retval None
  */
 /* NRTOS */
#if defined(USE_STM32_UTILITY_OS)
/**
  * @brief  Task for CAD processing
  * @retval None
  */
void USBPD_CAD_Task(void)
{
  UTIL_TIMER_Stop(&TimerCAD);
  uint32_t _timing = USBPD_CAD_Process();
  UTIL_TIMER_SetPeriod(&TimerCAD, _timing);
  UTIL_TIMER_Start(&TimerCAD);
}

/**
  * @brief  timer function to wakeup CAD Task
  * @param pArg Pointer on an argument
  * @retval None
  */
void TimerCADfunction(void *pArg)
{
  UTIL_SEQ_SetTask(TASK_CAD, 0);
}

#if !defined(USBPDCORE_LIB_NO_PD)
/**
  * @brief  timer function to wakeup PE_0 Task
  * @param pArg Pointer on an argument
  * @retval None
  */
void TimerPE0function(void *pArg)
{
  UTIL_SEQ_SetTask(TASK_PE_0, 0);
}

/**
  * @brief  timer function to wakeup PE_1 Task
  * @param pArg Pointer on an argument
  * @retval None
  */
void TimerPE1function(void *pArg)
{
  UTIL_SEQ_SetTask(TASK_PE_1, 0);
}

/**
  * @brief  Task for PE_0 processing
  * @retval None
  */
void USBPD_PE_Task_P0(void)
{
  UTIL_TIMER_Stop(&TimerPE0);
  uint32_t _timing =
    USBPD_PE_StateMachine_SNK(USBPD_PORT_0);
  if (_timing != 0xFFFFFFFF)
  {
    UTIL_TIMER_SetPeriod(&TimerPE0, _timing);
    UTIL_TIMER_Start(&TimerPE0);
  }
}

/**
  * @brief  Task for PE_1 processing
  * @retval None
  */
void USBPD_PE_Task_P1(void)
{
  UTIL_TIMER_Stop(&TimerPE1);
  uint32_t _timing =
    USBPD_PE_StateMachine_SNK(USBPD_PORT_1);
  if (_timing != 0xFFFFFFFF)
  {
    UTIL_TIMER_SetPeriod(&TimerPE1, _timing);
    UTIL_TIMER_Start(&TimerPE1);
  }
}
#endif

/**
  * @brief  Task for DPM_USER processing
  * @retval None
  */
void USBPD_TaskUser(void)
{
  USBPD_DPM_UserExecute(NULL);
}
#endif /* USE_STM32_UTILITY_OS */

void USBPD_DPM_Run(void)
{
#if defined(USE_STM32_UTILITY_OS)
  /* DPM_Run is called from the application super loop.  Registration and
   * timer creation are one-time operations; running them on every pass
   * leaks/replaces the utility-OS objects. */
  static uint8_t os_started;
  if (os_started == 0U)
  {
    os_started = 1U;
    UTIL_SEQ_RegTask(TASK_CAD,  0, USBPD_CAD_Task);
  UTIL_SEQ_SetTask(TASK_CAD,  0);
  UTIL_TIMER_Create(&TimerCAD, 10, UTIL_TIMER_ONESHOT, TimerCADfunction, NULL);

  UTIL_SEQ_RegTask(TASK_PE_0, 0,  USBPD_PE_Task_P0);
  UTIL_SEQ_PauseTask(TASK_PE_0);
  UTIL_TIMER_Create(&TimerPE0, 10, UTIL_TIMER_ONESHOT, TimerPE0function, NULL);
#if USBPD_PORT_COUNT == 2
  UTIL_SEQ_RegTask(TASK_PE_1, 0,  USBPD_PE_Task_P1);
  UTIL_SEQ_PauseTask(TASK_PE_1);
  UTIL_TIMER_Create(&TimerPE1, 10, UTIL_TIMER_ONESHOT, TimerPE1function, NULL);
#endif /* USBPD_PORT_COUNT == 2 */

    UTIL_SEQ_RegTask(TASK_USER, 0, USBPD_TaskUser);
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
      /* EPR-freeze telemetry: one-shot checkpoints on the trace UART while
       * the app has g_usbpd_tele armed (set when `epr enter` is accepted).
       * >B before the PE run, >E only if the PE run returns.  A freeze
       * inside the closed PE/PRL code therefore leaves ">B" with no ">E".
       */
      if (g_usbpd_tele != 0u)
      {
        USBPD_HW_IF_Tele("\r\n>B\r\n");
      }
      DPM_Sleep_time[port] = USBPD_PE_StateMachine_SNK(port);
      DPM_Sleep_start[port] = HAL_GetTick();
      if (g_usbpd_tele != 0u)
      {
        USBPD_HW_IF_Tele("\r\n>E\r\n");
      }
    }
  }

  USBPD_DPM_UserExecute(NULL);
#endif /* USE_STM32_UTILITY_OS */
}

/**
  * @brief  Initialize DPM (port power role, PWR_IF, CAD and PE Init procedures)
  * @retval USBPD status
  */
void USBPD_DPM_TimerCounter(void)
{
  /* Call PE/PRL timers functions only if DPM is initialized */
  if (USBPD_TRUE == DPM_Params[USBPD_PORT_0].DPM_Initialized)
  {
    USBPD_DPM_UserTimerCounter(USBPD_PORT_0);
    USBPD_PE_TimerCounter(USBPD_PORT_0);
    USBPD_PRL_TimerCounter(USBPD_PORT_0);
  }
#if USBPD_PORT_COUNT==2
  if (USBPD_TRUE == DPM_Params[USBPD_PORT_1].DPM_Initialized)
  {
    USBPD_DPM_UserTimerCounter(USBPD_PORT_1);
    USBPD_PE_TimerCounter(USBPD_PORT_1);
    USBPD_PRL_TimerCounter(USBPD_PORT_1);
  }
#endif /* USBPD_PORT_COUNT == 2 */

}

/**
  * @brief  WakeUp PE task
  * @param  PortNum port number
  * @retval None
  */
static void USBPD_PE_TaskWakeUp(uint8_t PortNum)
{
#if defined(USE_STM32_UTILITY_OS)
  UTIL_SEQ_SetTask(PortNum == 0 ? TASK_PE_0 : TASK_PE_1, 0);
#else
  DPM_Sleep_time[PortNum] = 0;
#endif /* USE_STM32_UTILITY_OS */
}

/**
  * @brief  WakeUp CAD task
  * @retval None
  */
static void USBPD_DPM_CADTaskWakeUp(void)
{
#if defined(USE_STM32_UTILITY_OS)
  UTIL_SEQ_SetTask(TASK_CAD, 0);
#else
  DPM_Sleep_time[USBPD_PORT_COUNT] = 0;
#endif /* USE_STM32_UTILITY_OS */
}

/**
  * @brief  CallBack reporting events on a specified port from CAD layer.
  * @param  PortNum   The handle of the port
  * @param  State     CAD state
  * @param  Cc        The Communication Channel for the USBPD communication
  * @retval None
  */
void USBPD_DPM_CADCallback(uint8_t PortNum, USBPD_CAD_EVENT State, CCxPin_TypeDef Cc)
{
  USBPD_TRACE_Add(USBPD_TRACE_CADEVENT, PortNum, (uint8_t)State, NULL, 0);
  /* _TRACE */
  /* app_pd.c reads DPM_Params[port].ActiveCCIs to report the CC pin in use. */
  DPM_Params[PortNum].ActiveCCIs = Cc;
  switch (State)
  {
    case USBPD_CAD_EVENT_ATTEMC :
    {
#if defined(_VCONN_SUPPORT)
      DPM_Params[PortNum].VconnStatus = USBPD_TRUE;
#endif /* _VCONN_SUPPORT */
      USBPD_DPM_UserCableDetection(PortNum, USBPD_CAD_EVENT_ATTEMC);
      DPM_StartPETask(PortNum);
      break;
    }
    case USBPD_CAD_EVENT_ATTACHED :
      USBPD_DPM_UserCableDetection(PortNum, USBPD_CAD_EVENT_ATTACHED);
      DPM_StartPETask(PortNum);
      break;

    case USBPD_CAD_EVENT_DETACHED :
    case USBPD_CAD_EVENT_EMC :
    {
      /* Terminate PE task */
#if defined(USE_STM32_UTILITY_OS)
      UTIL_SEQ_PauseTask(PortNum == 0 ? TASK_PE_0 : TASK_PE_1);
#else
      DPM_Sleep_time[PortNum] = 0xFFFFFFFFU;
#endif /* USE_STM32_UTILITY_OS */
      DPM_Params[PortNum].PE_SwapOngoing = USBPD_FALSE;
      DPM_Params[PortNum].PE_Power   = USBPD_POWER_NO;
      USBPD_DPM_UserCableDetection(PortNum, State);
#ifdef _VCONN_SUPPORT
      DPM_Params[PortNum].VconnStatus = USBPD_FALSE;
      DPM_CORE_DEBUG_TRACE(PortNum, "Note: VconnStatus=FALSE");
#endif /* _VCONN_SUPPORT */
      break;
    }
    default :
      /* nothing to do */
      break;
  }
}

static void DPM_StartPETask(uint8_t PortNum)
{
  USBPD_PE_StateMachine_Reset(PortNum);
#if defined(USE_STM32_UTILITY_OS)
  /* Resume the task */
  UTIL_SEQ_ResumeTask(PortNum == 0 ? TASK_PE_0 : TASK_PE_1);
  /* Enable task execution */
  UTIL_SEQ_SetTask(PortNum == 0 ? TASK_PE_0 : TASK_PE_1, 0);
#else
  DPM_Sleep_time[PortNum] = 0U;
#endif /* USE_STM32_UTILITY_OS */
}

__WEAK void USBPD_DPM_ErrorHandler(void)
{
  /* This function is called to block application execution
     in case of an unexpected behavior.  The stock implementation spins in
     while(1) which freezes the board silently (solid PB2, dead USB
     console).  Make the failure visible instead: LED blink code 8. */
  Appli_Fatal(8);
}

