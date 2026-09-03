/*
 * pd_tr.h - UCPD transport API for the pdsink UCPD driver (PDEngine port).
 *
 * The pdsink protocol core expects a "TCPC-like" driver (pd::IDriver in
 * pdsink terms).  On this hardware the TCPC is an STM32 UCPD peripheral fed
 * by the proven open ST USB-PD device layer (usbpd_phy.c / usbpd_phy_hw_if.c
 * / usbpd_hw_if_it.c / usbpd_hw.c).  This header is the narrow seam between
 * the hardware-independent driver logic (pd_ucpd_driver.cpp, which is
 * compiled and bench-tested on the host) and the concrete transport:
 *
 *   - target build:    pd_tr_st.c   (binds to the ST device layer + UCPD1)
 *   - host bench:      pd_tr_sim.c  (simulated UCPD + scripted PD source)
 *
 * The driver code never touches STM32 registers directly; everything it
 * needs is in this API.  IRQ-context entry points (pd_drv_on_*) are the
 * driver side; the transport/ISR calls them with RX frames and TX/HR/CC
 * completion events.  They are safe to call from the UCPD interrupt: they
 * only memcpy into a small ring, flip flags, and - for the GoodCRC reply -
 * start a frame transmission, which on UCPD is a set of register writes.
 */
#ifndef PD_TR_H
#define PD_TR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CC levels, same numeric order as pdsink TCPC_CC_LEVEL::Type:
 * NONE=0, RP_0_5=1, RP_1_5=2, RP_3_0=3. */
#define PD_CC_NONE   0
#define PD_CC_RP_0_5 1
#define PD_CC_RP_1_5 2
#define PD_CC_RP_3_0 3

/* SOP types the transport may report / accept for TX. */
#define PD_SOP_SOP        0
#define PD_SOP_SOP1       1
#define PD_SOP_SOP2       2
#define PD_SOP_SOP1_DEBUG 3
#define PD_SOP_SOP2_DEBUG 4
#define PD_SOP_CABLE_RESET 5
#define PD_SOP_HARD_RESET 6

/* Maximum bytes per frame on the wire (message header + payload) that the
 * stack will send/receive.  pdsink chunks extended messages, so a single
 * frame never exceeds PD_CHUNK (28 bytes payload) + the 2-byte message
 * header. */
#define PD_TR_MAX_FRAME   32u
/* Maximum payload bytes that follow the 2-byte message header. */
#define PD_TR_MAX_PAYLOAD (PD_TR_MAX_FRAME - 2u)

/* ------------------------------------------------------------------ */
/* Transport -> driver (called by the transport / IRQ context)        */
/* ------------------------------------------------------------------ */

/*
 * RX frame completed without error.  `data` holds [message header(2) |
 * payload] exactly as received (the UCPD peripheral validates and strips
 * the CRC; SOP markers are consumed by the peripheral).  `len` is the
 * payload byte count (2 + 4*NumDataObjects, or 2 + ext header + chunk for
 * chunked extended messages).  `sop` is one of PD_SOP_*.
 *
 * May be called from IRQ context.  The driver copies the frame, replies
 * with GoodCRC when required and arms a stack wake-up.
 */
void pd_drv_on_rx_frame(const uint8_t *data, uint8_t len, uint8_t sop);

/*
 * The frame transmission the driver started (normal data frame) has
 * finished shifting out: status 0 = sent, 1 = discarded (collision or
 * RX started first), 2 = aborted.  This is NOT a GoodCRC confirmation;
 * the driver only learns that from a matching GoodCRC frame (or its own
 * watch timeout).  IRQ context.
 */
void pd_drv_on_tx_done(int status);

/* A hard reset signalling burst we started has been transmitted.  IRQ. */
void pd_drv_on_hr_done(void);

/* Hard reset signalling received from the partner.  IRQ context: the
 * driver only latches it; processing happens in the main loop. */
void pd_drv_on_hr_rx(void);

/* Type-C (CC line) event: one of the CC comparators changed state.  The
 * driver latches it and the main loop re-scans.  IRQ context. */
void pd_drv_on_cc_event(void);

/* ------------------------------------------------------------------ */
/* Driver -> transport (called from the main loop / driver service)   */
/* ------------------------------------------------------------------ */

/*
 * Send one frame: [message header(2) | payload] as described above.
 * Returns 0 when the frame was armed (the TX-complete callback will
 * follow), -1 when the transport is busy (RX in progress / TX already
 * running) and the caller must retry later.  `sop` must be PD_SOP_SOP
 * for all stack traffic (sink-only port); PD_SOP_HARD_RESET is sent via
 * pd_tr_send_hard_reset() instead.
 */
int pd_tr_send_frame(uint8_t sop, const uint8_t *buf, uint16_t len);

/* Send a hard reset signalling burst (SOP'/SOP'' style).  0 = armed. */
int pd_tr_send_hard_reset(void);

/*
 * Sink collision-avoidance termination (SinkTxOK/SinkTxNG).
 * enable=1 -> SinkTxOK: the state the sink presents while it may start
 * transmitting (internal CC pull configured as the 3.0 A class, exactly
 * like the ST device layer's SetResistor_SinkTxOK); enable=0 -> SinkTxNG
 * (idle, SetResistor_SinkTxNG).  The transport may also take the
 * opportunity to settle the CC analog front-end before the level is read.
 */
void pd_tr_set_sink_tx_ok(int enable);
/*
 * Read the current CC1/CC2 line levels (PD_CC_*).  The sink presents Rd;
 * the levels report the partner's pull-ups.  Both lines can be sampled
 * at any time; the active-CC read (pd_tr_read_active_cc) reflects the
 * line selected with pd_tr_set_active_cc().
 */
void pd_tr_read_cc(int *cc1, int *cc2);

/* Read the level of the active (selected) CC line. */
int pd_tr_read_active_cc(void);

/* Select the CC line used for communication (1 or 2).  Used when the
 * Type-C state machine has decided the polarity. */
void pd_tr_set_active_cc(int cc);

/*
 * Attach/detach the PD communication path (UCPD RX/TX DMA, interrupt
 * enables, RX enable).  Called by the driver when the stack transitions
 * between the detached and attached states.  attach(cc) is 1 or 2.
 */
void pd_tr_attach(int cc);
void pd_tr_detach(void);

/*
 * True when a valid 5 V supply is present.  This board is a CC-only PD
 * tester without a VBUS ADC: the transport reports the CC-detected
 * attach state (a source that pulls CC up supplies VBUS by definition).
 */
int pd_tr_vbus_ok(void);

/* UCPD TX data path busy (a frame is shifting out or RX is in progress). */
int pd_tr_tx_busy(void);

/* Milliseconds since boot (monotonic). */
uint32_t pd_tr_now_ms(void);

/*
 * One-time transport setup.  On the target this configures the UCPD
 * detection stage (SNK role, Rd on both CC lines, Type-C events) and
 * wires the device-layer callbacks into the driver.  Must be called
 * before the pdsink stack starts.
 */
int pd_tr_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PD_TR_H */
