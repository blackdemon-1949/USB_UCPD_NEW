/*
 * pd_tr_src.hpp - scripted USB-PD "source partner" for the full-stack
 * host benches (M3 SPR / M5 EPR / M4-app glue suites).
 *
 * The source emulator answers like real charger hardware on top of the
 * simulated UCPD transport (pd_tr_sim): GoodCRC every message,
 * Source_Capabilities after attach, Accept + PS_RDY on Request, and the
 * PD 3.1 EPR conversation (EPR_Mode Enter_Acknowledged/Enter_Succeeded,
 * a chunked EPR_Source_Capabilities extended message answered on the
 * sink's Request-Chunk, EPR_KeepAlive_Ack, and EPR_Mode(Exit) answered
 * with SPR Source_Capabilities).
 *
 * Header-only so every bench TU that scripts a source can use the same
 * partner without duplicating the frame formats.
 */
#ifndef PD_TR_SRC_HPP
#define PD_TR_SRC_HPP

#include <stdint.h>

#include <vector>

#include "data_objects.h"
#include "pd_tr_sim.hpp"

namespace pdport_src {

// ---------------------------------------------------------------------
// PDO helpers (same bit-model as the pdsink validate suite)
// ---------------------------------------------------------------------

inline uint32_t make_fixed_pdo(uint32_t voltage_mv, uint32_t current_ma) {
    pd::PDO_FIXED pdo{};
    pdo.pdo_type = pd::PDO_TYPE::FIXED;
    pdo.voltage = voltage_mv / 50;
    pdo.max_current = current_ma / 10;
    return pdo.raw_value;
}

inline uint32_t make_pps_apdo(uint32_t min_voltage_mv, uint32_t max_voltage_mv,
                              uint32_t current_ma) {
    pd::PDO_SPR_PPS pdo{};
    pdo.pdo_type = pd::PDO_TYPE::AUGMENTED;
    pdo.apdo_subtype = pd::PDO_AUGMENTED_SUBTYPE::SPR_PPS;
    pdo.min_voltage = min_voltage_mv / 100;
    pdo.max_voltage = max_voltage_mv / 100;
    pdo.max_current = current_ma / 50;
    return pdo.raw_value;
}

// 5 V fixed PDO that additionally advertises EPR mode capability (PD 3.1
// fixed PDO bit 25).  Required on PDO1 of an EPR-capable source.
inline uint32_t make_fixed_pdo_epr_capable(uint32_t voltage_mv,
                                           uint32_t current_ma) {
    const uint32_t raw = make_fixed_pdo(voltage_mv, current_ma);
    pd::PDO_FIXED pdo{raw};
    pdo.epr_capable = 1;
    return pdo.raw_value;
}

// EPR AVS (adjustable voltage supply) source PDO: min/max voltage in mV,
// PDP in watts (PD 3.1 table 6-15).  Sits at the last position of the
// EPR_Source_Capabilities list (after the fixed EPR PDOs).
inline uint32_t make_epr_avs_pdo(uint32_t min_voltage_mv,
                                 uint32_t max_voltage_mv,
                                 uint32_t pdp_watts) {
    pd::PDO_EPR_AVS pdo{};
    pdo.pdo_type = pd::PDO_TYPE::AUGMENTED;
    pdo.apdo_subtype = pd::PDO_AUGMENTED_SUBTYPE::EPR_AVS;
    pdo.min_voltage = min_voltage_mv / 100;
    pdo.max_voltage = max_voltage_mv / 100;
    pdo.pdp = pdp_watts;
    return pdo.raw_value;
}

// SPR AVS source PDO (USB PD 3.2, table 6.14): an adjustable-voltage
// object in the Standard Power Range with two current windows
// (9-15 V and 15-20 V).  Both currents in mA.
inline uint32_t make_spr_avs_pdo(uint32_t current_9_15_ma,
                                 uint32_t current_15_20_ma) {
    pd::PDO_SPR_AVS pdo{};
    pdo.pdo_type = pd::PDO_TYPE::AUGMENTED;
    pdo.apdo_subtype = pd::PDO_AUGMENTED_SUBTYPE::SPR_AVS;
    pdo.max_current_15v = current_9_15_ma / 10;
    pdo.max_current_20v = current_15_20_ma / 10;
    return pdo.raw_value;
}

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
            if (type == (int)pd::PD_CTRL_MSGT::GoodCRC) { continue; }
            // Echo GoodCRC for every other frame.
            schedule(make_ctrl(pd::PD_CTRL_MSGT::GoodCRC, id), now + 1);
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
                                     pd::PD_REVISION::REV30, nobj, 0);
    }

    std::vector<uint8_t> make_caps() {
        std::vector<uint8_t> payload;
        for (uint32_t pdo : cfg.caps) {
            payload.push_back((uint8_t)(pdo & 0xFFu));
            payload.push_back((uint8_t)((pdo >> 8) & 0xFFu));
            payload.push_back((uint8_t)((pdo >> 16) & 0xFFu));
            payload.push_back((uint8_t)((pdo >> 24) & 0xFFu));
        }
        return make_frame(next_hdr((uint8_t)pd::PD_DATA_MSGT::Source_Capabilities,
                                   (uint8_t)cfg.caps.size()),
                          payload);
    }

    std::vector<uint8_t> make_ctrl(pd::PD_CTRL_MSGT::Type type, int id) {
        const uint16_t hdr =
            pdport_test::make_hdr((uint8_t)type, (uint8_t)id,
                                  pd::PD_REVISION::REV30, 0, 0);
        return make_frame(hdr, {});
    }

    void handle_sink_msg(uint16_t hdr_raw, const uint8_t* payload,
                         uint16_t len, uint32_t now) {
        pd::PD_HEADER hdr{hdr_raw};
        const int type = hdr.message_type;

        // ---- extended messages (incl. chunked) ----
        if (hdr.extended) {
            handle_sink_ext(hdr, payload, len, now);
            return;
        }

        switch (type) {
            case pd::PD_DATA_MSGT::Request:
            case pd::PD_DATA_MSGT::EPR_Request: {
                if (len < 4) { break; }
                const uint32_t rdo =
                    (uint32_t)(payload[0] | (payload[1] << 8) |
                               (payload[2] << 16) | (payload[3] << 24));
                if (type == pd::PD_DATA_MSGT::EPR_Request) {
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
                schedule(make_ctrl(pd::PD_CTRL_MSGT::Accept, (int)next_hdr0()),
                         accept_at);
                schedule(make_ctrl(pd::PD_CTRL_MSGT::PS_RDY, (int)next_hdr0()),
                         accept_at + cfg.psrdy_after_accept_ms);
                break;
            }
            case pd::PD_DATA_MSGT::EPR_Mode: {
                if (len >= 4) {
                    const uint32_t dobj =
                        (uint32_t)(payload[0] | (payload[1] << 8) |
                                   (payload[2] << 16) | (payload[3] << 24));
                    // EPRMDO: action in bits 24..31, data in bits 16..23
                    const int action = (int)((dobj >> 24) & 0xFFu);
                    if (action == (int)pd::EPR_MODE_ACTION::ENTER) {
                        on_epr_enter(dobj, now);
                    } else if (action == (int)pd::EPR_MODE_ACTION::EXIT) {
                        on_epr_exit(now);
                    }
                }
                break;
            }
            case pd::PD_CTRL_MSGT::Get_Source_Cap:
            case pd::PD_CTRL_MSGT::Soft_Reset:
            default:
                break;
        }
    }

    // Extended message from the sink: EPR_KeepAlive or a Request-Chunk for
    // our multi-chunk EPR Source_Capabilities.
    void handle_sink_ext(const pd::PD_HEADER& hdr, const uint8_t* payload,
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
        if (hdr.message_type == (int)pd::PD_EXT_MSGT::Extended_Control &&
            len >= 4)
        {
            const int ecdb_type = payload[2];
            if (ecdb_type == (int)pd::PD_EXT_CTRL_MSGT::EPR_KeepAlive) {
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
            schedule(make_epr_mode(pd::EPR_MODE_ACTION::ENTER_FAILED),
                     now + cfg.epr_ack_delay_ms);
            return;
        }

        schedule(make_epr_mode(pd::EPR_MODE_ACTION::ENTER_ACKNOWLEDGED),
                 now + cfg.epr_ack_delay_ms);
        schedule(make_epr_mode(pd::EPR_MODE_ACTION::ENTER_SUCCEEDED),
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
        return make_frame(next_hdr(pd::PD_DATA_MSGT::EPR_Mode, 1), p);
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
        pd::PD_EXT_HEADER eh{};
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
            pd::PD_EXT_MSGT::EPR_Source_Capabilities,
            (uint8_t)(tx_msg_id++ & 0x7), pd::PD_REVISION::REV30,
            (uint8_t)(p.size() / 4), 1);
        return make_frame(hdr, p);
    }

    std::vector<uint8_t> make_keepalive_ack() {
        std::vector<uint8_t> p;
        pd::PD_EXT_HEADER eh{};
        eh.data_size = 2;
        eh.chunked = 1;
        p.push_back((uint8_t)(eh.raw_value & 0xFFu));
        p.push_back((uint8_t)(eh.raw_value >> 8));
        // ECDB: type = EPR_KeepAlive_Ack (4)
        p.push_back(pd::PD_EXT_CTRL_MSGT::EPR_KeepAlive_Ack);
        p.push_back(0);
        const uint16_t hdr = pdport_test::make_hdr(
            pd::PD_EXT_MSGT::Extended_Control, (uint8_t)(tx_msg_id++ & 0x7),
            pd::PD_REVISION::REV30, 1, 1);
        return make_frame(hdr, p);
    }

    // id helper: allocate a message id without constructing a frame
    uint8_t next_hdr0() { return (uint8_t)(tx_msg_id++ & 0x7); }
};

} // namespace pdport_src

#endif /* PD_TR_SRC_HPP */
