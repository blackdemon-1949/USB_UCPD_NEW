/*
 * pd_tr_st.c - pdsink UCPD transport on the ST H7RS open device layer.
 *
 * Implements the whole pd_tr.h contract (see the header for the driver-side
 * semantics) on top of the proven open ST USB-PD device layer that this
 * project already ships and debugs:
 *
 *     usbpd_phy.c        - PHY API + callback wiring
 *     usbpd_phy_hw_if.c  - resistor / CC control, TX DMA path, attach
 *     usbpd_hw_if_it.c   - UCPD1 IRQ dispatch into Ports[].cbs
 *     usbpd_cad_hw_if.c  - CAD_Init(): UCPD clock/analog/SNK-Rd detection init
 *     usbpd_hw.c         - instance + DMA channel helpers
 *     usbpd_timersserver.c
 *
 * Only CAD_Init() and the PHY layer are used here; the closed USBPD core
 * library is NOT part of this path (pdsink's TC/PRL/PE replaces it).  The
 * device layer is driven through exactly the same seams the closed core
 * used, so the hardware behaviour (BMC bit clock, CC termination classes,
 * DMA error containment, ...) is the previously validated one.
 *
 * Callbacks in this file run in the UCPD1 IRQ context and only do what the
 * driver allows from IRQ context (see pd_ucpd_driver.cpp): copy the frame
 * into the driver ring, or latch TX/HR/CC completion.  All heavier work is
 * deferred to UcpdDriver::service(), polled from the main loop.
 *
 * Wiring (CubeIDE project, single port USBPD_PORT_0):
 *   - add this file to the Appli build,
 *   - call pd_tr_init() before pdsink's Task::start(),
 *   - call pdport_service() (the pdsink 1 ms pump) from the main loop.
 * See Middlewares/PDEngine/port/README.md for the full step list.
 *
 * RX frame sizing: the UCPD validates and strips the CRC in hardware; the
 * payload byte count of the just-received message is RX_PAYSZ (the same
 * field the TX path programs via TX_PAYSZ, which excludes the CRC).  The
 * DMA is armed with the maximum transaction size; RX_PAYSZ is therefore
 * the only exact length source and is captured inside the RX-complete
 * callback before the next message can overwrite it.
 */

#include <string.h>

#include "pd_tr.h"

#include "usbpd_devices_conf.h"   /* UCPD1 instance, GPDMA channels, LL      */
#include "usbpd_core.h"           /* USBPD types/defines (open headers)      */
#include "usbpd_phy.h"            /* USBPD_PHY_* API + USBPD_PHY_Callbacks   */
#include "usbpd_hw_if.h"          /* Ports[], HW_SignalAttachement, CC codes */
#include "usbpd_cad_hw_if.h"      /* CAD_Init()                              */

/* Single-port transport.  The project is a one-port (USBPD_PORT_0) board;
 * the pdsink stack is single-port by construction. */
#define TR_PORT USBPD_PORT_0

extern uint32_t HAL_GetTick(void);   /* declared here, defined by the HAL */

/* ---------------------------------------------------------------------- */
/* Private state                                                          */
/* ---------------------------------------------------------------------- */

/* RX buffer handed to the device layer.  The UCPD RX DMA is re-armed with
 * SIZE_MAX_PD_TRANSACTION_UNCHUNK (264 B) on every message, so this buffer
 * must be at least that large.  It is the same buffer the closed core used
 * to use for the same purpose (CacheInvalidate/CacheClean are managed by
 * the device layer around RX/TX). */
static uint8_t tr_rx_buf[SIZE_MAX_PD_TRANSACTION_UNCHUNK]
                        __attribute__((aligned(32)));

/* Settings/params owned by this transport (the closed core used to own
 * equivalent instances).  Zero initialisation is correct for a sink-only
 * port: every feature flag the CAD/PHY layer consults (FRS, VCONN, DRP
 * toggling, accessories) is left disabled. */
static USBPD_SettingsTypeDef tr_settings;
static USBPD_ParamsTypeDef   tr_params;

/* Attach bookkeeping.  tx_ok mirrors the SinkTxOK termination state so the
 * transport can answer pd_tr_tx_busy() truthfully while a GoodCRC TX is in
 * flight and keep the CC analog settled between reads. */
static uint8_t tr_attached;   /* UCPD PD path armed on the active CC        */
static uint8_t tr_sink_tx_ok; /* SinkTxOK (Rp 3.0 A class) presented        */
static uint8_t tr_cc_line;    /* active CC line for comms (CC1/CC2/CCNONE)  */

/* ---------------------------------------------------------------------- */
/* Level decode (sink role, per the UCPD SR Type-C comparator table)      */
/* ---------------------------------------------------------------------- */

/* SR TYPEC_VSTATE_CCx[1:0] while the port presents Rd:
 *   00 = vRa/open            -> no source pull-up        -> PD_CC_NONE
 *   01 = vRdUSB (default)    -> partner Rp 0.5 A class   -> PD_CC_RP_0_5
 *   10 = vRd1.5              -> partner Rp 1.5 A class   -> PD_CC_RP_1_5
 *   11 = vRd3.0              -> partner Rp 3.0 A class   -> PD_CC_RP_3_0
 * (LL_UCPD_SNK_CCx_VRP/VRP15A/VRP30A constants, sink column of the table
 * in CAD_Check_HW_SNK.) */
static int tr_cc_code_to_level(uint32_t code)
{
  switch (code)
  {
    case 1u: return PD_CC_RP_0_5;  /* LL_UCPD_SNK_CCx_VRP    */
    case 2u: return PD_CC_RP_1_5;  /* LL_UCPD_SNK_CCx_VRP15A */
    case 3u: return PD_CC_RP_3_0;  /* LL_UCPD_SNK_CCx_VRP30A */
    default: return PD_CC_NONE;    /* open / vRa             */
  }
}

static uint32_t tr_read_sr(void)
{
  return Ports[TR_PORT].husbpd->SR;
}

/* ---------------------------------------------------------------------- */
/* USBPD_PHY callbacks (invoked from the UCPD1 IRQ path)                  */
/* ---------------------------------------------------------------------- */

/* A complete, CRC-valid message has been received.  `Type` is a
 * USBPD_SOPTYPE_* value; the payload sits in Ports[].ptr_RxBuff (== our
 * tr_rx_buf) and its exact byte count is RX_PAYSZ (CRC excluded). */
static void TR_MessageReceived(uint8_t PortNum, USBPD_SOPType_TypeDef Type)
{
  (void)PortNum;
  UCPD_TypeDef *hucpd = Ports[TR_PORT].husbpd;
  uint32_t pay_size = LL_UCPD_ReadRxPaySize(hucpd);

  /* Only SOP frames are forwarded to the stack by the PHY (SupportedSOP),
   * so Type is USBPD_SOPTYPE_SOP here; belt-and-braces keep the mapping
   * explicit for future SOP'/SOP" work. */
  if (Type != USBPD_SOPTYPE_SOP)
  {
    return;
  }

  /* Sanity: a PD frame is 2-byte header + payload, never longer than the
   * DMA window.  The driver drops anything above PD_TR_MAX_FRAME (the
   * stack chunks extended messages); it is cheaper to filter here. */
  if ((pay_size < 2u) || (pay_size > PD_TR_MAX_FRAME) ||
      (pay_size > (uint32_t)sizeof(tr_rx_buf)))
  {
    return;
  }

  /* Forward: the driver copies the frame into its ring, arms the GoodCRC
   * reply when the wire is free and requests a stack wake-up. */
  pd_drv_on_rx_frame(tr_rx_buf, (uint8_t)pay_size, PD_SOP_SOP);
}

/* A reset indication was received (hard reset signalling, or cable reset
 * order set).  Only partner hard resets interest the sink stack. */
static void TR_ResetIndication(uint8_t PortNum, USBPD_SOPType_TypeDef Type)
{
  (void)PortNum;
  if (Type == USBPD_SOPTYPE_HARD_RESET)
  {
    pd_drv_on_hr_rx();
  }
  /* CABLE_RESET: nothing to do - the driver never queues SOP'/SOP" and a
   * cable reset on a plain sink port is dropped like any non-SOP frame. */
}

/* The TX side of a reset completed (TxHRSTSENT): a hard reset burst we
 * started has been shifted out. */
static void TR_ResetCompleted(uint8_t PortNum, USBPD_SOPType_TypeDef Type)
{
  (void)PortNum;
  if (Type == USBPD_SOPTYPE_HARD_RESET)
  {
    pd_drv_on_hr_done();
  }
}

/* Normal (non-reset) frame transmission finished: 0 = sent, 1 = discarded,
 * 2 = aborted - the exact status contract of pd_drv_on_tx_done(). */
static void TR_TxCompleted(uint8_t PortNum, uint32_t Status)
{
  (void)PortNum;
  pd_drv_on_tx_done((int)Status);
}

/* BIST patterns are a source-side conformance feature; a sink port never
 * arms one.  Keep the slot non-NULL (the device layer dereferences the
 * callback table unconditionally on TX events). */
static void TR_BistCompleted(uint8_t PortNum, USBPD_BISTMsg_TypeDef bistmode)
{
  (void)PortNum;
  (void)bistmode;
}

/* Fast Role Swap is not supported on this board (sink, no VBUS source
 * path).  Slot must stay non-NULL for the same reason as above. */
static void TR_FastRoleSwapReception(uint8_t PortNum)
{
  (void)PortNum;
}

static const USBPD_PHY_Callbacks tr_phy_cbs =
{
  TR_MessageReceived,
  TR_ResetIndication,
  TR_ResetCompleted,
  TR_BistCompleted,
  TR_TxCompleted,
  TR_FastRoleSwapReception
};

/* CC event wake-up hook (TYPECEVT1/2 IRQ path in usbpd_hw_if_it.c calls
 * Ports[].USBPD_CAD_WakeUp()).  pdsink's TC scans CC levels on its own 1 ms
 * schedule, so this only makes the scan happen sooner. */
static void TR_CcWakeUp(void)
{
  pd_drv_on_cc_event();
}

/* ---------------------------------------------------------------------- */
/* pd_tr.h: driver -> transport (main loop context)                       */
/* ---------------------------------------------------------------------- */

int pd_tr_send_frame(uint8_t sop, const uint8_t *buf, uint16_t len)
{
  USBPD_StatusTypeDef st;

  if (buf == NULL || len < 2u || len > PD_TR_MAX_FRAME)
  {
    return -1;
  }
  if (sop != PD_SOP_SOP)
  {
    /* Sink-only port: all stack traffic is SOP; SOP'/SOP"/debug are not
     * supported (SupportedSOP = SOP only). */
    return -1;
  }
  if (!tr_attached)
  {
    return -1;   /* PD path not armed yet: caller keeps retrying */
  }

  /* SendBuffer() refuses (USBPD_ERROR) while an RX transaction is in
   * progress or the TX DMA is still enabled - the exact "busy" condition
   * the driver expects (-1 -> defer / retry).  When it accepts, the frame
   * is fully handed to the DMA and TX-complete arrives through
   * TR_TxCompleted(). */
  st = USBPD_PHY_SendMessage(TR_PORT, USBPD_SOPTYPE_SOP,
                             (uint8_t *)buf, len);
  return (st == USBPD_OK) ? 0 : -1;
}

int pd_tr_send_hard_reset(void)
{
  if (!tr_attached)
  {
    return -1;
  }
  /* USBPD_PHY_ResetRequest == SendMessage(HARD_RESET, NULL, 0): the device
   * layer drives LL_UCPD_SendHardReset directly (no DMA); completion is
   * reported through TR_ResetCompleted() -> pd_drv_on_hr_done(). */
  return (USBPD_PHY_ResetRequest(TR_PORT, USBPD_SOPTYPE_HARD_RESET) ==
          USBPD_OK) ? 0 : -1;
}

void pd_tr_set_sink_tx_ok(int enable)
{
  if (enable)
  {
    if (!tr_sink_tx_ok)
    {
      /* Present the SinkTxOK termination: internal pull set to the 3.0 A
       * class with the matching factory trim - the identical sequence the
       * ST closed-core PRL used before its AMS transmissions. */
      USBPD_PHY_SetResistor_SinkTxOK(TR_PORT);
      tr_sink_tx_ok = 1u;
    }
  }
  else
  {
    if (tr_sink_tx_ok)
    {
      USBPD_PHY_SetResistor_SinkTxNG(TR_PORT);
      tr_sink_tx_ok = 0u;
    }
  }
}

void pd_tr_read_cc(int *cc1, int *cc2)
{
  uint32_t sr = tr_read_sr();

  if (cc1 != NULL)
  {
    *cc1 = tr_cc_code_to_level(
             (sr & UCPD_SR_TYPEC_VSTATE_CC1_Msk) >>
             UCPD_SR_TYPEC_VSTATE_CC1_Pos);
  }
  if (cc2 != NULL)
  {
    *cc2 = tr_cc_code_to_level(
             (sr & UCPD_SR_TYPEC_VSTATE_CC2_Msk) >>
             UCPD_SR_TYPEC_VSTATE_CC2_Pos);
  }
}

int pd_tr_read_active_cc(void)
{
  uint32_t sr = tr_read_sr();

  switch (Ports[TR_PORT].CCx)
  {
    case CC1:
      return tr_cc_code_to_level(
               (sr & UCPD_SR_TYPEC_VSTATE_CC1_Msk) >>
               UCPD_SR_TYPEC_VSTATE_CC1_Pos);
    case CC2:
      return tr_cc_code_to_level(
               (sr & UCPD_SR_TYPEC_VSTATE_CC2_Msk) >>
               UCPD_SR_TYPEC_VSTATE_CC2_Pos);
    default:
      return PD_CC_NONE;
  }
}

void pd_tr_set_active_cc(int cc)
{
  /* The CC pin used by the UCPD comparator/DMA path is selected inside
   * HW_SignalAttachement(); remember the choice so read_active_cc() and
   * the level helpers agree with the armed line. */
  tr_cc_line = (cc == 1) ? (uint8_t)CC1 : (uint8_t)CC2;
}

void pd_tr_attach(int cc)
{
  if (tr_attached)
  {
    return;   /* already armed on the current line */
  }

  /* HW_SignalAttachement() performs the whole PD-path bring-up the closed
   * CAD used after debounce: TX/RX GPDMA instance init, RX DMA arming on
   * ptr_RxBuff, UCPD RX/TX DMA enable, NORMAL RX mode, IMR PD-event mask,
   * CC pin select and CCx enable. */
  HW_SignalAttachement(TR_PORT, (cc == 1) ? CC1 : CC2);
  tr_cc_line = (cc == 1) ? (uint8_t)CC1 : (uint8_t)CC2;
  tr_attached = 1u;

  /* Sync the shared params the board/diag code reads (ActiveCCIs).  Power
   * contract state is owned by the pdsink glue once PS_RDY lands; start
   * from the safe "no contract" state. */
  tr_params.ActiveCCIs = Ports[TR_PORT].CCx;
  tr_params.PE_Power = USBPD_POWER_NO;
}

void pd_tr_detach(void)
{
  if (!tr_attached)
  {
    return;
  }
  /* Full teardown back to the detection stage: RX/TX DMA off, RX disabled,
   * IMR reduced to the Type-C event bits, Rd asserted on both lines. */
  HW_SignalDetachment(TR_PORT);
  tr_attached = 0u;
  tr_sink_tx_ok = 0u;      /* termination was torn down with the PD path  */
  tr_cc_line = CCNONE;
  tr_params.ActiveCCIs = CCNONE;
  tr_params.PE_Power = USBPD_POWER_NO;
}

int pd_tr_vbus_ok(void)
{
  int cc1 = PD_CC_NONE;
  int cc2 = PD_CC_NONE;

  /* CC-only board: a source that presents Rp on CC supplies VBUS by
   * definition (see the pd_tr.h contract note). */
  if (tr_attached)
  {
    return 1;
  }
  pd_tr_read_cc(&cc1, &cc2);
  return (cc1 != PD_CC_NONE) || (cc2 != PD_CC_NONE);
}

int pd_tr_tx_busy(void)
{
  UCPD_TypeDef *hucpd = Ports[TR_PORT].husbpd;

  /* Busy while a frame is shifting out (TX DMA enabled) or while an RX
   * transaction is in progress (RXStatus latched by RXORDDET, cleared at
   * RXMSGEND) - exactly the conditions under which the device layer's
   * SendBuffer() refuses a new transmission. */
  if ((hucpd->IMR != 0u) && (Ports[TR_PORT].hdmatx != NULL) &&
      ((Ports[TR_PORT].hdmatx->CCR & DMA_CCR_EN) != 0u))
  {
    return 1;
  }
  if (Ports[TR_PORT].RXStatus == USBPD_TRUE)
  {
    return 1;
  }
  return 0;
}

uint32_t pd_tr_now_ms(void)
{
  return HAL_GetTick();
}

/* ---------------------------------------------------------------------- */
/* Init                                                                   */
/* ---------------------------------------------------------------------- */

int pd_tr_init(void)
{
  static uint8_t done;

  if (done)
  {
    return 0;
  }

  /* Port role: sink-only.  Everything else stays at zero (disabled). */
  tr_params.PE_PowerRole = USBPD_PORTPOWERROLE_SNK;
  tr_params.PE_SwapOngoing = USBPD_FALSE;
  tr_params.PE_SpecRevision = USBPD_SPECIFICATION_REV3;
  tr_params.DPM_Initialized = USBPD_FALSE;

  tr_settings.PE_DefaultRole = USBPD_PORTPOWERROLE_SNK;
  tr_settings.PE_SpecRevision = USBPD_SPECIFICATION_REV3;
  tr_settings.PE_SupportedSOP = USBPD_SUPPORTED_SOP_SOP;

  /* Register the PHY callbacks and the RX buffer with the device layer.
   * SupportedSOP = SOP only: the PHY then drops SOP'/SOP"/debug/cable
   * traffic before it reaches the driver. */
  (void)USBPD_PHY_Init(TR_PORT, &tr_phy_cbs, tr_rx_buf,
                       USBPD_PORTPOWERROLE_SNK,
                       (uint32_t)USBPD_SUPPORTED_SOP_SOP);

  /* UCPD clock/analog init + SNK detection stage (Rd on both CC lines).
   * CAD_Init() is the open, previously validated bring-up; its state
   * machine pointer slots are never used because no closed core runs. */
  CAD_Init(TR_PORT, &tr_settings, &tr_params, TR_CcWakeUp);

  /* Detection-stage interrupts: Type-C events on CC1/CC2 wake the driver
   * scan (HW_SignalDetachment() arms the same bits on detach). */
  Ports[TR_PORT].husbpd->IMR |= (UCPD_IMR_TYPECEVT1IE | UCPD_IMR_TYPECEVT2IE);

  done = 1u;
  return 0;
}
