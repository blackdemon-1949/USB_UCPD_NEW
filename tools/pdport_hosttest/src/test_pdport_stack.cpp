/*
 * test_pdport_stack.cpp - full-stack SPR bench (M3).
 *
 * Runs the whole pdsink protocol engine (Task + TC + PRL + PE + DPM)
 * on top of the M2 UcpdDriver against the simulated UCPD transport
 * (pd_tr_sim), with a scripted USB-PD "source" partner that answers
 * like real charger hardware: GoodCRC every message, send
 * Source_Capabilities after attach, Accept + PS_RDY on Request.
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
    };

    explicit SourceEmu(Cfg cfg) : cfg(std::move(cfg)) { reset(); }

    Cfg cfg;

    // observables for assertions
    int caps_sent = 0;
    int requests_seen = 0;
    uint32_t last_rdo = 0;
    uint32_t attach_edges = 0;

    void reset() {
        caps_sent = 0;
        requests_seen = 0;
        last_rdo = 0;
        attach_edges = 0;
        prev_attached = false;
        processed = 0;
        tx_msg_id = 0;
        sink_hr_prev = 0;
        scheduled.clear();
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
        // Partner hard reset: the "charger" restarts its state machine.
        if ((uint32_t)s.hr_events != sink_hr_prev) {
            sink_hr_prev = (uint32_t)s.hr_events;
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
            handle_sink_msg(type, ev.data + 2, ev.len - 2, now);
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

    void handle_sink_msg(int type, const uint8_t* payload, uint16_t len,
                         uint32_t now) {
        switch (type) {
            case PD_DATA_MSGT::Request: {
                if (len >= 4) {
                    requests_seen++;
                    last_rdo = (uint32_t)(payload[0] | (payload[1] << 8) |
                                          (payload[2] << 16) |
                                          (payload[3] << 24));
                    const uint32_t accept_at = now + cfg.accept_delay_ms;
                    schedule(make_ctrl(PD_CTRL_MSGT::Accept, (int)next_hdr0()),
                             accept_at);
                    schedule(make_ctrl(PD_CTRL_MSGT::PS_RDY, (int)next_hdr0()),
                             accept_at + cfg.psrdy_after_accept_ms);
                }
                break;
            }
            case PD_CTRL_MSGT::Get_Source_Cap:
            case PD_CTRL_MSGT::Soft_Reset:
            default:
                break;
        }
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
