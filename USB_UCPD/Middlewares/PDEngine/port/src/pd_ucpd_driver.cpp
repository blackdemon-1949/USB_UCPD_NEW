/*
 * pd_ucpd_driver.cpp - pdsink IDriver over the STM32 UCPD transport.
 *
 * See pd_ucpd_driver.h for the design notes.  The hardware is a plain
 * "no GoodCRC, no retry" TCPC: pdsink's PRL performs retries
 * (get_hw_features() reports tx_auto_retry=false), while this driver
 * implements the two jobs the UCPD cannot do:
 *
 *  1. Reply GoodCRC to every valid non-GoodCRC message received (armed in
 *     IRQ context, immediately after the frame lands, exactly like the
 *     ST closed-core PRL used to through the same device layer).
 *  2. Resolve TX: SUCCEEDED only on a matching GoodCRC; FAILED on the
 *     watch timeout, discard or abort events.
 *
 * RX frames are queued as payload-only slices: the message header is
 * parsed here and reconstructed into port.rx_chunk.header by
 * fetch_rx_data(), while the ring holds the bytes that follow the header
 * (data objects, or the 2-byte extended header + chunk payload).  This
 * matches what pdsink's RCH expects (prl.cpp reads the extended header
 * from rx_chunk.read16(0) and copies data objects verbatim).
 */
#include "pd_ucpd_driver.h"

#include <string.h>

#include "data_objects.h"
#include "pd_log.h"
#include "prl.h"
#include "task.h"

namespace pdport {

// Watch window for the GoodCRC after a frame is on the wire.  The PD
// spec's tRetry (905 us on this device layer) is the time the *sender*
// waits before repeating; the partner's GoodCRC lands well inside that.
// One tick of slack covers scheduling; a late GoodCRC simply resolves a
// retried transmission (same message ID), which the PRL tolerates.
static constexpr uint32_t TX_WATCH_MS = 3;

// Millisecond tick provider for pdsink timers.
uint32_t UcpdDriver::now_ms_c() { return pd_tr_now_ms(); }

UcpdDriver::UcpdDriver(pd::Port& port) : port_(port) {
    active_ = this;
}

UcpdDriver::~UcpdDriver() {
    if (active_ == this) { active_ = nullptr; }
}

auto UcpdDriver::get_time_func() const -> pd::ITimer::TimeFunc {
    return &UcpdDriver::now_ms_c;
}

void UcpdDriver::rearm(uint32_t /*interval*/) {
    // Unsupported: the stack is polled with a fixed 1 ms tick delivered by
    // service() (task.cpp: "This is NOT needed for periodic 1ms timer
    // without rearm support").
}

bool UcpdDriver::is_rearm_supported() { return false; }

void UcpdDriver::setup() {
    // The transport was initialised before the stack started (UCPD
    // detection stage + IRQ callbacks wired into pd_drv_on_*).
    drop_tx_state();
}

// ---------------------------------------------------------------------
// CC scanning / polarity (used by the pdsink Type-C state machine)
// ---------------------------------------------------------------------

void UcpdDriver::req_scan_cc() {
    int cc1 = PD_CC_NONE;
    int cc2 = PD_CC_NONE;
    pd_tr_read_cc(&cc1, &cc2);
    scan_cc1_ = static_cast<pd::TCPC_CC_LEVEL::Type>(cc1);
    scan_cc2_ = static_cast<pd::TCPC_CC_LEVEL::Type>(cc2);
    cc_scan_done_ = true;
}

bool UcpdDriver::try_scan_cc_result(pd::TCPC_CC_LEVEL::Type& cc1,
                                    pd::TCPC_CC_LEVEL::Type& cc2) {
    if (!cc_scan_done_) { return false; }
    cc_scan_done_ = false;
    cc1 = scan_cc1_;
    cc2 = scan_cc2_;
    return true;
}

void UcpdDriver::req_active_cc() {
    // The PRL needs the SinkTxOK level before it sends the first message
    // of an AMS.  Mirror the ST device-layer sequence used by the proven
    // closed-core PRL: present SinkTxOK (internal Rp 3.0 A class) first,
    // then sample the active CC comparator.  pdsink's
    // TCPC_CC_LEVEL::SinkTxOK maps to the vRd-3.0 band value 3, which is
    // exactly what the ST IsResistor_SinkTxOk path reads back.
    raise_tx_ok();
    active_cc_ = static_cast<pd::TCPC_CC_LEVEL::Type>(pd_tr_read_active_cc());
    active_cc_done_ = true;
}

bool UcpdDriver::try_active_cc_result(pd::TCPC_CC_LEVEL::Type& cc) {
    if (!active_cc_done_) { return false; }
    active_cc_done_ = false;
    cc = active_cc_;
    return true;
}

bool UcpdDriver::is_vbus_ok() {
    // This bench is a CC-only PD tester without a VBUS ADC: a source that
    // presents Rp on CC supplies VBUS by definition, so VBUS presence is
    // derived from the CC levels.
    int cc1 = PD_CC_NONE;
    int cc2 = PD_CC_NONE;
    pd_tr_read_cc(&cc1, &cc2);
    return (cc1 != PD_CC_NONE) || (cc2 != PD_CC_NONE);
}

void UcpdDriver::req_set_polarity(pd::TCPC_POLARITY active_cc) {
    if (active_cc == pd::TCPC_POLARITY::NONE) {
        // Detach (also the boot state).
        if (phy_state_ == PhyState::ATTACHED) {
            pd_tr_detach();
            phy_state_ = PhyState::DETACHED;
            drop_tx_state();
            rx_ring_tail_ = rx_ring_head_;
        }
        polarity_done_ = true;
        return;
    }

    const int cc = (active_cc == pd::TCPC_POLARITY::CC1) ? 1 : 2;

    if (phy_state_ == PhyState::DETACHED) {
        pd_tr_set_active_cc(cc);
        pd_tr_attach(cc);
        phy_state_ = PhyState::ATTACHED;
        drop_tx_state();
        // Flush frames received before the stack was ready, then go idle.
        rx_ring_tail_ = rx_ring_head_;
        goodcrc_pending_ = false;
        pending_gc_raw_ = 0;
        rx_enable_value_ = true;
        rx_enable_pending_ = false;
        lower_tx_ok(); // SinkTxNG (idle) until the PRL raises it
    }
    // Same-polarity re-request or already attached: nothing to do (the
    // UCPD is configured for the active line).
    polarity_done_ = true;
}

bool UcpdDriver::is_set_polarity_done() { return polarity_done_; }

// ---------------------------------------------------------------------
// RX enable / BIST / hard reset
// ---------------------------------------------------------------------

void UcpdDriver::req_rx_enable(bool enable) {
    rx_enable_value_ = enable;
    rx_enable_pending_ = true;
}

bool UcpdDriver::is_rx_enable_done() { return !rx_enable_pending_; }

void UcpdDriver::req_set_bist(pd::TCPC_BIST_MODE /*mode*/) {
    // BIST Carrier/TestData is a source-side conformance feature; a sink
    // only uses BIST to leave the mode.  Treat requests as completed.
    bist_done_ = true;
}

bool UcpdDriver::is_set_bist_done() { return bist_done_; }

void UcpdDriver::req_hr_send() {
    hr_done_ = false;
    if (pd_tr_send_hard_reset() == 0) {
        counters_.hr_sent++;
        port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::SENDING);
    } else {
        hr_done_ = true;
        port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::FAILED);
    }
}

bool UcpdDriver::is_hr_send_done() { return hr_done_; }

// ---------------------------------------------------------------------
// RX path (IRQ context up to the ring push)
// ---------------------------------------------------------------------

void UcpdDriver::arm_goodcrc(uint16_t hdr_raw) {
    // Wire is free (caller checked): send the 2-byte GoodCRC frame.  If
    // the transport refuses (race), the reply is deferred to service().
    if (tx_origin_ != TxOrigin::NONE) {
        goodcrc_pending_ = true;
        pending_gc_raw_ = hdr_raw;
        counters_.goodcrc_deferred++;
        return;
    }
    tx_origin_ = TxOrigin::GOODCRC;
    const uint8_t buf[2] = {(uint8_t)(hdr_raw & 0xFFu),
                            (uint8_t)((hdr_raw >> 8) & 0xFFu)};
    if (pd_tr_send_frame(PD_SOP_SOP, buf, 2) == 0) {
        counters_.goodcrc_sent++;
    } else {
        tx_origin_ = TxOrigin::NONE;
        goodcrc_pending_ = true;
        pending_gc_raw_ = hdr_raw;
        counters_.goodcrc_deferred++;
    }
}

void UcpdDriver::on_rx_frame(const uint8_t* data, uint8_t len, uint8_t sop) {
    if (data == nullptr || len < 2u || len > PD_TR_MAX_FRAME) {
        return; // malformed frame: ignore (CRC was already validated)
    }

    pd::PD_HEADER hdr;
    hdr.raw_value = static_cast<uint16_t>(data[0] | (uint16_t)(data[1] << 8));

    const bool is_goodcrc =
        (hdr.extended == 0) && (hdr.data_obj_count == 0) &&
        (hdr.message_type == static_cast<uint16_t>(pd::PD_CTRL_MSGT::GoodCRC));

    if (is_goodcrc) {
        counters_.rx_goodcrc++;
        // Resolve an outstanding data transmission: the GoodCRC carries
        // the message ID of the frame it acknowledges.
        if (tx_origin_ == TxOrigin::DATA &&
            port_.tcpc_tx_status.load() == pd::TCPC_TRANSMIT_STATUS::SENDING &&
            hdr.message_id == tx_msg_id_) {
            tx_finish_ok();
        }
        wake_needed_ = true;
        return;
    }

    // Only SOP traffic reaches the stack (the sink talks to the port
    // partner; SOP'/SOP''/debug traffic is not for us).
    if (sop != PD_SOP_SOP) {
        return;
    }

    // Payload after the message header.  Data messages: 4 bytes per data
    // object.  Extended messages (incl. chunked extended): the rest of the
    // frame - the 2-byte extended header plus this chunk's payload.
    const uint16_t payload_len =
        (hdr.extended != 0) ? (uint16_t)(len - 2u)
                            : (uint16_t)hdr.data_obj_count * 4u;
    if (payload_len > (uint16_t)(len - 2u)) {
        return; // short frame: ignore
    }
    if (payload_len > PD_TR_MAX_PAYLOAD) {
        return; // oversized payload: not addressable by the stack
    }

    // Build the GoodCRC reply (echoes message ID / spec revision).
    pd::PD_HEADER gc;
    gc.raw_value = 0;
    gc.message_type = static_cast<uint16_t>(pd::PD_CTRL_MSGT::GoodCRC);
    gc.spec_revision = hdr.spec_revision;
    gc.message_id = hdr.message_id;

    if (!rx_enable_value_) {
        // Stack RX disabled (hard-reset window): acknowledge on the wire
        // but do not deliver the frame to the stack.
        if (!pd_tr_tx_busy()) {
            arm_goodcrc(gc.raw_value);
        }
        return;
    }

    // Ring push (single producer = this IRQ-context function).
    const uint8_t next = (uint8_t)((rx_ring_head_ + 1u) % RX_RING_SIZE);
    if (next == rx_ring_tail_) {
        // Ring full: drop the oldest frame (the partner retries).
        rx_ring_tail_ = (uint8_t)((rx_ring_tail_ + 1u) % RX_RING_SIZE);
    }
    RxSlot& slot = rx_ring_[rx_ring_head_];
    slot.hdr_raw = hdr.raw_value;
    slot.len = (uint8_t)payload_len;
    slot.sop = sop;
    if (payload_len > 0u) {
        memcpy(slot.data, &data[2], payload_len);
    }
    rx_ring_head_ = next;

    counters_.rx_frames++;
    wake_needed_ = true;

    if (!pd_tr_tx_busy()) {
        arm_goodcrc(gc.raw_value);
    } else {
        goodcrc_pending_ = true;
        pending_gc_raw_ = gc.raw_value;
        counters_.goodcrc_deferred++;
    }
}

void UcpdDriver::on_tx_done(int status) {
    // Transport-level completion of the frame we armed.
    if (tx_origin_ == TxOrigin::DATA) {
        if (port_.tcpc_tx_status.load() == pd::TCPC_TRANSMIT_STATUS::SENDING &&
            status != 0) {
            // Discard/abort: the frame never reached the partner intact.
            tx_finish_fail();
            counters_.tx_discard++;
        }
        // status == 0 (sent): still waiting for the GoodCRC.
    } else if (tx_origin_ == TxOrigin::GOODCRC) {
        tx_origin_ = TxOrigin::NONE;
        if (status != 0) {
            // The GoodCRC never went out; re-arm from the main loop.
            goodcrc_pending_ = true;
        }
    }
    wake_needed_ = true;
}

void UcpdDriver::on_hr_done() {
    // Hard reset burst transmitted: resolve like a successful transfer.
    hr_done_ = true;
    if (port_.tcpc_tx_status.load() == pd::TCPC_TRANSMIT_STATUS::SENDING) {
        port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::SUCCEEDED);
    }
    tx_origin_ = TxOrigin::NONE;
    wake_needed_ = true;
}

void UcpdDriver::on_hr_rx() {
    // Latched; processed in service() (main loop) to keep the pdsink
    // notification path out of IRQ context.
    ev_hr_rx_ = true;
    wake_needed_ = true;
}

void UcpdDriver::on_cc_event() {
    ev_cc_ = true;
    counters_.cc_events++;
    wake_needed_ = true;
}

bool UcpdDriver::fetch_rx_data() {
    if (rx_ring_tail_ == rx_ring_head_) {
        return false;
    }

    const RxSlot& slot = rx_ring_[rx_ring_tail_];
    pd::PD_CHUNK& chunk = port_.rx_chunk;
    chunk.clear();
    chunk.header.raw_value = slot.hdr_raw;
    const uint16_t cap = static_cast<uint16_t>(chunk.get_data().capacity());
    const uint16_t copy_n = (slot.len < cap) ? slot.len : cap;
    // Resize first: ETL vector resize() value-initialises the grown
    // region, which would wipe bytes copied before the resize.
    chunk.get_data().resize(copy_n);
    if (copy_n > 0u) {
        memcpy(chunk.get_data().data(), slot.data, copy_n);
    }

    rx_ring_tail_ = (uint8_t)((rx_ring_tail_ + 1u) % RX_RING_SIZE);
    return true;
}

// ---------------------------------------------------------------------
// TX path
// ---------------------------------------------------------------------

void UcpdDriver::req_transmit() {
    // Called by the PRL right after it filled port.tx_chunk.  Snapshot the
    // frame so it survives any PRL buffer reuse during the async send.
    const pd::PD_CHUNK& chunk = port_.tx_chunk;

    tx_frame_len_ = 0;
    tx_frame_[tx_frame_len_++] = (uint8_t)(chunk.header.raw_value & 0xFFu);
    tx_frame_[tx_frame_len_++] =
        (uint8_t)((chunk.header.raw_value >> 8) & 0xFFu);
    const uint16_t data_n = static_cast<uint16_t>(chunk.get_data().size());
    const uint16_t cap = PD_TR_MAX_PAYLOAD;
    const uint16_t n = (data_n < cap) ? data_n : cap;
    if (n > 0u) {
        memcpy(&tx_frame_[2], chunk.get_data().data(), n);
        tx_frame_len_ = (uint8_t)(2u + n);
    }

    tx_msg_id_ = chunk.header.message_id;
    port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::ENQUEUED);
    wake_needed_ = true;
}

void UcpdDriver::raise_tx_ok() {
    if (!sink_tx_ok_) {
        pd_tr_set_sink_tx_ok(1);
        sink_tx_ok_ = true;
    }
    sink_ok_since_ms_ = pd_tr_now_ms();
}

void UcpdDriver::lower_tx_ok() {
    if (sink_tx_ok_) {
        pd_tr_set_sink_tx_ok(0);
        sink_tx_ok_ = false;
    }
}

void UcpdDriver::tx_finish_ok() {
    // GoodCRC with a matching message ID received (or HR burst sent).
    port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::SUCCEEDED);
    counters_.tx_succeeded++;
    if (tx_origin_ == TxOrigin::DATA) {
        counters_.rx_goodcrc_ok++;
    }
    tx_origin_ = TxOrigin::NONE;
    lower_tx_ok(); // back to the idle termination
}

void UcpdDriver::tx_finish_fail() {
    // No GoodCRC in time, or the frame was discarded/aborted: report
    // FAILED, the PRL retries (tx_auto_retry == false).
    port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::FAILED);
    counters_.tx_failed++;
    tx_origin_ = TxOrigin::NONE;
    lower_tx_ok();
}

void UcpdDriver::drop_tx_state() {
    port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::UNSET);
    tx_origin_ = TxOrigin::NONE;
    goodcrc_pending_ = false;
    pending_gc_raw_ = 0;
    lower_tx_ok();
}

// ---------------------------------------------------------------------
// Main-loop service
// ---------------------------------------------------------------------

void UcpdDriver::tick_timer_event() {
    const uint32_t now = pd_tr_now_ms();
    if (first_service_) {
        first_service_ = false;
        last_timer_ms_ = now;
    }
    if ((int32_t)(now - last_timer_ms_) >= 1) {
        last_timer_ms_ = now;
        // The stack expects a 1 ms periodic tick (task.cpp updates the
        // timers and re-runs the FSMs when the timer event arrives).
        port_.notify_task(pd::MsgTask_Timer{});
    }
}

bool UcpdDriver::service() {
    bool event = false;
    if (wake_needed_) {
        wake_needed_ = false;
        event = true;
    }

    // 1. Periodic 1 ms tick for the pdsink timers / FSM pump.
    tick_timer_event();

    // 2. RX enable requests are synchronous on this transport.
    if (rx_enable_pending_) {
        rx_enable_pending_ = false;
        if (!rx_enable_value_) {
            // Around hard resets the stack silences the RX path: drop
            // queued frames so stale traffic cannot reach the PRL after
            // the reset.
            rx_ring_tail_ = rx_ring_head_;
            goodcrc_pending_ = false;
            pending_gc_raw_ = 0;
        }
        event = true;
    }

    // 3. Latched IRQ events.
    if (ev_hr_rx_) {
        ev_hr_rx_ = false;
        // Hard reset from the partner: flush the RX path and forward to
        // the PRL exactly like a TCPC would (see the reference driver).
        drop_tx_state();
        rx_ring_tail_ = rx_ring_head_;
        goodcrc_pending_ = false;
        pending_gc_raw_ = 0;
        counters_.hr_rx++;
        port_.notify_prl(pd::MsgToPrl_TcpcHardReset{});
        event = true;
    }

    if (ev_cc_) {
        ev_cc_ = false;
        // A CC change may mean attach/detach; the TC re-scans on its own
        // schedule, the wake-up simply makes the stack poll sooner.
        event = true;
    }

    // 4. Deferred GoodCRC replies (the wire was busy when the frame
    //    arrived, or the previous attempt was discarded/aborted).
    if (goodcrc_pending_) {
        if (port_.tcpc_tx_status.load() != pd::TCPC_TRANSMIT_STATUS::SENDING &&
            !pd_tr_tx_busy()) {
            const uint16_t raw = pending_gc_raw_;
            goodcrc_pending_ = false;
            arm_goodcrc(raw);
            event = true;
        }
    }

    // 5. Data transmission: watch for timeout, arm queued frames.
    const auto st = port_.tcpc_tx_status.load();
    if (st == pd::TCPC_TRANSMIT_STATUS::SENDING &&
        tx_origin_ == TxOrigin::DATA) {
        if ((int32_t)(pd_tr_now_ms() - tx_deadline_ms_) >= 0) {
            tx_finish_fail();
            event = true;
        }
    } else if (st == pd::TCPC_TRANSMIT_STATUS::ENQUEUED &&
               !pd_tr_tx_busy()) {
        tx_origin_ = TxOrigin::DATA;
        raise_tx_ok(); // present SinkTxOK while the data frame goes out
        if (pd_tr_send_frame(PD_SOP_SOP, tx_frame_, tx_frame_len_) == 0) {
            port_.tcpc_tx_status.store(pd::TCPC_TRANSMIT_STATUS::SENDING);
            tx_deadline_ms_ = pd_tr_now_ms() + TX_WATCH_MS;
            counters_.tx_frames++;
        } else {
            // Busy after all (partner started first): keep ENQUEUED and
            // try again next round.
            tx_origin_ = TxOrigin::NONE;
            lower_tx_ok();
        }
        event = true;
    }

    // 6. Idle safety: if SinkTxOK was raised for an AMS gate but the PRL
    //    gave up without a transmission, drop back to SinkTxNG.
    if (sink_tx_ok_ && tx_origin_ == TxOrigin::NONE &&
        (int32_t)(pd_tr_now_ms() - sink_ok_since_ms_) >= 10) {
        lower_tx_ok();
    }

    return event;
}

auto UcpdDriver::get_hw_features() -> pd::TCPC_HW_FEATURES {
    // The UCPD has no GoodCRC/retry hardware: the PRL must do retries and
    // this driver handles GoodCRC explicitly.
    pd::TCPC_HW_FEATURES f;
    f.rx_auto_goodcrc_send = false;
    f.tx_auto_goodcrc_check = false;
    f.tx_auto_retry = false;
    return f;
}

// ---------------------------------------------------------------------
// C bridge: entry points declared in pd_tr.h, invoked by the transport
// (UCPD IRQ path on the target, sim transport on the host).  All of them
// only latch/copy state, so they are safe from IRQ context.
// ---------------------------------------------------------------------

} // namespace pdport

pdport::UcpdDriver* pdport::UcpdDriver::active_ = nullptr;

extern "C" {

void pd_drv_on_rx_frame(const uint8_t* data, uint8_t len, uint8_t sop) {
    if (pdport::UcpdDriver* d = pdport::UcpdDriver::active()) {
        d->on_rx_frame(data, len, sop);
    }
}

void pd_drv_on_tx_done(int status) {
    if (pdport::UcpdDriver* d = pdport::UcpdDriver::active()) {
        d->on_tx_done(status);
    }
}

void pd_drv_on_hr_done(void) {
    if (pdport::UcpdDriver* d = pdport::UcpdDriver::active()) {
        d->on_hr_done();
    }
}

void pd_drv_on_hr_rx(void) {
    if (pdport::UcpdDriver* d = pdport::UcpdDriver::active()) {
        d->on_hr_rx();
    }
}

void pd_drv_on_cc_event(void) {
    if (pdport::UcpdDriver* d = pdport::UcpdDriver::active()) {
        d->on_cc_event();
    }
}

} // extern "C"
