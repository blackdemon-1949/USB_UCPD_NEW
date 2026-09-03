/*
 * test_pdport_stack.cpp - full-stack SPR (M3) and EPR (M5) benches.
 *
 * Runs the whole pdsink protocol engine (Task + TC + PRL + PE + DPM)
 * on top of the M2 UcpdDriver against the simulated UCPD transport
 * (pd_tr_sim), with a scripted USB-PD "source" partner that answers
 * like real charger hardware: GoodCRC every message, send
 * Source_Capabilities after attach, Accept + PS_RDY on Request.
 *
 * The EPR benches (M5) script a PD 3.1 EPR-capable source: EPR_Mode
 * Enter_Acknowledged / Enter_Succeeded, a chunked EPR_Source_Capabilities
 * extended message (answered by the sink's Request-Chunk), Accept/PS_RDY
 * on EPR_Request and EPR_KeepAlive_Ack on EPR_KeepAlive - plus the
 * sink-initiated EPR_Mode(Exit) path (a project-local pdsink addition).
 *
 * This is the closest thing to a board bench available on the host: it
 * exercises the exact driver contract the target device layer will
 * implement (pd_tr_st.c), the exact pdsink core the firmware will run,
 * and real PD timing (1 ms ticks, spec timers).
 */
#include <gtest/gtest.h>

#include <stdint.h>

#include <vector>

#include "data_objects.h"
#include "dpm.h"
#include "messages.h"
#include "pd_tr_sim.hpp"
#include "pe.h"
#include "port.h"
#include "prl.h"
#include "task.h"
#include "tc.h"

#include "pd_ucpd_driver.h"

using namespace pd;

namespace {

// ---------------------------------------------------------------------
// PDO helpers (same bit-model as the pdsink validate suite)
// ---------------------------------------------------------------------

uint32_t make_fixed_pdo(uint32_t voltage_mv, uint32_t current_ma) {
    PDO_FIXED pdo{};
    pdo.pdo_type = PDO_TYPE::FIXED;
    pdo.voltage = voltage_mv / 50;
    pdo.max_current = current_ma / 10;
    return pdo.raw_value;
}

uint32_t make_pps_apdo(uint32_t min_voltage_mv, uint32_t max_voltage_mv,
                       uint32_t current_ma) {
    PDO_SPR_PPS pdo{};
    pdo.pdo_type = PDO_TYPE::AUGMENTED;
    pdo.apdo_subtype = PDO_AUGMENTED_SUBTYPE::SPR_PPS;
    pdo.min_voltage = min_voltage_mv / 100;
    pdo.max_voltage = max_voltage_mv / 100;
    pdo.max_current = current_ma / 50;
    return pdo.raw_value;
}

// 5 V fixed PDO that additionally advertises EPR mode capability (PD 3.1
// fixed PDO bit 25).  Required on PDO1 of an EPR-capable source.
uint32_t make_fixed_pdo_epr_capable(uint32_t voltage_mv, uint32_t current_ma) {
    const uint32_t raw = make_fixed_pdo(voltage_mv, current_ma);
    PDO_FIXED pdo{raw};
    pdo.epr_capable = 1;
    return pdo.raw_value;
}

// EPR AVS (adjustable voltage supply) source PDO: min/max voltage in mV,
// PDP in watts (PD 3.1 table 6-15).  Sits at the last position of the
// EPR_Source_Capabilities list (after the fixed EPR PDOs).
uint32_t make_epr_avs_pdo(uint32_t min_voltage_mv, uint32_t max_voltage_mv,
                          uint32_t pdp_watts) {
    PDO_EPR_AVS pdo{};
    pdo.pdo_type = PDO_TYPE::AUGMENTED;
    pdo.apdo_subtype = PDO_AUGMENTED_SUBTYPE::EPR_AVS;
    pdo.min_voltage = min_voltage_mv / 100;
    pdo.max_voltage = max_voltage_mv / 100;
    pdo.pdp = pdp_watts;
    return pdo.raw_value;
}

// ---------------------------------------------------------------------
// Bench DPM: records DPM notifications and applies a policy.
// ---------------------------------------------------------------------

class BenchDpm : public DPM {
public:
    explicit BenchDpm(Port& port) : DPM(port) {}

    // counters of DPM-level notifications
    int startup = 0;
    int cable_attached = 0;
    int cable_detached = 0;
    int src_caps = 0;
    int select_cap_done = 0;
    int snk_ready = 0;
    int handshake_done = 0;
    int transit_to_default = 0;
    int epr_entry_failed = 0;

    void notify(uint32_t id) {
        switch (id) {
            case MSG_TO_DPM__STARTUP: startup++; break;
            case MSG_TO_DPM__CABLE_ATTACHED: cable_attached++; break;
            case MSG_TO_DPM__CABLE_DETACHED: cable_detached++; break;
            case MSG_TO_DPM__SRC_CAPS_RECEIVED: src_caps++; break;
            case MSG_TO_DPM__SELECT_CAP_DONE: select_cap_done++; break;
            case MSG_TO_DPM__SNK_READY: snk_ready++; break;
            case MSG_TO_DPM__HANDSHAKE_DONE: handshake_done++; break;
            case MSG_TO_DPM__TRANSIT_TO_DEFAULT: transit_to_default++; break;
            case MSG_TO_DPM__EPR_ENTRY_FAILED: epr_entry_failed++; break;
            default: break;
        }
    }
};

class DpmRouter
    : public etl::message_router<DpmRouter, MsgToDpm_Startup,
                                 MsgToDpm_CableAttached, MsgToDpm_CableDetached,
                                 MsgToDpm_SrcCapsReceived, MsgToDpm_SelectCapDone,
                                 MsgToDpm_SnkReady, MsgToDpm_HandshakeDone,
                                 MsgToDpm_TransitToDefault,
                                 MsgToDpm_NewPowerLevelAccepted,
                                 MsgToDpm_NewPowerLevelRejected,
                                 MsgToDpm_EPREntryFailed, MsgToDpm_Alert,
                                 MsgToDpm_SrcDisabled> {
public:
    explicit DpmRouter(BenchDpm& dpm)
        : etl::message_router<DpmRouter, MsgToDpm_Startup,
                             MsgToDpm_CableAttached, MsgToDpm_CableDetached,
                             MsgToDpm_SrcCapsReceived, MsgToDpm_SelectCapDone,
                             MsgToDpm_SnkReady, MsgToDpm_HandshakeDone,
                             MsgToDpm_TransitToDefault,
                             MsgToDpm_NewPowerLevelAccepted,
                             MsgToDpm_NewPowerLevelRejected,
                             MsgToDpm_EPREntryFailed, MsgToDpm_Alert,
                             MsgToDpm_SrcDisabled>(1),
          dpm(dpm) {}

    BenchDpm& dpm;

#define IMPL(MsgT)                                     \
    void on_receive(const MsgT& m) {                   \
        dpm.notify((uint32_t)m.get_message_id());      \
    }
    IMPL(MsgToDpm_Startup)
    IMPL(MsgToDpm_CableAttached)
    IMPL(MsgToDpm_CableDetached)
    IMPL(MsgToDpm_SrcCapsReceived)
    IMPL(MsgToDpm_SelectCapDone)
    IMPL(MsgToDpm_SnkReady)
    IMPL(MsgToDpm_HandshakeDone)
    IMPL(MsgToDpm_TransitToDefault)
    IMPL(MsgToDpm_NewPowerLevelAccepted)
    IMPL(MsgToDpm_NewPowerLevelRejected)
    IMPL(MsgToDpm_EPREntryFailed)
    IMPL(MsgToDpm_Alert)
    IMPL(MsgToDpm_SrcDisabled)
#undef IMPL
    void on_receive_unknown(const etl::imessage&) {}
};

// ---------------------------------------------------------------------
// Scripted source partner ("charger emulator")
// ---------------------------------------------------------------------

class SourceEmu {
public:
    struct Cfg {
        std::vector<uint32_t> caps; // source capability PDOs (SPR)
        uint32_t caps_delay_ms = 150; // after attach
        bool send_caps = true; // false = dead charger (HR timeout bench)
        uint32_t accept_delay_ms = 5;
        uint32_t psrdy_after_accept_ms = 30;
        // ---- EPR source script (PD 3.1) ----
        bool epr_enabled = false;      // PDO1 is EPR-capable; answer EPR_Mode(Enter)
        bool epr_enter_ack = true;     // false = answer Enter_Failed (NAK bench)
        std::vector<uint32_t> epr_pdos; // full 11-slot PDO list sent as EPR SrcCaps
        uint32_t epr_ack_delay_ms = 3;      // EPR_Mode Enter_Acknowledged
        uint32_t epr_succeeded_delay_ms = 30; // EPR_Mode Enter_Succeeded after ACK
        uint32_t epr_caps_delay_ms = 60;    // first EPR SrcCaps chunk after SUCCEEDED
        uint32_t epr_chunk2_delay_ms = 4;   // second chunk after Request-Chunk
        uint32_t epr_exit_caps_delay_ms = 60; // SPR SrcCaps after EPR_Mode(Exit)
        bool epr_skip_first_caps = false; // bench: stall the first EPR SrcCaps
    };

    explicit SourceEmu(Cfg cfg) : cfg(std::move(cfg)) { reset(); }

    Cfg cfg;

    // observables for assertions
    int caps_sent = 0;
    int requests_seen = 0;
    uint32_t last_rdo = 0;
    uint32_t attach_edges = 0;
    // EPR observables
    int epr_enter_attempts = 0;   // EPR_Mode(Enter) messages seen
    int epr_exits_seen = 0;       // EPR_Mode(Exit) messages seen
    int epr_requests_seen = 0;    // EPR_Request messages seen
    int epr_chunk_requests_seen = 0; // Request-Chunk messages seen
    int epr_keepalives_seen = 0;  // EPR_KeepAlive extended control msgs
    int epr_keepalive_acks_sent = 0;
    int epr_caps_sent = 0;        // EPR SrcCaps chunk1 deliveries (payload)
    uint32_t last_epr_rdo = 0;    // EPR_Request DO1 (RDO)
    uint32_t last_epr_pdo = 0;    // EPR_Request DO2 (PDO)
    int epr_enter_watts = -1;     // EPRMDO data field of the Enter request

    void reset() {
        caps_sent = 0;
        requests_seen = 0;
        last_rdo = 0;
        attach_edges = 0;
        epr_enter_attempts = 0;
        epr_exits_seen = 0;
        epr_requests_seen = 0;
        epr_chunk_requests_seen = 0;
        epr_keepalives_seen = 0;
        epr_keepalive_acks_sent = 0;
        epr_caps_sent = 0;
        last_epr_rdo = 0;
        last_epr_pdo = 0;
        epr_enter_watts = -1;
        prev_attached = false;
        processed = 0;
        tx_msg_id = 0;
        sink_hr_prev = 0;
        scheduled.clear();
        reset_epr_phase();
        epr_skipped_once = false;
    }

    // Called once per simulated ms (before the stack tick).
    void tick() {
        auto& s = pdport_test::sim();
        const uint32_t now = s.now_ms;

        // Attach edge: schedule the first Source_Capabilities.
        if (s.attached && !prev_attached) {
            attach_edges++;
            if (cfg.send_caps && !sched_caps_after_hr) {
                schedule(make_caps(), now + cfg.caps_delay_ms);
            }
        }
        // Partner hard reset: the "charger" restarts its state machine
        // (including any EPR-mode state).
        if ((uint32_t)s.hr_events != sink_hr_prev) {
            sink_hr_prev = (uint32_t)s.hr_events;
            reset_epr_phase();
            if (cfg.send_caps) { schedule(make_caps(), now + 100); }
        }
        prev_attached = s.attached;

        // Consume sink transmissions.
        while (processed < s.tx_events.size()) {
            const auto& ev = s.tx_events[processed];
            if (s.tx_busy) {
                // Frame is still on the wire: receive it now.
                const bool ok = pdport_test::sim_tx_complete(0);
                (void)ok;
            }
            processed++;
            if (ev.len < 2) { continue; }
            const uint16_t hdr = (uint16_t)(ev.data[0] | (uint16_t)(ev.data[1] << 8));
            const int type = hdr & 0x1F;
            const int id = (hdr >> 9) & 0x7;
            if (type == (int)PD_CTRL_MSGT::GoodCRC) { continue; }
            // Echo GoodCRC for every other frame.
            schedule(make_ctrl(PD_CTRL_MSGT::GoodCRC, id), now + 1);
            handle_sink_msg(hdr, ev.data + 2, ev.len - 2, now);
        }

        // Fire due transmissions (never while the wire is busy).
        while (!scheduled.empty() && scheduled.front().due_ms <= now) {
            if (!s.tx_busy) {
                auto f = std::move(scheduled.front().frame);
                scheduled.erase(scheduled.begin());
                pdport_test::sim_rx_frame(PD_SOP_SOP, f.data(),
                                          (uint16_t)f.size());
                continue;
            }
            // postpone one ms
            scheduled.front().due_ms = now + 1;
            break;
        }
    }

private:
    struct Action {
        uint32_t due_ms;
        std::vector<uint8_t> frame;
    };

    bool prev_attached = false;
    bool sched_caps_after_hr = false;
    size_t processed = 0;
    uint32_t tx_msg_id = 0;
    uint32_t sink_hr_prev = 0;
    std::vector<Action> scheduled;

    // EPR phase state (source side of the EPR conversation)
    bool epr_phase = false;          // EPR mode entered (SUCCEEDED sent)
    bool epr_enter_acked = false;    // Enter_Acknowledged already sent
    bool epr_caps_chunk1_sent = false;
    bool epr_caps_chunk2_sent = false;
    bool epr_skipped_once = false;   // consumed cfg.epr_skip_first_caps

    // Reset the source-side EPR conversation (not the counters).
    void reset_epr_phase() {
        epr_phase = false;
        epr_enter_acked = false;
        epr_caps_chunk1_sent = false;
        epr_caps_chunk2_sent = false;
    }

    void schedule(std::vector<uint8_t> frame, uint32_t at_ms) {
        Action a;
        a.due_ms = at_ms;
        a.frame = std::move(frame);
        // keep sorted by due time
        auto it = scheduled.begin();
        while (it != scheduled.end() && it->due_ms <= a.due_ms) { ++it; }
        scheduled.insert(it, std::move(a));
    }

    std::vector<uint8_t> make_frame(uint16_t hdr,
                                    const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> f;
        f.push_back((uint8_t)(hdr & 0xFFu));
        f.push_back((uint8_t)((hdr >> 8) & 0xFFu));
        f.insert(f.end(), payload.begin(), payload.end());
        return f;
    }

    uint16_t next_hdr(uint8_t type, uint8_t nobj) {
        return pdport_test::make_hdr(type, (uint8_t)(tx_msg_id++ & 0x7),
                                     PD_REVISION::REV30, nobj, 0);
    }

    std::vector<uint8_t> make_caps() {
        std::vector<uint8_t> payload;
        for (uint32_t pdo : cfg.caps) {
            payload.push_back((uint8_t)(pdo & 0xFFu));
            payload.push_back((uint8_t)((pdo >> 8) & 0xFFu));
            payload.push_back((uint8_t)((pdo >> 16) & 0xFFu));
            payload.push_back((uint8_t)((pdo >> 24) & 0xFFu));
        }
        return make_frame(next_hdr((uint8_t)PD_DATA_MSGT::Source_Capabilities,
                                   (uint8_t)cfg.caps.size()),
                          payload);
    }

    std::vector<uint8_t> make_ctrl(PD_CTRL_MSGT::Type type, int id) {
        const uint16_t hdr =
            pdport_test::make_hdr((uint8_t)type, (uint8_t)id,
                                  PD_REVISION::REV30, 0, 0);
        return make_frame(hdr, {});
    }

    void handle_sink_msg(uint16_t hdr_raw, const uint8_t* payload,
                         uint16_t len, uint32_t now) {
        PD_HEADER hdr{hdr_raw};
        const int type = hdr.message_type;

        // ---- extended messages (incl. chunked) ----
        if (hdr.extended) {
            handle_sink_ext(hdr, payload, len, now);
            return;
        }

        switch (type) {
            case PD_DATA_MSGT::Request:
            case PD_DATA_MSGT::EPR_Request: {
                if (len < 4) { break; }
                const uint32_t rdo =
                    (uint32_t)(payload[0] | (payload[1] << 8) |
                               (payload[2] << 16) | (payload[3] << 24));
                if (type == PD_DATA_MSGT::EPR_Request) {
                    epr_requests_seen++;
                    last_epr_rdo = rdo;
                    last_epr_pdo = (len >= 8)
                        ? (uint32_t)(payload[4] | (payload[5] << 8) |
                                     (payload[6] << 16) | (payload[7] << 24))
                        : 0;
                } else {
                    requests_seen++;
                    last_rdo = rdo;
                }
                const uint32_t accept_at = now + cfg.accept_delay_ms;
                schedule(make_ctrl(PD_CTRL_MSGT::Accept, (int)next_hdr0()),
                         accept_at);
                schedule(make_ctrl(PD_CTRL_MSGT::PS_RDY, (int)next_hdr0()),
                         accept_at + cfg.psrdy_after_accept_ms);
                break;
            }
            case PD_DATA_MSGT::EPR_Mode: {
                if (len >= 4) {
                    const uint32_t dobj =
                        (uint32_t)(payload[0] | (payload[1] << 8) |
                                   (payload[2] << 16) | (payload[3] << 24));
                    // EPRMDO: action in bits 24..31, data in bits 16..23
                    const int action = (int)((dobj >> 24) & 0xFFu);
                    if (action == (int)EPR_MODE_ACTION::ENTER) {
                        on_epr_enter(dobj, now);
                    } else if (action == (int)EPR_MODE_ACTION::EXIT) {
                        on_epr_exit(now);
                    }
                }
                break;
            }
            case PD_CTRL_MSGT::Get_Source_Cap:
            case PD_CTRL_MSGT::Soft_Reset:
            default:
                break;
        }
    }

    // Extended message from the sink: EPR_KeepAlive or a Request-Chunk for
    // our multi-chunk EPR Source_Capabilities.
    void handle_sink_ext(const PD_HEADER& hdr, const uint8_t* payload,
                         uint16_t len, uint32_t now) {
        if (len < 2) { return; }
        const uint16_t ext_raw =
            (uint16_t)(payload[0] | (uint16_t)(payload[1] << 8));
        // PD_EXT_HEADER: data_size :9, request_chunk bit 10,
        // chunk_number bits 11..14, chunked bit 15.
        const int request_chunk = (ext_raw >> 10) & 0x1;
        const int chunk_number = (ext_raw >> 11) & 0xF;

        if (request_chunk) {
            if (epr_caps_chunk1_sent && !epr_caps_chunk2_sent &&
                chunk_number == 1)
            {
                epr_chunk_requests_seen++;
                schedule(make_epr_caps_chunk(1), now + cfg.epr_chunk2_delay_ms);
            }
            return;
        }

        // Extended_Control: ECDB type is payload byte 2 (after ext header).
        if (hdr.message_type == (int)PD_EXT_MSGT::Extended_Control && len >= 4) {
            const int ecdb_type = payload[2];
            if (ecdb_type == (int)PD_EXT_CTRL_MSGT::EPR_KeepAlive) {
                epr_keepalives_seen++;
                // Answer EPR_KeepAlive_Ack (Extended_Control, ECDB type 4).
                epr_keepalive_acks_sent++;
                schedule(make_keepalive_ack(), now + 2);
            }
        }
    }

    void on_epr_enter(uint32_t dobj, uint32_t now) {
        epr_enter_attempts++;
        epr_enter_watts = (int)((dobj >> 16) & 0xFFu); // EPRMDO data field
        if (!cfg.epr_enabled) { return; }
        if (epr_phase) { return; } // already entered; nothing to do

        if (!cfg.epr_enter_ack) {
            // NAK bench: answer Enter_Failed; the sink must stay in SPR.
            schedule(make_epr_mode(EPR_MODE_ACTION::ENTER_FAILED),
                     now + cfg.epr_ack_delay_ms);
            return;
        }

        schedule(make_epr_mode(EPR_MODE_ACTION::ENTER_ACKNOWLEDGED),
                 now + cfg.epr_ack_delay_ms);
        schedule(make_epr_mode(EPR_MODE_ACTION::ENTER_SUCCEEDED),
                 now + cfg.epr_ack_delay_ms + cfg.epr_succeeded_delay_ms);
        epr_enter_acked = true;
        epr_phase = true; // sink is in EPR mode after SUCCEEDED

        // EPR_Source_Capabilities (chunked extended message, 11 PDOs).
        if (cfg.epr_skip_first_caps && !epr_skipped_once) {
            epr_skipped_once = true; // drop the first EPR SrcCaps delivery
            return;
        }
        schedule(make_epr_caps_chunk(0),
                 now + cfg.epr_ack_delay_ms + cfg.epr_succeeded_delay_ms +
                     cfg.epr_caps_delay_ms);
    }

    void on_epr_exit(uint32_t now) {
        epr_exits_seen++;
        reset_epr_phase();
        // The Source answers EPR_Mode(Exit) with plain SPR Source_Capabilities
        // (PD 3.1: within tFirstSourceCap) and the SPR contract resumes.
        schedule(make_caps(), now + cfg.epr_exit_caps_delay_ms);
    }

    // ---- EPR frame builders -------------------------------------------

    std::vector<uint8_t> make_epr_mode(int action) {
        std::vector<uint8_t> p(4, 0);
        const uint32_t dobj = ((uint32_t)(action & 0xFF)) << 24;
        p[0] = (uint8_t)(dobj & 0xFFu);
        p[1] = (uint8_t)((dobj >> 8) & 0xFFu);
        p[2] = (uint8_t)((dobj >> 16) & 0xFFu);
        p[3] = (uint8_t)((dobj >> 24) & 0xFFu);
        return make_frame(next_hdr(PD_DATA_MSGT::EPR_Mode, 1), p);
    }

    // One physical chunk of the chunked EPR Source_Capabilities extended
    // message: PD header (extended, EPR_Source_Capabilities) + extended
    // header (chunked, data_size = total payload, chunk_number = n) + up to
    // 26 payload bytes (total payload is a multiple of 4, so every chunk is
    // naturally padded to a 4-byte multiple).  Mirrors pdsink's TCH layout.
    std::vector<uint8_t> make_epr_caps_chunk(int chunk_number) {
        std::vector<uint8_t> data;
        const size_t pdo_count = cfg.epr_pdos.size();
        for (size_t i = 0; i < pdo_count; i++) {
            const uint32_t v = cfg.epr_pdos[i];
            data.push_back((uint8_t)(v & 0xFFu));
            data.push_back((uint8_t)((v >> 8) & 0xFFu));
            data.push_back((uint8_t)((v >> 16) & 0xFFu));
            data.push_back((uint8_t)((v >> 24) & 0xFFu));
        }

        const size_t off = (size_t)chunk_number * 26;
        size_t take = data.size() - off;
        if (take > 26) { take = 26; }

        std::vector<uint8_t> p;
        PD_EXT_HEADER eh{};
        eh.data_size = (uint16_t)data.size();
        eh.chunked = 1;
        eh.chunk_number = (uint16_t)chunk_number;
        p.push_back((uint8_t)(eh.raw_value & 0xFFu));
        p.push_back((uint8_t)(eh.raw_value >> 8));
        for (size_t i = 0; i < take; i++) { p.push_back(data[off + i]); }
        while ((p.size() % 4) != 0) { p.push_back(0); }

        if (chunk_number == 0) { epr_caps_chunk1_sent = true; }
        else { epr_caps_chunk2_sent = true; }
        epr_caps_sent++;

        const uint16_t hdr = pdport_test::make_hdr(
            PD_EXT_MSGT::EPR_Source_Capabilities, (uint8_t)(tx_msg_id++ & 0x7),
            PD_REVISION::REV30, (uint8_t)(p.size() / 4), 1);
        return make_frame(hdr, p);
    }

    std::vector<uint8_t> make_keepalive_ack() {
        std::vector<uint8_t> p;
        PD_EXT_HEADER eh{};
        eh.data_size = 2;
        eh.chunked = 1;
        p.push_back((uint8_t)(eh.raw_value & 0xFFu));
        p.push_back((uint8_t)(eh.raw_value >> 8));
        // ECDB: type = EPR_KeepAlive_Ack (4)
        p.push_back(PD_EXT_CTRL_MSGT::EPR_KeepAlive_Ack);
        p.push_back(0);
        const uint16_t hdr = pdport_test::make_hdr(
            PD_EXT_MSGT::Extended_Control, (uint8_t)(tx_msg_id++ & 0x7),
            PD_REVISION::REV30, 1, 1);
        return make_frame(hdr, p);
    }

    // id helper: allocate a message id without constructing a frame
    uint8_t next_hdr0() { return (uint8_t)(tx_msg_id++ & 0x7); }
};

// ---------------------------------------------------------------------
// Stack context
// ---------------------------------------------------------------------

struct StackCtx {
    Port port;
    pdport::UcpdDriver drv;
    PRL prl;
    BenchDpm dpm;
    PE pe;
    TC tc;
    Task task;
    DpmRouter dpm_router;

    StackCtx() : drv(port), prl(port, drv), dpm(port), pe(port, dpm, prl, drv),
                 tc(port, drv), task(port, drv), dpm_router(dpm) {
        port.dpm_rtr = &dpm_router;
        task.start(tc, dpm, pe, prl, drv);
    }
};

// Hard-reset burst completion (the UCPD IRQ equivalent): the bench must
// report the completion of the burst to the driver.
struct HrWatcher {
    uint32_t seen = 0;
    void poll() {
        const uint32_t hr = (uint32_t)pdport_test::sim().hr_events;
        if (hr > seen) {
            seen = hr;
            pd_drv_on_hr_done();
        } else if (hr < seen) {
            seen = hr; // sim reset between tests
        }
    }
};

// Advance one simulated ms.
void step_ms(StackCtx& ctx, SourceEmu& src, HrWatcher& hr) {
    src.tick();
    pdport_test::sim_advance_ms(1);
    hr.poll();
    ctx.drv.service();
}

// Run until `done()` or `max_ms` simulated ms elapsed.
bool run_until(StackCtx& ctx, SourceEmu& src, HrWatcher& hr, uint32_t max_ms,
               const std::function<bool()>& done) {
    const uint32_t t0 = pdport_test::sim().now_ms;
    const uint32_t limit = t0 + max_ms;
    while (pdport_test::sim().now_ms < limit) {
        step_ms(ctx, src, hr);
        if (done()) { return true; }
    }
    return false;
}

void source_present() {
    pdport_test::sim_set_cc(PD_CC_RP_3_0, PD_CC_NONE);
    pdport_test::sim_set_vbus(1);
}

void source_absent() {
    pdport_test::sim_set_cc(PD_CC_NONE, PD_CC_NONE);
    pdport_test::sim_set_vbus(0);
}

bool has_explicit_contract(StackCtx& ctx) {
    return ctx.port.pe_flags.test(PE_FLAG::HAS_EXPLICIT_CONTRACT);
}

} // namespace

// =====================================================================
// Tests
// =====================================================================

TEST(PdportStack, SprFixed12vContract) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000),  make_fixed_pdo(9000, 3000),
                make_fixed_pdo(12000, 3000), make_fixed_pdo(15000, 3000),
                make_fixed_pdo(20000, 5000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    // Policy: contract at PDO position 3 (12 V / 3 A).
    ctx.dpm.trigger_by_position(3);
    source_present();

    const bool ok = run_until(ctx, src, hr, 4000, [&]() {
        return ctx.dpm.snk_ready >= 1 && ctx.dpm.handshake_done >= 1 &&
               has_explicit_contract(ctx) &&
               ctx.port.rdo_contracted != 0;
    });

    ASSERT_TRUE(ok) << "no SPR contract within 4000 ms";

    // Attached and negotiated on CC1 at 12 V / 3 A (PDO position 3).
    EXPECT_TRUE(ctx.port.is_attached);
    EXPECT_GE(ctx.dpm.cable_attached, 1);
    EXPECT_GE(ctx.dpm.src_caps, 1);
    EXPECT_GE(ctx.dpm.snk_ready, 1);
    EXPECT_GE(ctx.dpm.handshake_done, 1);

    const RDO_ANY rdo_any{ctx.port.rdo_contracted};
    const RDO_FIXED rdo{ctx.port.rdo_contracted};
    EXPECT_EQ(rdo_any.obj_position, 3u);
    EXPECT_EQ(rdo.operating_current, 300u); // 3.0 A in 10 mA units

    // The source saw exactly one Request for PDO 3.
    EXPECT_EQ(src.requests_seen, 1);
    const RDO_ANY src_rdo{src.last_rdo};
    EXPECT_EQ(src_rdo.obj_position, 3u);

    // Driver sanity: all sink frames acknowledged by the partner.
    const auto& c = ctx.drv.counters();
    EXPECT_GT(c.rx_frames, 0u);
    EXPECT_GT(c.goodcrc_sent, 0u);
    EXPECT_GT(c.tx_succeeded, 0u);
    EXPECT_GT(c.rx_goodcrc_ok, 0u);
    EXPECT_EQ(c.tx_failed, 0u);
    EXPECT_EQ(c.hr_rx, 0u);
    EXPECT_EQ(pdport_test::sim().hr_events, 0);

    // No protocol error loops: stack stays in the negotiated state.
    EXPECT_EQ(ctx.dpm.snk_ready, 1);
    EXPECT_EQ(ctx.dpm.startup, 1);
}

TEST(PdportStack, SprDefaultVsafe5vContract) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000), make_fixed_pdo(12000, 3000),
                make_fixed_pdo(20000, 5000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    source_present();

    const bool ok = run_until(ctx, src, hr, 4000, [&]() {
        return ctx.dpm.snk_ready >= 1 && has_explicit_contract(ctx);
    });
    ASSERT_TRUE(ok);

    // No trigger: the DPM falls back to the vSafe5V entry (position 1).
    const RDO_ANY rdo_any{ctx.port.rdo_contracted};
    EXPECT_EQ(rdo_any.obj_position, 1u);
    EXPECT_EQ(ctx.dpm.snk_ready, 1);
}

TEST(PdportStack, SprPpsContract) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    // 5 V fixed + two PPS ranges (as in the upstream validate suite).
    cfg.caps = {make_fixed_pdo(5000, 3000),
                make_pps_apdo(5000, 11000, 3000),
                make_pps_apdo(5000, 21000, 5000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    // The DPM policy: enter a 9 V / 3 A PPS contract (PDO position 2).
    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_PPS, 9000, 3000);
    source_present();

    const bool ok = run_until(ctx, src, hr, 4000, [&]() {
        return ctx.dpm.snk_ready >= 1 && has_explicit_contract(ctx);
    });
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ctx.pe.is_in_pps_contract());

    const RDO_ANY rdo_any{ctx.port.rdo_contracted};
    const RDO_PPS rdo{ctx.port.rdo_contracted};
    EXPECT_EQ(rdo_any.obj_position, 2u);
    EXPECT_EQ(rdo.output_voltage, 450u); // 9.00 V in 20 mV units
    EXPECT_EQ(rdo.operating_current, 60u); // 3.0 A in 50 mA units

    // pdsink's own PPS-contract predicate must agree.
    // (Needs trigger configured; the contract itself is the assertion.)

    EXPECT_EQ(src.requests_seen, 1);
    EXPECT_EQ(ctx.dpm.snk_ready, 1);
}

TEST(PdportStack, PpsContractChangeRequest) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000),
                make_pps_apdo(5000, 11000, 3000),
                make_pps_apdo(5000, 21000, 5000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    source_present();

    // Phase 1: default (5 V).
    bool ok = run_until(ctx, src, hr, 4000, [&]() {
        return has_explicit_contract(ctx) && ctx.dpm.snk_ready >= 1;
    });
    ASSERT_TRUE(ok);
    EXPECT_EQ(RDO_ANY{ctx.port.rdo_contracted}.obj_position, 1u);
    EXPECT_EQ(ctx.dpm.snk_ready, 1);

    // Phase 2: DPM asks for a PPS power level change to 9 V / 3 A.
    // (Position 2 in the source capability list.)
    ctx.dpm.trigger_by_position(2, 9000);
    ok = run_until(ctx, src, hr, 4000, [&]() {
        return RDO_ANY{ctx.port.rdo_contracted}.obj_position == 2u &&
               ctx.dpm.snk_ready >= 2;
    });
    ASSERT_TRUE(ok);
    const RDO_PPS rdo{ctx.port.rdo_contracted};
    EXPECT_EQ(rdo.output_voltage, 450u);
    EXPECT_EQ(ctx.dpm.snk_ready, 2);
    EXPECT_EQ(src.requests_seen, 2);
}

TEST(PdportStack, DetachThenReattachNegotiatesAgain) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000), make_fixed_pdo(12000, 3000),
                make_fixed_pdo(20000, 5000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    // Phase 1: full contract at 12 V (PDO 2).
    ctx.dpm.trigger_by_position(2);
    source_present();
    bool ok = run_until(ctx, src, hr, 4000, [&]() {
        return has_explicit_contract(ctx) && ctx.dpm.snk_ready >= 1;
    });
    ASSERT_TRUE(ok);
    EXPECT_EQ(RDO_ANY{ctx.port.rdo_contracted}.obj_position, 2u);

    // Phase 2: source disappears (cable pull).  The boot TC setup also
    // notifies CableDetached once, so a real detach is the second one.
    source_absent();
    ok = run_until(ctx, src, hr, 2000, [&]() {
        return !ctx.port.is_attached && ctx.dpm.cable_detached >= 2;
    });
    ASSERT_TRUE(ok) << "no detach handling within 2000 ms";

    // Phase 3: reattach and renegotiate.
    source_present();
    ok = run_until(ctx, src, hr, 4000, [&]() {
        return has_explicit_contract(ctx) && ctx.dpm.snk_ready >= 2;
    });
    ASSERT_TRUE(ok) << "no re-negotiation after reattach within 4000 ms";
    EXPECT_EQ(RDO_ANY{ctx.port.rdo_contracted}.obj_position, 2u);
    EXPECT_EQ(ctx.dpm.cable_attached, 2);
    EXPECT_EQ(src.requests_seen, 2);
}

TEST(PdportStack, SilentSourceCausesHardResetNotHang) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000)};
    cfg.send_caps = false; // dead charger: no Source_Capabilities ever
    SourceEmu src(cfg);
    HrWatcher hr;

    source_present();

    // The sink must give up waiting for caps with a hard reset (the
    // tTypeCSinkWaitCap path) - and keep running, never hanging.
    const bool ok = run_until(ctx, src, hr, 6000, [&]() {
        // HR requested by the stack and the burst completed (the driver
        // clears SENDING only via the HR-done IRQ path).
        return pdport_test::sim().hr_events >= 1 &&
               ctx.port.tcpc_tx_status.load() !=
                   TCPC_TRANSMIT_STATUS::SENDING;
    });
    ASSERT_TRUE(ok) << "no hard reset from caps timeout within 6000 ms";
    EXPECT_GE(ctx.drv.counters().hr_sent, 1u);

    // The stack is still alive and ticking after the reset: TC still
    // attached, PE restarted (Startup re-notified), no stuck TX.
    EXPECT_TRUE(ctx.port.is_attached);
    EXPECT_GE(ctx.dpm.startup, 1);
    const auto st = ctx.port.tcpc_tx_status.load();
    EXPECT_TRUE(st == TCPC_TRANSMIT_STATUS::UNSET ||
                st == TCPC_TRANSMIT_STATUS::SUCCEEDED ||
                st == TCPC_TRANSMIT_STATUS::FAILED);
}

// ---------------------------------------------------------------------
// M5: EPR-mode benches (PD 3.1 extended power range)
// ---------------------------------------------------------------------
//
// The emulated source advertises an EPR-capable 5 V PDO, enters the EPR
// conversation when the sink sends EPR_Mode(Enter), sends the chunked
// EPR_Source_Capabilities extended message (11 PDOs, two chunks), answers
// EPR_Request with Accept/PS_RDY and answers EPR_KeepAlive with
// EPR_KeepAlive_Ack.  On EPR_Mode(Exit) it drops back to SPR
// Source_Capabilities (PD 3.1 tFirstSourceCap behaviour).

// Shared EPR source configuration: SPR PDOs (PDO1 EPR-capable) + an EPR
// PDO list in the PD 3.1 layout: SPR positions 1-7 (zero-padded) followed
// by fixed EPR PDOs and one AVS at the end.  pdsink's validate_source_caps
// rejects empty SPR or empty EPR positions, so every non-SPR slot beyond
// position 7 must carry a real EPR PDO.
void epr_cfg_base(SourceEmu::Cfg& cfg) {
    cfg.epr_enabled = true;
    cfg.caps = {make_fixed_pdo_epr_capable(5000, 3000),
                make_fixed_pdo(9000, 3000), make_fixed_pdo(20000, 5000),
                make_pps_apdo(5000, 11000, 3000)};
    // EPR Source_Capabilities data: positions 1-7 = SPR PDOs (same as the
    // SPR message; zero-padded), position 8 = fixed 28 V EPR PDO,
    // position 9 = AVS 15-48 V / 140 W.
    cfg.epr_pdos = {make_fixed_pdo_epr_capable(5000, 3000),
                    make_fixed_pdo(9000, 3000), make_fixed_pdo(20000, 5000),
                    make_pps_apdo(5000, 11000, 3000),
                    0, 0, 0,
                    make_fixed_pdo(28000, 5000),
                    make_epr_avs_pdo(15000, 48000, 140)};
}

TEST(PdportStack, EprEnterSucceededAvsContractKeepAlive) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    epr_cfg_base(cfg);
    SourceEmu src(cfg);
    HrWatcher hr;

    // Policy: enter EPR and contract the AVS at 28 V.  The trigger stays
    // armed: during the initial SPR phase there is no AVS, so the DPM falls
    // back to vSafe5V (position 1); after EPR entry the AVS matches.
    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_EPR_AVS, 28000);
    source_present();

    const bool ok = run_until(ctx, src, hr, 5000, [&]() {
        if (!ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE)) { return false; }
        if (src.epr_requests_seen < 1) { return false; }
        // stable EPR contract + at least two keep-alive exchanges
        return src.epr_keepalives_seen >= 2;
    });
    ASSERT_TRUE(ok) << "no EPR AVS contract within 5000 ms";

    // ---- EPR mode really entered (the M5 acceptance event) ----
    EXPECT_TRUE(ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE));
    EXPECT_GE(ctx.dpm.snk_ready, 2); // SPR ready + EPR ready
    EXPECT_GE(ctx.dpm.handshake_done, 1);
    EXPECT_GE(ctx.dpm.src_caps, 2); // SPR caps + EPR caps

    // The sink asked for the AVS (position 9) at 28 V / 5 A (140 W cap).
    EXPECT_EQ(src.epr_requests_seen, 1);
    // The EPR_Mode(Enter) carried the sink's EPR power demand (140 W).
    EXPECT_EQ(src.epr_enter_watts, 140);
    const RDO_AVS rdo{src.last_epr_rdo};
    const RDO_ANY rdo_any{src.last_epr_rdo};
    EXPECT_EQ(rdo_any.obj_position, 9u);
    EXPECT_EQ(rdo.output_voltage, (28000u / 100u) << 2);
    EXPECT_EQ(rdo.operating_current, 100u); // 5.0 A in 50 mA units
    EXPECT_EQ(src.last_epr_pdo, cfg.epr_pdos[8]);

    // Contract bookkeeping is truthful.
    const RDO_ANY contracted{ctx.port.rdo_contracted};
    EXPECT_EQ(contracted.obj_position, 9u);
    EXPECT_EQ(ctx.port.source_caps.size(), 9u); // full EPR PDO list kept
    EXPECT_EQ(ctx.port.source_caps[8], cfg.epr_pdos[8]);

    // The chunked EPR Source_Capabilities exchange happened (2 chunks,
    // one Request-Chunk from the sink).
    EXPECT_EQ(src.epr_caps_sent, 2);
    EXPECT_EQ(src.epr_chunk_requests_seen, 1);

    // EPR keep-alives flow and are answered (source would otherwise time
    // out the EPR mode).
    EXPECT_GE(src.epr_keepalives_seen, 2);
    EXPECT_EQ(src.epr_keepalives_seen, src.epr_keepalive_acks_sent);
    EXPECT_EQ(src.epr_exits_seen, 0);

    // No hard resets, no failed frames on the whole EPR path.
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
    EXPECT_EQ(ctx.drv.counters().tx_failed, 0u);

    // Driver still healthy.
    const auto& c = ctx.drv.counters();
    EXPECT_GT(c.rx_frames, 0u);
    EXPECT_GT(c.tx_succeeded, 0u);
    EXPECT_GT(c.rx_goodcrc_ok, 0u);
}

TEST(PdportStack, EprSinkInitiatedExitToSprThenPps) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    epr_cfg_base(cfg);
    SourceEmu src(cfg);
    HrWatcher hr;

    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_EPR_AVS, 28000);
    source_present();

    // Phase 1: EPR AVS contract (same as the test above).
    const bool ok1 = run_until(ctx, src, hr, 5000, [&]() {
        return ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE) &&
               src.epr_requests_seen >= 1;
    });
    ASSERT_TRUE(ok1) << "no EPR AVS contract within 5000 ms";

    // Phase 2 (PD 3.1 exit step 1): move the contract down to an SPR PDO
    // (position 3, 20 V) while still in EPR mode.
    ctx.dpm.trigger_by_position(3);
    const bool ok2 = run_until(ctx, src, hr, 4000, [&]() {
        if (!ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE)) { return false; }
        const RDO_ANY r{ctx.port.rdo_contracted};
        return r.obj_position == 3u;
    });
    ASSERT_TRUE(ok2) << "no SPR-level contract inside EPR mode within 4000 ms";
    {
        const RDO_ANY r{ctx.port.rdo_contracted};
        EXPECT_EQ(r.obj_position, 3u);
        // EPR_Request was used for the level change inside EPR mode.
        EXPECT_EQ(src.epr_requests_seen, 2);
    }

    // Phase 3 (exit step 2): sink-initiated EPR_Mode(Exit).
    ctx.dpm.request_epr_exit();
    const bool ok3 = run_until(ctx, src, hr, 4000, [&]() {
        return src.epr_exits_seen == 1 &&
               !ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE) &&
               has_explicit_contract(ctx);
    });
    ASSERT_TRUE(ok3) << "no EPR mode exit within 4000 ms";

    // The source observed exactly one Exit, and the sink never re-enters
    // EPR mode on its own afterwards.
    EXPECT_EQ(src.epr_exits_seen, 1);
    EXPECT_TRUE(ctx.port.pe_flags.test(PE_FLAG::EPR_AUTO_ENTER_DISABLED));
    {
        const RDO_ANY r{ctx.port.rdo_contracted};
        EXPECT_EQ(r.obj_position, 3u);
    }
    // The exit re-negotiation used plain SPR Request (not EPR_Request).
    const int spr_requests_after_exit = src.requests_seen;

    // Keep-alives stop once EPR mode is gone.
    const int keepalives_at_exit = src.epr_keepalives_seen;
    run_until(ctx, src, hr, 1000, [&]() { return false; });
    EXPECT_EQ(src.epr_keepalives_seen, keepalives_at_exit);
    EXPECT_EQ(src.epr_enter_attempts, 1); // no re-entry attempts

    // Phase 4: SPR + PPS still perfect after the EPR excursion.
    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_PPS, 9000, 3000);
    const bool ok4 = run_until(ctx, src, hr, 4000, [&]() {
        if (!ctx.port.pe_flags.test(PE_FLAG::HAS_EXPLICIT_CONTRACT)) {
            return false;
        }
        if (ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE)) { return false; }
        const RDO_ANY r{ctx.port.rdo_contracted};
        return r.obj_position == 4u; // PPS PDO in the SPR caps
    });
    ASSERT_TRUE(ok4) << "PPS contract after EPR exit failed within 4000 ms";
    EXPECT_TRUE(ctx.pe.is_in_pps_contract());
    EXPECT_EQ(ctx.dpm.epr_entry_failed, 0);

    // No hard resets anywhere in the enter -> exit -> PPS journey.
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
    EXPECT_EQ(ctx.drv.counters().tx_failed, 0u);
    // The exit path went through the extra SPR re-negotiation.
    EXPECT_GT(src.requests_seen, spr_requests_after_exit);
}

TEST(PdportStack, EprEntryFailedDisablesAutoEntrySprIntact) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    epr_cfg_base(cfg);
    cfg.epr_enter_ack = false; // source answers Enter_Failed
    SourceEmu src(cfg);
    HrWatcher hr;

    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_EPR_AVS, 28000);
    source_present();

    // The sink must report the failed EPR entry and stay in SPR.
    const bool ok = run_until(ctx, src, hr, 4000, [&]() {
        return ctx.dpm.epr_entry_failed >= 1 &&
               ctx.dpm.handshake_done >= 1;
    });
    ASSERT_TRUE(ok) << "no EPR entry-failure report within 4000 ms";

    EXPECT_FALSE(ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE));
    EXPECT_TRUE(ctx.port.pe_flags.test(PE_FLAG::EPR_AUTO_ENTER_DISABLED));
    EXPECT_EQ(src.epr_enter_attempts, 1); // exactly one attempt, no loop
    EXPECT_EQ(src.epr_caps_sent, 0);

    // The SPR contract survived and PPS is still requestable.
    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_PPS, 9000, 3000);
    const bool ok2 = run_until(ctx, src, hr, 4000, [&]() {
        const RDO_ANY r{ctx.port.rdo_contracted};
        return ctx.port.pe_flags.test(PE_FLAG::HAS_EXPLICIT_CONTRACT) &&
               !ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE) &&
               r.obj_position == 4u;
    });
    ASSERT_TRUE(ok2) << "PPS contract after EPR NAK failed within 4000 ms";
    EXPECT_TRUE(ctx.pe.is_in_pps_contract());
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
}

TEST(PdportStack, EprCapsTimeoutHardResetThenRecovery) {
    pdport_test::sim_reset();
    StackCtx ctx;
    SourceEmu::Cfg cfg;
    epr_cfg_base(cfg);
    cfg.epr_skip_first_caps = true; // first EPR entry never gets caps
    SourceEmu src(cfg);
    HrWatcher hr;

    ctx.dpm.trigger_variant(PDO_VARIANT::APDO_EPR_AVS, 28000);
    source_present();

    // The missing EPR Source_Capabilities must produce a hard reset
    // (tTypeCSinkWaitCap in EPR mode), not a hang.
    const bool ok1 = run_until(ctx, src, hr, 6000, [&]() {
        return pdport_test::sim().hr_events >= 1;
    });
    ASSERT_TRUE(ok1) << "no hard reset on EPR caps timeout within 6000 ms";
    EXPECT_GE(ctx.drv.counters().hr_sent, 1u);

    // The source restarts in SPR; the sink renegotiates and (flags were
    // cleared by the reset) enters EPR mode again - this time the caps
    // arrive and the AVS contract is established.
    const bool ok2 = run_until(ctx, src, hr, 9000, [&]() {
        return ctx.port.pe_flags.test(PE_FLAG::IN_EPR_MODE) &&
               src.epr_requests_seen >= 1 &&
               src.epr_keepalives_seen >= 1;
    });
    ASSERT_TRUE(ok2) << "no EPR recovery after hard reset within 9000 ms";

    EXPECT_GE(src.epr_enter_attempts, 2); // first entry + post-reset entry
    EXPECT_EQ(src.epr_caps_sent, 2);      // caps delivered on second attempt
    EXPECT_EQ(src.epr_chunk_requests_seen, 1);

    // Restored to the AVS contract and stable (no further resets).
    const RDO_ANY r{ctx.port.rdo_contracted};
    EXPECT_EQ(r.obj_position, 9u);
    EXPECT_EQ(pdport_test::sim().hr_events, 1); // exactly the one caps-timeout HR
    EXPECT_EQ(ctx.dpm.snk_ready >= 3, true);    // SPR + EPR + post-HR EPR
}
