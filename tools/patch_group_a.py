#!/usr/bin/env python3
"""Repair the interrupt priorities, USB device bring-up and CDC DMA placement
that the CubeMX regeneration reverted to stock ST defaults."""
import sys, os

ROOT = sys.argv[1]

def load(rel):
    p = os.path.join(ROOT, rel)
    raw = open(p, 'rb').read()
    crlf = b'\r\n' in raw
    t = raw.decode('utf-8')
    if crlf:
        t = t.replace('\r\n', '\n')
    return p, t, crlf

def save(p, t, crlf):
    if crlf:
        t = t.replace('\n', '\r\n')
    open(p, 'wb').write(t.encode('utf-8'))

def patch(rel, pairs):
    p, t, crlf = load(rel)
    for old, new in pairs:
        n = t.count(old)
        assert n == 1, '%s: expected 1 occurrence, found %d of:\n%r' % (rel, n, old[:220])
        t = t.replace(old, new)
    save(p, t, crlf)
    print('patched', rel)

# ------------------------------------------------------------------ gpdma.c
patch('Appli/Core/Src/gpdma.c', [
("""  /* GPDMA1 interrupt Init */
    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);
""",
 """  /* GPDMA1 interrupt Init
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
"""),
("""  /* USER CODE BEGIN GPDMA1_Init 1 */
  /* USBPD trace TX (tracer_emb over USART3 DMA) uses GPDMA1 Channel 2.
     The .ioc does not model this channel, so a plain CubeMX regeneration
     would drop the NVIC setup below.  It lives in this USER CODE section
     on purpose: regeneration keeps it. */
    HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
  /* USER CODE END GPDMA1_Init 1 */""",
 """  /* USER CODE BEGIN GPDMA1_Init 1 */
  /* USBPD trace TX (TRACER_EMB over USART1 DMA) uses GPDMA1 Channel 3 in this
     configuration - see TRACER_EMB_TX_DMA_CHANNEL in tracer_emb_conf.h.
     Tracing must never delay USB enumeration (4) or PD message handling (5),
     so the trace channels sit one level below both. */
    HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);
  /* USER CODE END GPDMA1_Init 1 */"""),
])

# ------------------------------------------------------------------- ucpd.c
patch('Appli/Core/Src/ucpd.c', [
("""  /* UCPD1 interrupt Init */
  NVIC_SetPriority(UCPD1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),0, 0));""",
 """  /* UCPD1 interrupt Init
   * Priority 5: below USB OTG_HS (4) so USB enumeration cannot be starved
   * by PD bursts, above SysTick (15) so the PRL keeps its timing.  The CAD
   * layer re-applies this via UCPD_INSTANCE0_ENABLEIRQ
   * (usbpd_devices_conf.h); keep the two in sync. */
  NVIC_SetPriority(UCPD1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),5, 0));"""),
])

# ------------------------------------------------------- usbpd_devices_conf.h
patch('Appli/USBPD/Target/usbpd_devices_conf.h', [
("""#define UCPD_INSTANCE0_ENABLEIRQ  do{\\
                                        NVIC_SetPriority(UCPD1_IRQn,0);                              \\""",
 """/* UCPD1 interrupt priority.
 * The ST default of 0 makes the UCPD ISR the highest priority task in the
 * system.  While a PD negotiation runs at the same time the USB cable is
 * plugged in, UCPD/PRL interrupt bursts then starve the OTG_HS interrupt
 * (enumeration is timing critical -> "corrupt" CDC in the device manager)
 * and the SysTick-driven CAD/PE software timers.  USB keeps priority 4,
 * UCPD and its DMA channels get 5: both stacks stay responsive. */
#define UCPD_INSTANCE0_ENABLEIRQ  do{\\
                                        NVIC_SetPriority(UCPD1_IRQn,5);                              \\""")])

# ----------------------------------------------------------- tracer_emb_conf.h
patch('Appli/Core/Inc/tracer_emb_conf.h', [
("""#define TRACER_EMB_TX_IRQ_PRIORITY                   0""",
 """/* USART1 trace IRQ priority.  6 = just below USB OTG (4) and UCPD (5):
 * tracing must never delay USB enumeration or PD message handling. */
#define TRACER_EMB_TX_IRQ_PRIORITY                   6"""),
("""  * @author  MCD Application Team
  * @brief   This file contains the Trace HW related defines.""",
 """  * @author  MCD Application Team, adapted for the WeAct H7R3Z8 board
  * @brief   Trace HW related defines (TRACER_EMB).
  *
  * Wiring on the WeAct STM32H7R3Z8J6 board:
  *     PB6  = USART1_TX  ->  USB-UART adapter RX   (AF7)
  *     PB7  = USART1_RX  ->  USB-UART adapter TX   (AF7)
  *     GND  = GND        ->  USB-UART adapter GND  (3.3 V logic!)
  * The trace TX DMA is GPDMA1 channel 3; in STM32CubeMonitor-UCPD add a board
  * node, select the TRACER interface and pick the adapter's COM port
  * (921600 baud, 8N1)."""),
])

# --------------------------------------------------------------- usbd_conf.c
patch('Appli/USB_DEVICE/Target/usbd_conf.c', [
("""    /* Peripheral interrupt init */
    HAL_NVIC_SetPriority(OTG_HS_IRQn, 6, 0);""",
 """    /* Peripheral interrupt init */
    /* USB OTG must outrank UCPD1 (priority 5): enumeration is timing
       critical and must not be starved by UCPD/PRL interrupt bursts while
       a PD source negotiates at the same time the cable is plugged. */
    HAL_NVIC_SetPriority(OTG_HS_IRQn, 4, 0);"""),
("""  else
  {
    Error_Handler();
  }
    /* Set Speed. */""",
 """  else
  {
    /* Garbage enum speed (USB_GetDevSpeed can return 0xF, e.g. when the
       PHY reference clock is marginal).  Running Error_Handler() from the
       USB IRQ used to brick the whole board (solid PB2, dead PD stack).
       Fall back to full-speed descriptors instead and keep running. */
    speed = USBD_SPEED_FULL;
  }
    /* Set Speed. */"""),
("""  /* Inform USB library that core enters in suspend Mode. */
  USBD_LL_Suspend((USBD_HandleTypeDef*)hpcd->pData);
  /* Enter in STOP mode. */""",
 """  /* Inform USB library that core enters in suspend Mode. */
  USBD_LL_Suspend((USBD_HandleTypeDef*)hpcd->pData);
  /* Drop the console session; an IN transfer aborted by the suspend can
     never complete, which would otherwise leave the logger stuck busy. */
  APP_LOG_OnUsbSuspend();
  /* Enter in STOP mode. */"""),
])

# --------------------------------------------------------------- usb_device.c
patch('Appli/USB_DEVICE/App/usb_device.c', [
("""  /* Init Device Library, add supported class and start the library. */
  if (USBD_Init(&hUsbDeviceHS, &CDC_Desc, DEVICE_HS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceHS, &USBD_CDC) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_CDC_RegisterInterface(&hUsbDeviceHS, &USBD_Interface_fops_HS) != USBD_OK)
  {
    Error_Handler();
  }
""",
 """  /* Init Device Library, add supported class and start the library.
     A failure here used to call Error_Handler() (solid PB2, dead board);
     the PD bench part of the firmware must survive a USB problem, so log
     it and keep running without the console instead. */
  if (USBD_Init(&hUsbDeviceHS, &CDC_Desc, DEVICE_HS) == USBD_OK)
  {
    if (USBD_RegisterClass(&hUsbDeviceHS, &USBD_CDC) == USBD_OK)
    {
      if (USBD_CDC_RegisterInterface(&hUsbDeviceHS, &USBD_Interface_fops_HS) == USBD_OK)
      {
        if (USBD_Start(&hUsbDeviceHS) == USBD_OK)
        {
          usb_device_ok = 1U;
        }
      }
    }
  }
  if (usb_device_ok == 0U)
  {
    APP_LOG_Write("usb: CDC init failed - running without the serial console\\r\\n");
  }
""")])

# ------------------------------------------------------------- usbd_cdc_if.c
patch('Appli/USB_DEVICE/App/usbd_cdc_if.c', [
("""/** Received data over USB are stored in this buffer      */
uint8_t UserRxBufferHS[APP_RX_DATA_SIZE];

/** Data to send over USB CDC are stored in this buffer   */
uint8_t UserTxBufferHS[APP_TX_DATA_SIZE];""",
 """/** Received data over USB are stored in this buffer      */
/* USB OTG HS DMA is not cache coherent with the Cortex-M7.  Keep CDC
 * buffers in the linker-provided non-cacheable AXI SRAM window. */
uint8_t UserRxBufferHS[APP_RX_DATA_SIZE]
  __attribute__((section("noncacheable_buffer"), aligned(32)));

/** Data to send over USB CDC are stored in this buffer   */
uint8_t UserTxBufferHS[APP_TX_DATA_SIZE]
  __attribute__((section("noncacheable_buffer"), aligned(32)));"""),
])

# -------------------------------------------------------------- usbd_desc.c
patch('Appli/USB_DEVICE/App/usbd_desc.c', [
('#define USBD_MANUFACTURER_STRING     "STMicroelectronics"',
 '#define USBD_MANUFACTURER_STRING     "WeAct / PD Bench"'),
('#define USBD_PRODUCT_STRING     "STM32 Virtual ComPort"',
 '#define USBD_PRODUCT_STRING     "H7R3 PD Sink"'),
])

print('OK')
