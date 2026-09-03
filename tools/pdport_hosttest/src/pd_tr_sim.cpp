/*
 * pd_tr_sim.cpp - implements the C transport API (pd_tr.h) over the
 * simulated state in pd_tr_sim.hpp.
 */
#include "pd_tr_sim.hpp"

#include <cstring>

extern "C" {

#include "pd_tr.h"

int pd_tr_send_frame(uint8_t sop, const uint8_t* buf, uint16_t len) {
    pdport_test::SimState& s = pdport_test::sim();
    if (s.tx_busy) { return -1; }
    if (buf == nullptr || len < 2 || len > PD_TR_MAX_FRAME) { return -1; }
    pdport_test::SimState::TxEvent ev;
    ev.sop = sop;
    ev.len = len;
    std::memcpy(ev.data, buf, len);
    s.tx_events.push_back(ev);
    s.tx_busy = true;
    return 0;
}

int pd_tr_send_hard_reset(void) {
    pdport_test::SimState& s = pdport_test::sim();
    s.hr_events++;
    return 0;
}

void pd_tr_read_cc(int* cc1, int* cc2) {
    pdport_test::SimState& s = pdport_test::sim();
    if (cc1) { *cc1 = s.cc1; }
    if (cc2) { *cc2 = s.cc2; }
}

int pd_tr_read_active_cc(void) {
    pdport_test::SimState& s = pdport_test::sim();
    return (s.active_cc == 1) ? s.cc1 : s.cc2;
}

void pd_tr_set_active_cc(int cc) {
    pdport_test::SimState& s = pdport_test::sim();
    if (cc == 1 || cc == 2) { s.active_cc = cc; }
}

void pd_tr_attach(int cc) {
    pdport_test::SimState& s = pdport_test::sim();
    s.attached = true;
    s.attach_line = cc;
    s.vbus = 1;
}

void pd_tr_detach(void) {
    pdport_test::SimState& s = pdport_test::sim();
    s.attached = false;
    s.attach_line = 0;
    s.vbus = 0;
}

int pd_tr_vbus_ok(void) {
    pdport_test::SimState& s = pdport_test::sim();
    return s.vbus;
}

void pd_tr_set_sink_tx_ok(int enable) {
    pdport_test::SimState& s = pdport_test::sim();
    s.sink_tx_ok = enable ? 1 : 0;
}

int pd_tr_tx_busy(void) {
    pdport_test::SimState& s = pdport_test::sim();
    return s.tx_busy ? 1 : 0;
}

uint32_t pd_tr_now_ms(void) {
    pdport_test::SimState& s = pdport_test::sim();
    return s.now_ms;
}

int pd_tr_init(void) {
    pdport_test::SimState& s = pdport_test::sim();
    s.init_calls++;
    return 0;
}

} // extern "C"

namespace pdport_test {

SimState g_sim;

SimState& sim() { return g_sim; }

void sim_reset(void) {
    g_sim = SimState{};
    // The transport starts un-attached but the driver holds no state: the
    // active-driver registration survives reset (tests create one driver
    // per test body).
}

void sim_advance_ms(uint32_t ms) { g_sim.now_ms += ms; }

void sim_set_cc(int cc1, int cc2) {
    g_sim.cc1 = cc1;
    g_sim.cc2 = cc2;
}

void sim_set_vbus(int ok) { g_sim.vbus = ok; }

bool sim_rx_frame(uint8_t sop, const uint8_t* data, uint16_t len) {
    if (data == nullptr || len < 2 || len > PD_TR_MAX_FRAME) { return false; }
    // Mirror the transport behaviour: frames land while a TX is running
    // are still delivered (the UCPD RX path is independent of TX).
    pd_drv_on_rx_frame(data, len, sop);
    return true;
}

bool sim_tx_complete(int status) {
    if (!g_sim.tx_busy) { return false; }
    g_sim.tx_busy = false;
    pd_drv_on_tx_done(status);
    return true;
}

bool last_tx(uint8_t& sop, const uint8_t*& data, uint16_t& len) {
    if (g_sim.tx_events.empty()) { return false; }
    const SimState::TxEvent& ev = g_sim.tx_events.back();
    sop = ev.sop;
    data = ev.data;
    len = ev.len;
    return true;
}

uint16_t make_hdr(uint8_t msg_type, uint8_t msg_id, uint8_t spec_rev,
                  uint8_t num_data_objs, uint8_t extended) {
    return (uint16_t)((msg_type & 0x1Fu) | ((spec_rev & 0x3u) << 6) |
                      ((msg_id & 0x7u) << 9) | ((num_data_objs & 0x7u) << 12) |
                      ((extended & 0x1u) << 15));
}

} // namespace pdport_test
