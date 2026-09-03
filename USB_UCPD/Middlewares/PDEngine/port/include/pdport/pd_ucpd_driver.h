/*
 * pd_ucpd_driver.h - pdsink IDriver over the STM32 UCPD transport.
 *
 * M2 milestone: a self-written pdsink driver (pd::IDriver = ITCPC + ITimer)
 * over the proven open ST USB-PD device layer.  pdsink's PRL is told
 * through get_hw_features() that the hardware does NOT auto-send GoodCRC,
 * does not watch GoodCRC and does not retry, so the PRL owns retries while
 * this driver owns:
 *
 *   - the GoodCRC reply to every received non-GoodCRC message (the UCPD
 *     has no hardware GoodCRC; the reply is armed from the RX interrupt
 *     and must reach the partner within tGoodCRCAck),
 *   - the TX outcome: a transmission only SUCCEEDs when a GoodCRC with the
 *     matching message ID comes back; otherwise the driver's watch timeout
 *     reports FAILED and the PRL retries,
 *   - RX frame storage (SPSC ring filled by the IRQ path) and CC
 *     scanning / polarity hand-over to the UCPD transport.
 *
 * The driver is compiled for the host bench (with a simulated transport)
 * and for the target (pd_tr_st.c binding); no register access lives here.
 *
 * Concurrency model:
 *   - IRQ context: on_rx_frame() (copy into ring + arm GoodCRC reply),
 *     on_hr_rx()/on_cc_event() (latch flags only), on_tx_done()/on_hr_done()
 *     (update TX outcome state).
 *   - Main loop: service() runs the async jobs (deferred GoodCRC, TX watch
 *     timeout, queued transmission arming) and forwards latched events into
 *     the pdsink message bus as wake-ups/timer ticks, exactly like the
 *     FUSB302 driver's task does.
 */
#ifndef PD_UCPD_DRIVER_H
#define PD_UCPD_DRIVER_H

#include <stdint.h>

#include "idriver.h"
#include "pd_tr.h"
#include "port.h"

namespace pdport {

class UcpdDriver : public pd::IDriver {
public:
    explicit UcpdDriver(pd::Port& port);
    ~UcpdDriver();

    // pd::IDriver
    void setup() override;

    // pd::ITimer
    pd::ITimer::TimeFunc get_time_func() const override;
    void rearm(uint32_t interval) override;   // unsupported (1 ms tick)
    bool is_rearm_supported() override;

    // pd::ITCPC
    void req_scan_cc() override;
    bool try_scan_cc_result(pd::TCPC_CC_LEVEL::Type& cc1,
                            pd::TCPC_CC_LEVEL::Type& cc2) override;
    void req_active_cc() override;
    bool try_active_cc_result(pd::TCPC_CC_LEVEL::Type& cc) override;
    bool is_vbus_ok() override;
    void req_set_polarity(pd::TCPC_POLARITY active_cc) override;
    bool is_set_polarity_done() override;
    void req_rx_enable(bool enable) override;
    bool is_rx_enable_done() override;
    bool fetch_rx_data() override;
    void req_transmit() override;
    void req_set_bist(pd::TCPC_BIST_MODE mode) override;
    bool is_set_bist_done() override;
    void req_hr_send() override;
    bool is_hr_send_done() override;
    auto get_hw_features() -> pd::TCPC_HW_FEATURES override;

    // IRQ-context entry points (bridged from pd_tr.h by the transport)
    void on_rx_frame(const uint8_t* data, uint8_t len, uint8_t sop);
    void on_tx_done(int status);
    void on_hr_done(void);
    void on_hr_rx(void);
    void on_cc_event(void);

    /*
     * Main-loop service: run regularly (every loop iteration).  Emits the
     * pdsink 1 ms timer tick, executes pending asynchronous jobs, arms
     * queued transmissions, watches TX timeouts and forwards latched IRQ
     * events into the stack.  Returns true when something was processed.
     */
    bool service();

    // Diagnostics counters (CLI 'pd diag' equivalents).
    struct Counters {
        uint32_t rx_frames;        // non-GoodCRC frames queued for the stack
        uint32_t rx_goodcrc;       // GoodCRC frames received
        uint32_t rx_goodcrc_ok;    // ...that matched an outstanding TX
        uint32_t tx_frames;        // data frames armed on the wire
        uint32_t tx_succeeded;     // GoodCRC-confirmed transmissions
        uint32_t tx_failed;        // watch timeouts (PRL retries)
        uint32_t tx_discard;       // transport-level discard/abort events
        uint32_t goodcrc_sent;     // GoodCRC replies transmitted
        uint32_t goodcrc_deferred; // replies deferred (TX busy at RX time)
        uint32_t hr_sent;
        uint32_t hr_rx;
        uint32_t cc_events;
    };
    const Counters& counters() const { return counters_; }

    // Single-port wiring: the C bridge functions in pd_tr.h dispatch to the
    // driver registered here (set by the port initialisation code).
    static void set_active(UcpdDriver* d) { active_ = d; }
    static UcpdDriver* active() { return active_; }

private:
    enum class TxOrigin { NONE, DATA, GOODCRC };
    enum class PhyState { DETACHED, ATTACHED };

    static uint32_t now_ms_c();

    void drop_tx_state();
    void arm_goodcrc(uint16_t hdr_raw);
    void tx_finish_ok();               // partner GoodCRC matched
    void tx_finish_fail();             // watch timeout / transport error
    void tick_timer_event();
    void raise_tx_ok();                // present SinkTxOK (3.0 A class)
    void lower_tx_ok();                // back to SinkTxNG (idle)

    // ---- state ----
    pd::Port& port_;

    // RX ring: SPSC, producer = UCPD IRQ, consumer = main loop.
    struct RxSlot {
        uint16_t hdr_raw;               // received message header (LE)
        uint8_t data[PD_TR_MAX_PAYLOAD]; // payload after the 2-byte header
        uint8_t len;                    // payload bytes
        uint8_t sop;                    // PD_SOP_* of the frame
    };
    static constexpr uint8_t RX_RING_SIZE = 4;
    RxSlot rx_ring_[RX_RING_SIZE];
    volatile uint8_t rx_ring_head_ = 0; // written by IRQ
    uint8_t rx_ring_tail_ = 0;          // written by main loop

    // CC / polarity
    bool cc_scan_done_ = false;
    pd::TCPC_CC_LEVEL::Type scan_cc1_ = pd::TCPC_CC_LEVEL::NONE;
    pd::TCPC_CC_LEVEL::Type scan_cc2_ = pd::TCPC_CC_LEVEL::NONE;
    bool active_cc_done_ = false;
    pd::TCPC_CC_LEVEL::Type active_cc_ = pd::TCPC_CC_LEVEL::NONE;
    bool polarity_done_ = true;
    bool rx_enable_pending_ = false;
    bool rx_enable_value_ = true;

    // TX path
    uint8_t tx_frame_[PD_TR_MAX_FRAME];
    uint8_t tx_frame_len_ = 0;
    TxOrigin tx_origin_ = TxOrigin::NONE;
    uint32_t tx_deadline_ms_ = 0;
    uint8_t tx_msg_id_ = 0;

    // Latched IRQ events, consumed by service()
    volatile bool ev_hr_rx_ = false;
    volatile bool ev_cc_ = false;
    volatile bool goodcrc_pending_ = false; // GoodCRC reply owed (TX busy)
    uint16_t pending_gc_raw_ = 0;

    // BIST / HR
    bool bist_done_ = true;
    bool hr_done_ = true;

    PhyState phy_state_ = PhyState::DETACHED;
    bool sink_tx_ok_ = false;         // SinkTxOK currently presented
    uint32_t sink_ok_since_ms_ = 0;
    uint32_t last_timer_ms_ = 0;      // 1 ms pdsink tick tracking
    bool first_service_ = true;
    Counters counters_{};
    bool wake_needed_ = false;

    static UcpdDriver* active_;
};

} // namespace pdport

#endif /* PD_UCPD_DRIVER_H */
