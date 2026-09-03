/*
 * test_pdport_driver.cpp - host tests for the UCPD driver (M2).
 *
 * Exercises pdport::UcpdDriver against the simulated transport
 * (pd_tr_sim.cpp): RX queueing + automatic GoodCRC replies, deferred
 * GoodCRC, the TX lifecycle (ENQUEUED -> SENDING -> SUCCEEDED/FAILED),
 * hard-reset handling, CC scanning / polarity and the 1 ms stack tick.
 */
#include <gtest/gtest.h>

#include <string.h>

#include <vector>

#include "data_objects.h"
#include "messages.h"
#include "pd_tr_sim.hpp"
#include "port.h"
#include "prl.h"

#include "pd_ucpd_driver.h"

namespace {

using pdport::UcpdDriver;

// --- helpers ----------------------------------------------------------

// Build one wire frame: [hdr(2) | payload].
std::vector<uint8_t> make_frame(uint16_t hdr, const uint8_t* payload,
                                size_t payload_len) {
    std::vector<uint8_t> f;
    f.push_back((uint8_t)(hdr & 0xFFu));
    f.push_back((uint8_t)((hdr >> 8) & 0xFFu));
    if (payload != nullptr && payload_len > 0) {
        f.insert(f.end(), payload, payload + payload_len);
    }
    return f;
}

std::vector<uint8_t> make_frame(uint16_t hdr) { return make_frame(hdr, nullptr, 0); }

// Spy router for PRL-directed messages.
class PrlSpy
    : public etl::message_router<PrlSpy, pd::MsgToPrl_TcpcHardReset> {
public:
    PrlSpy() : etl::message_router<PrlSpy, pd::MsgToPrl_TcpcHardReset>(1) {}
    int hr_count = 0;
    void on_receive(const pd::MsgToPrl_TcpcHardReset&) { hr_count++; }
    void on_receive_unknown(const etl::imessage&) {}
};

// Parse a captured TX frame back into (hdr, payload).
struct ParsedTx {
    uint16_t hdr;
    std::vector<uint8_t> payload;
};
ParsedTx parse_tx(const pdport_test::SimState::TxEvent& ev) {
    ParsedTx p;
    p.hdr = (uint16_t)(ev.data[0] | (uint16_t)(ev.data[1] << 8));
    p.payload.assign(ev.data + 2, ev.data + ev.len);
    return p;
}

// Attach helper: simulate a 3 A-class source on CC1 and let the driver
// attach.
void attach_cc1(UcpdDriver& drv) {
    pdport_test::sim_set_cc(PD_CC_RP_3_0, PD_CC_NONE);
    drv.req_scan_cc();
    pd::TCPC_CC_LEVEL::Type cc1, cc2;
    ASSERT_TRUE(drv.try_scan_cc_result(cc1, cc2));
    ASSERT_EQ(cc1, pd::TCPC_CC_LEVEL::RP_3_0);
    ASSERT_EQ(cc2, pd::TCPC_CC_LEVEL::NONE);
    drv.req_set_polarity(pd::TCPC_POLARITY::CC1);
    ASSERT_TRUE(drv.is_set_polarity_done());
    ASSERT_TRUE(pdport_test::sim().attached);
}

} // namespace

// ---------------------------------------------------------------------
// CC scan / polarity / VBUS
// ---------------------------------------------------------------------

TEST(PdportDriver, ScanCcAndPolarity) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);

    // No source: both lines open, no VBUS.
    pdport_test::sim_set_cc(PD_CC_NONE, PD_CC_NONE);
    ASSERT_FALSE(drv.is_vbus_ok());
    drv.req_scan_cc();
    pd::TCPC_CC_LEVEL::Type cc1, cc2;
    ASSERT_TRUE(drv.try_scan_cc_result(cc1, cc2));
    EXPECT_EQ(cc1, pd::TCPC_CC_LEVEL::NONE);
    EXPECT_EQ(cc2, pd::TCPC_CC_LEVEL::NONE);

    // 1.5 A class source on CC2.
    pdport_test::sim_set_cc(PD_CC_NONE,
                            PD_CC_RP_1_5);
    EXPECT_TRUE(drv.is_vbus_ok());
    drv.req_scan_cc();
    ASSERT_TRUE(drv.try_scan_cc_result(cc1, cc2));
    EXPECT_EQ(cc1, pd::TCPC_CC_LEVEL::NONE);
    EXPECT_EQ(cc2, pd::TCPC_CC_LEVEL::RP_1_5);

    // Attach on CC2: transport selects the line, goes attached, sink idle
    // termination (SinkTxNG).
    drv.req_set_polarity(pd::TCPC_POLARITY::CC2);
    EXPECT_TRUE(drv.is_set_polarity_done());
    EXPECT_TRUE(pdport_test::sim().attached);
    EXPECT_EQ(pdport_test::sim().attach_line, 2);
    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 0);

    // Detach back to nothing: the line opens, VBUS disappears.
    pdport_test::sim_set_cc(PD_CC_NONE, PD_CC_NONE);
    drv.req_set_polarity(pd::TCPC_POLARITY::NONE);
    EXPECT_TRUE(drv.is_set_polarity_done());
    EXPECT_FALSE(pdport_test::sim().attached);
    EXPECT_FALSE(drv.is_vbus_ok());
}

TEST(PdportDriver, ActiveCcLevelAndSinkTxOk) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 0);
    drv.req_active_cc();
    pd::TCPC_CC_LEVEL::Type lvl;
    ASSERT_TRUE(drv.try_active_cc_result(lvl));
    EXPECT_EQ(lvl, pd::TCPC_CC_LEVEL::RP_3_0); // SinkTxOK level
    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 1);

    // Idle safety: after 10 ms without a transmission the termination
    // drops back to SinkTxNG.
    pdport_test::sim_advance_ms(11);
    drv.service();
    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 0);
}

// ---------------------------------------------------------------------
// RX + automatic GoodCRC
// ---------------------------------------------------------------------

TEST(PdportDriver, RxQueueAndAutoGoodCrc) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    // Source_Capabilities-like data message: 3 data objects.
    const uint16_t hdr = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_DATA_MSGT::Source_Capabilities), 2,
        pd::PD_REVISION::REV30, 3, 0);
    const uint8_t pdo[12] = {0x01, 0x2c, 0x00, 0x00, 0x02, 0x2c,
                             0x00, 0x00, 0x03, 0x2c, 0x00, 0x00};
    std::vector<uint8_t> f = make_frame(hdr, pdo, sizeof(pdo));

    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, f.data(),
                                          (uint16_t)f.size()));

    // The frame is queued and the GoodCRC reply is already on the wire.
    EXPECT_EQ(drv.counters().rx_frames, 1u);
    EXPECT_EQ(drv.counters().goodcrc_sent, 1u);
    ASSERT_FALSE(pdport_test::sim().tx_events.empty());
    const ParsedTx gc = parse_tx(pdport_test::sim().tx_events.back());
    EXPECT_EQ(gc.payload.size(), 0u);
    EXPECT_EQ(gc.hdr & 0x1Fu, static_cast<uint16_t>(pd::PD_CTRL_MSGT::GoodCRC));
    EXPECT_EQ((gc.hdr >> 9) & 0x7u, 2u);      // message ID echoed
    EXPECT_EQ((gc.hdr >> 6) & 0x3u, pd::PD_REVISION::REV30); // rev echoed

    // Drain: the message must be delivered to the PRL buffer.
    ASSERT_TRUE(drv.fetch_rx_data());
    EXPECT_EQ(port.rx_chunk.header.raw_value, hdr);
    ASSERT_EQ(port.rx_chunk.get_data().size(), sizeof(pdo));
    EXPECT_EQ(memcmp(port.rx_chunk.get_data().data(), pdo, sizeof(pdo)), 0);
    EXPECT_FALSE(drv.fetch_rx_data()); // nothing left

    // Complete the GoodCRC TX on the wire.
    ASSERT_TRUE(pdport_test::sim_tx_complete(0));
}

TEST(PdportDriver, RxDeferredGoodCrcWhileBusy) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    // Partner frame lands while our TX occupies the wire.
    pdport_test::sim().tx_busy = true;
    const uint16_t hdr = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 0,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> f = make_frame(hdr);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, f.data(),
                                          (uint16_t)f.size()));
    EXPECT_EQ(drv.counters().goodcrc_deferred, 1u);
    EXPECT_EQ(drv.counters().goodcrc_sent, 0u);

    // Wire frees up: next service round sends the deferred GoodCRC.
    pdport_test::sim().tx_busy = false;
    drv.service();
    ASSERT_GE(pdport_test::sim().tx_events.size(), 1u);
    const ParsedTx gc = parse_tx(pdport_test::sim().tx_events.back());
    EXPECT_EQ(gc.hdr & 0x1Fu, static_cast<uint16_t>(pd::PD_CTRL_MSGT::GoodCRC));
    EXPECT_EQ((gc.hdr >> 9) & 0x7u, 0u);
    ASSERT_TRUE(pdport_test::sim_tx_complete(0));
}

TEST(PdportDriver, RxDropsNonSopAndMalformed) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    // SOP' traffic is not for the sink: dropped, no GoodCRC, no queue.
    const uint16_t hdr = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 0,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> f = make_frame(hdr);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP1, f.data(),
                                          (uint16_t)f.size()));
    EXPECT_EQ(drv.counters().rx_frames, 0u);
    EXPECT_EQ(drv.counters().goodcrc_sent, 0u);
    EXPECT_FALSE(drv.fetch_rx_data());

    // GoodCRC frames are consumed by the driver, never queued.
    const uint16_t gch = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::GoodCRC), 1,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> gf = make_frame(gch);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, gf.data(),
                                          (uint16_t)gf.size()));
    EXPECT_EQ(drv.counters().rx_goodcrc, 1u);
    EXPECT_EQ(drv.counters().rx_frames, 0u);
    EXPECT_FALSE(drv.fetch_rx_data());
}

TEST(PdportDriver, RxRingOverflowDropsOldest) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    // Inject four control frames without draining.
    for (uint8_t id = 0; id < 4; id++) {
        const uint16_t hdr = pdport_test::make_hdr(
            static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), id,
            pd::PD_REVISION::REV30, 0, 0);
        std::vector<uint8_t> f = make_frame(hdr);
        ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP,
                                              f.data(), (uint16_t)f.size()));
    }
    EXPECT_EQ(drv.counters().rx_frames, 4u);

    // Ring capacity is 3: the oldest frame (id 0) was dropped.
    uint8_t ids_seen[4] = {};
    int n = 0;
    while (drv.fetch_rx_data()) {
        ids_seen[n++] = (uint8_t)((port.rx_chunk.header.raw_value >> 9) & 0x7u);
    }
    ASSERT_EQ(n, 3);
    EXPECT_EQ(ids_seen[0], 1u);
    EXPECT_EQ(ids_seen[1], 2u);
    EXPECT_EQ(ids_seen[2], 3u);

    // Drain the four auto GoodCRC replies.
    while (pdport_test::sim().tx_busy) {
        ASSERT_TRUE(pdport_test::sim_tx_complete(0));
    }
}

TEST(PdportDriver, RxDisabledDropsFramesButStillAcks) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    drv.req_rx_enable(false);
    EXPECT_FALSE(drv.is_rx_enable_done());
    drv.service();
    EXPECT_TRUE(drv.is_rx_enable_done());

    const uint16_t hdr = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 0,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> f = make_frame(hdr);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, f.data(),
                                          (uint16_t)f.size()));
    EXPECT_EQ(drv.counters().rx_frames, 0u); // not queued
    EXPECT_FALSE(drv.fetch_rx_data());
    // Still acknowledged on the wire so the partner does not retry forever.
    EXPECT_EQ(drv.counters().goodcrc_sent, 1u);
    ASSERT_TRUE(pdport_test::sim_tx_complete(0));

    drv.req_rx_enable(true);
    drv.service();
    EXPECT_TRUE(drv.is_rx_enable_done());
}

// ---------------------------------------------------------------------
// TX lifecycle
// ---------------------------------------------------------------------

TEST(PdportDriver, TxSucceededOnMatchingGoodCrc) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    // Queue a Request-like data message (1 data object), msg id 5.
    port.tx_chunk.clear();
    port.tx_chunk.header.raw_value = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_DATA_MSGT::Request), 5,
        pd::PD_REVISION::REV30, 1, 0);
    const uint8_t rdo[4] = {0x91, 0x2c, 0x01, 0x00};
    port.tx_chunk.get_data().assign(rdo, rdo + 4);

    drv.req_transmit();
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::ENQUEUED);

    drv.service();
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);
    EXPECT_EQ(drv.counters().tx_frames, 1u);
    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 1);

    // The frame on the wire carries header + payload.
    ASSERT_GE(pdport_test::sim().tx_events.size(), 1u);
    const ParsedTx tx = parse_tx(pdport_test::sim().tx_events.back());
    EXPECT_EQ(tx.hdr, port.tx_chunk.header.raw_value);
    ASSERT_EQ(tx.payload.size(), 4u);
    EXPECT_EQ(memcmp(tx.payload.data(), rdo, 4), 0);

    // Wire-level completion (sent): status must remain SENDING.
    ASSERT_TRUE(pdport_test::sim_tx_complete(0));
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);

    // Partner GoodCRC with the matching message ID resolves the TX.
    const uint16_t gch = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::GoodCRC), 5,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> gf = make_frame(gch);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, gf.data(),
                                          (uint16_t)gf.size()));
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SUCCEEDED);
    EXPECT_EQ(drv.counters().tx_succeeded, 1u);
    EXPECT_EQ(drv.counters().rx_goodcrc_ok, 1u);
    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 0); // idle again
}

TEST(PdportDriver, TxIgnoresWrongGoodCrcIdThenTimesOut) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    port.tx_chunk.clear();
    port.tx_chunk.header.raw_value = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 3,
        pd::PD_REVISION::REV30, 0, 0);
    drv.req_transmit();
    drv.service();
    ASSERT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);
    ASSERT_TRUE(pdport_test::sim_tx_complete(0));

    // A GoodCRC with a different message ID must not resolve the TX.
    const uint16_t gch = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::GoodCRC), 2,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> gf = make_frame(gch);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, gf.data(),
                                          (uint16_t)gf.size()));
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);

    // Watch timeout: FAILED (the PRL then retries).
    pdport_test::sim_advance_ms(4);
    drv.service();
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::FAILED);
    EXPECT_EQ(drv.counters().tx_failed, 1u);
    EXPECT_EQ(pdport_test::sim().sink_tx_ok, 0);
}

TEST(PdportDriver, TxDiscardAndAbortFailImmediately) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    port.tx_chunk.clear();
    port.tx_chunk.header.raw_value = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 0,
        pd::PD_REVISION::REV30, 0, 0);
    drv.req_transmit();
    drv.service();
    ASSERT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);

    // Collision: the UCPD discarded our frame (status 1).
    ASSERT_TRUE(pdport_test::sim_tx_complete(1));
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::FAILED);
    EXPECT_EQ(drv.counters().tx_discard, 1u);
}

TEST(PdportDriver, TxWhileWireBusyStaysEnqueued) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    port.tx_chunk.clear();
    port.tx_chunk.header.raw_value = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 1,
        pd::PD_REVISION::REV30, 0, 0);
    drv.req_transmit();

    // Partner occupies the wire: our frame stays ENQUEUED.
    pdport_test::sim().tx_busy = true;
    drv.service();
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::ENQUEUED);

    // Wire frees: next service round arms it.
    pdport_test::sim().tx_busy = false;
    drv.service();
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);
    ASSERT_TRUE(pdport_test::sim_tx_complete(0));

    const uint16_t gch = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::GoodCRC), 1,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> gf = make_frame(gch);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, gf.data(),
                                          (uint16_t)gf.size()));
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SUCCEEDED);
}

// ---------------------------------------------------------------------
// Hard reset
// ---------------------------------------------------------------------

TEST(PdportDriver, HardResetFromPartnerFlushesAndNotifies) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    PrlSpy spy;
    port.prl_rtr = &spy;

    // A message is queued, then the partner sends a hard reset.
    const uint16_t hdr = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_CTRL_MSGT::Soft_Reset), 0,
        pd::PD_REVISION::REV30, 0, 0);
    std::vector<uint8_t> f = make_frame(hdr);
    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, f.data(),
                                          (uint16_t)f.size()));

    // Pending TX state is dropped on the reset.
    port.tx_chunk.clear();
    port.tx_chunk.header.raw_value = hdr;
    drv.req_transmit();
    drv.service(); // ENQUEUED
    ASSERT_TRUE(pdport_test::sim_tx_complete(0)); // SENDING

    pd_drv_on_hr_rx();
    drv.service();

    EXPECT_EQ(spy.hr_count, 1);
    EXPECT_EQ(drv.counters().hr_rx, 1u);
    // The queued frame was flushed by the hard reset.
    EXPECT_FALSE(drv.fetch_rx_data());
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::UNSET);
}

TEST(PdportDriver, HardResetSendCompletes) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    drv.req_hr_send();
    EXPECT_FALSE(drv.is_hr_send_done());
    EXPECT_EQ(pdport_test::sim().hr_events, 1);
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SENDING);

    pd_drv_on_hr_done();
    EXPECT_TRUE(drv.is_hr_send_done());
    EXPECT_EQ(port.tcpc_tx_status.load(), pd::TCPC_TRANSMIT_STATUS::SUCCEEDED);
}

// ---------------------------------------------------------------------
// Extended (chunked) frames
// ---------------------------------------------------------------------

TEST(PdportDriver, ExtendedMessagePassThrough) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);
    attach_cc1(drv);

    // A chunked extended frame: PD header (extended) + payload made of the
    // 2-byte extended header + 24 data bytes.  pdsink expects the payload
    // bytes verbatim (ehdr at offset 0).
    uint16_t hdr = pdport_test::make_hdr(
        static_cast<uint8_t>(pd::PD_EXT_MSGT::EPR_Source_Capabilities), 0,
        pd::PD_REVISION::REV30, 0, 1);
    uint8_t payload[26];
    payload[0] = 0x8c; // ehdr low: data size 12, chunked
    payload[1] = 0x00; // ehdr high: chunk number 0
    for (int i = 2; i < 26; i++) { payload[i] = (uint8_t)i; }
    std::vector<uint8_t> f = make_frame(hdr, payload, sizeof(payload));

    ASSERT_TRUE(pdport_test::sim_rx_frame(PD_SOP_SOP, f.data(),
                                          (uint16_t)f.size()));
    ASSERT_TRUE(drv.fetch_rx_data());
    EXPECT_EQ(port.rx_chunk.header.raw_value, hdr);
    ASSERT_EQ(port.rx_chunk.get_data().size(), sizeof(payload));
    EXPECT_EQ(memcmp(port.rx_chunk.get_data().data(), payload, sizeof(payload)),
              0);
    ASSERT_TRUE(pdport_test::sim_tx_complete(0)); // drain the GoodCRC
}

// ---------------------------------------------------------------------
// 1 ms stack tick
// ---------------------------------------------------------------------

TEST(PdportDriver, OneMsTickForwardedToTask) {
    pdport_test::sim_reset();
    pd::Port port;
    UcpdDriver drv(port);

    // Register a task spy to count MsgTask_Timer deliveries.
    struct TaskSpy
        : public etl::message_router<TaskSpy, pd::MsgTask_Timer> {
        TaskSpy() : etl::message_router<TaskSpy, pd::MsgTask_Timer>(2) {}
        int ticks = 0;
        void on_receive(const pd::MsgTask_Timer&) { ticks++; }
        void on_receive_unknown(const etl::imessage&) {}
    } spy;
    port.task_rtr = &spy;

    drv.service();
    EXPECT_EQ(spy.ticks, 0); // first call only arms the tick baseline

    pdport_test::sim_advance_ms(1);
    drv.service();
    EXPECT_EQ(spy.ticks, 1);

    pdport_test::sim_advance_ms(1);
    drv.service();
    EXPECT_EQ(spy.ticks, 2);
}
