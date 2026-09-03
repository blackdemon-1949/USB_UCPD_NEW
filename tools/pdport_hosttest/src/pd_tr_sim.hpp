/*
 * pd_tr_sim.hpp - scripted in-process transport for the UCPD driver tests.
 *
 * Implements the C transport API (pd_tr.h) against simulated state so the
 * driver (pd_ucpd_driver.cpp) can be exercised on the host: scripted RX
 * frames, captured TX frames, a fake CC/VBUS environment and a
 * test-controlled clock.  The "wire" is lossless and synchronous:
 *
 *   - pd_tr_send_frame() arms a TX and marks the wire busy; nothing moves
 *     until the test calls tx_complete() (which invokes the driver's
 *     on_tx_done) or cancels it.
 *   - RX frames are injected with rx_frame(); the transport calls the
 *     driver's pd_drv_on_rx_frame() exactly like the UCPD IRQ path would.
 */
#ifndef PD_TR_SIM_HPP
#define PD_TR_SIM_HPP

#include <stdint.h>

#include <vector>

#include "pd_tr.h"

namespace pdport_test {

// ---- test-facing transport state -------------------------------------

struct SimState {
    // CC levels per line (PD_CC_* codes)
    int cc1 = PD_CC_NONE;
    int cc2 = PD_CC_NONE;
    int vbus = 0; // synthetic VBUS presence
    int sink_tx_ok = 0; // SinkTxOK/SinkTxNG presentation state

    // active CC line (1 or 2) selected via pd_tr_set_active_cc
    int active_cc = 1;

    // transport "wire" state
    bool tx_busy = false;      // a frame TX is in progress
    bool attached = false;     // pd_tr_attach/detach
    int attach_line = 0;       // line passed to pd_tr_attach

    // captured TX events (all frames the driver armed)
    struct TxEvent {
        uint8_t sop;
        uint16_t len;
        uint8_t data[40];
    };
    std::vector<TxEvent> tx_events;
    int hr_events = 0;         // hard reset bursts requested
    int init_calls = 0;

    uint32_t now_ms = 0;       // test-controlled monotonic clock
};

// Global sim state (single port in tests).
SimState& sim();

// ---- test helpers -----------------------------------------------------

// Reset the whole simulated transport state (per-test).
void sim_reset(void);

// Advance the sim clock (drives the driver's TX watch timeout).
void sim_advance_ms(uint32_t ms);

// Set CC line levels and VBUS.
void sim_set_cc(int cc1, int cc2);
void sim_set_vbus(int ok);

// Inject an RX frame: bytes are the full wire frame: [hdr(2) | payload].
// `sop` is one of PD_SOP_*.  Returns false if the frame was rejected by
// the driver entry (malformed).
bool sim_rx_frame(uint8_t sop, const uint8_t* data, uint16_t len);

// Complete/abort the TX that is currently on the wire (status: 0 sent,
// 1 discarded, 2 aborted).  Returns false if no TX was pending.
bool sim_tx_complete(int status);

// Convenience: pull a data payload (PD header already encoded, payload
// bytes following) into the last TX frame.
bool last_tx(uint8_t& sop, const uint8_t*& data, uint16_t& len);

// Build a little-endian 2-byte message header value.
uint16_t make_hdr(uint8_t msg_type, uint8_t msg_id, uint8_t spec_rev,
                  uint8_t num_data_objs, uint8_t extended);

} // namespace pdport_test

#endif /* PD_TR_SIM_HPP */
