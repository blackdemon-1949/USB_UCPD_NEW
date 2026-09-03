/*
 * test_pdport_app.cpp - M4-app board-glue suite.
 *
 * Exercises the exact C seam the application modules will call on the
 * pdsink path (pdport_app.h, implemented by pdport_app.cpp) against the
 * full stack + simulated transport, with the same scripted PD 3.1 EPR
 * source partner as the M5 bench (pd_tr_src.hpp):
 *
 *   - pdport_init/service + status snapshot during attach/contract,
 *   - the request engine (position / PPS) re-pointed from the CLI,
 *   - EPR control with truthful return codes: auto entry, AVS request,
 *     two-step sink-initiated exit, refusal paths (no EPR source /
 *     nothing attached), re-entry after exit,
 *   - hard-reset recovery when the EPR source stalls its capabilities,
 *     and the guarantee that the C API keeps answering afterwards.
 *
 * The pdsink graph lives in pdport_app.cpp itself (like on the board);
 * this suite drives it through the same extern "C" functions the
 * firmware will call - not through internal objects.
 */
#include <gtest/gtest.h>

#include <stdint.h>

#include <cstring>
#include <functional>
#include <vector>

#include "data_objects.h"
#include "pd_tr_sim.hpp"
#include "pd_tr_src.hpp"
#include "pdport_app.h"

#include "pd_ucpd_driver.h"  // pd_drv_on_hr_done (HR completion, IRQ eq.)

using pdport_src::make_fixed_pdo;
using pdport_src::make_fixed_pdo_epr_capable;
using pdport_src::make_pps_apdo;
using pdport_src::make_epr_avs_pdo;
using pdport_src::SourceEmu;

namespace {

// Event sink: counts PDPORT_EV_* notifications (like an app module).
struct EvSink {
    uint32_t counts[32] = {0};
    void on(uint32_t ev) {
        if (ev >= 50u && (ev - 50u) < 32u) { counts[ev - 50u]++; }
    }
};

EvSink g_ev;
void ev_cb(uint32_t ev, void* arg) {
    EvSink* s = static_cast<EvSink*>(arg);
    if (s) { s->on(ev); }
}

// Hard-reset burst completion (the UCPD IRQ equivalent on the bench).
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

void step_ms(SourceEmu& src, HrWatcher& hr) {
    src.tick();
    pdport_test::sim_advance_ms(1);
    hr.poll();
    pdport_service();
}

bool run_until(SourceEmu& src, HrWatcher& hr, uint32_t max_ms,
               const std::function<bool()>& done) {
    const uint32_t t0 = pdport_test::sim().now_ms;
    const uint32_t limit = t0 + max_ms;
    while (pdport_test::sim().now_ms < limit) {
        step_ms(src, hr);
        if (done()) { return true; }
    }
    return false;
}

pdport_status_t status() {
    pdport_status_t st;
    pdport_get_status(&st);
    return st;
}

void source_present() {
    pdport_test::sim_set_cc(PD_CC_RP_3_0, PD_CC_NONE);
    pdport_test::sim_set_vbus(1);
}

void source_absent() {
    pdport_test::sim_set_cc(PD_CC_NONE, PD_CC_NONE);
    pdport_test::sim_set_vbus(0);
}

void expect_event(uint32_t ev_min, uint32_t idx) {
    EXPECT_GE(g_ev.counts[idx], ev_min);
}

// Shared EPR-capable source list (9 slots): SPR 1-4, zero pad 5-7,
// fixed 28 V/5 A at slot 8, AVS 15-48 V/140 W at slot 9.
SourceEmu::Cfg epr_source_cfg() {
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo_epr_capable(5000, 3000),
                make_fixed_pdo(9000, 3000), make_fixed_pdo(20000, 5000),
                make_pps_apdo(5000, 11000, 3000)};
    cfg.epr_enabled = true;
    cfg.epr_pdos = {make_fixed_pdo_epr_capable(5000, 3000),
                    make_fixed_pdo(9000, 3000),
                    make_fixed_pdo(20000, 5000),
                    make_pps_apdo(5000, 11000, 3000),
                    0, 0, 0,
                    make_fixed_pdo(28000, 5000),
                    make_epr_avs_pdo(15000, 48000, 140)};
    return cfg;
}

} // namespace

// =====================================================================
// Tests
// =====================================================================

TEST(PdportApp, InitStatusSprContractThenPpsAndDetach) {
    pdport_test::sim_reset();
    EXPECT_EQ(pdport_init(), 0);
    EXPECT_EQ(pdport_init(), 0); // idempotent

    pdport_set_event_cb(ev_cb, &g_ev);
    memset(g_ev.counts, 0, sizeof(g_ev.counts));

    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000), make_fixed_pdo(9000, 3000),
                make_fixed_pdo(20000, 5000),
                make_pps_apdo(5000, 11000, 3000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    auto st = status();
    EXPECT_EQ(st.initialised, 1u);
    EXPECT_EQ(st.attached, 0u);
    EXPECT_EQ(st.explicit_contract, 0u);

    // Attach: the default DPM policy requests vSafe5V (PDO 1).
    source_present();
    const bool c1 = run_until(src, hr, 4000, [&]() {
        auto s = status();
        return s.attached && s.explicit_contract && s.contract_position == 1 &&
               g_ev.counts[PDPORT_EV_HANDSHAKE_DONE - 50] >= 1u;
    });
    ASSERT_TRUE(c1) << "no 5 V SPR contract";

    st = status();
    EXPECT_EQ(st.vbus_ok, 1u);
    EXPECT_EQ(st.contract_mv, 5000u);
    EXPECT_EQ(st.contract_ma, 3000u);
    EXPECT_EQ(st.in_epr_mode, 0u);
    EXPECT_GT(st.tx_frames, 0u);
    EXPECT_GT(st.rx_frames, 0u);
    EXPECT_EQ(src.requests_seen, 1);
    expect_event(1, PDPORT_EV_CABLE_ATTACHED - 50);
    expect_event(1, PDPORT_EV_HANDSHAKE_DONE - 50);

    // "req 3" parity: fixed PDO 3 = 20 V / 5 A.
    EXPECT_EQ(pdport_request_position(3, 0, 0), 0);
    const bool c2 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.contract_position == 3 && s.contract_mv == 20000u;
    });
    ASSERT_TRUE(c2) << "no 20 V contract after position request";
    EXPECT_EQ(status().contract_ma, 5000u);

    // "pps 9000" parity: PDO 4 is the PPS APDO.
    EXPECT_EQ(pdport_request_pps(9000, 3000), 0);
    const bool c3 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.contract_position == 4 && s.in_pps_contract &&
               s.contract_mv == 9000u;
    });
    ASSERT_TRUE(c3) << "no PPS contract at 9 V";
    EXPECT_EQ(status().contract_ma, 3000u);
    EXPECT_EQ(pdport_test::sim().hr_events, 0);

    // Cable pull: the link drops (the stack keeps the last contract state
    // until the next attach re-initialises the PE - same semantics as the
    // M3 detach bench).
    source_absent();
    const bool c4 = run_until(src, hr, 3000, [&]() {
        return !status().attached &&
               g_ev.counts[PDPORT_EV_CABLE_DETACHED - 50] >= 1u;
    });
    ASSERT_TRUE(c4) << "detach not handled within 3000 ms";

    // Reattach: renegotiates on its own, keeping the PPS trigger (the DPM
    // trigger state survives, like the M3 reattach bench), no hard reset.
    source_present();
    const bool c5 = run_until(src, hr, 4000, [&]() {
        auto s = status();
        return s.attached && s.explicit_contract && s.contract_position == 4 &&
               s.in_pps_contract && src.requests_seen >= 4;
    });
    ASSERT_TRUE(c5) << "no renegotiation to PPS after reattach";
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
}

TEST(PdportApp, EprEnterAvsContractThenTwoStepExitThenPpsThenReenter) {
    pdport_test::sim_reset();
    ASSERT_EQ(pdport_init(), 0);
    pdport_set_event_cb(ev_cb, &g_ev);
    memset(g_ev.counts, 0, sizeof(g_ev.counts));

    SourceEmu src(epr_source_cfg());
    HrWatcher hr;

    // 5 V SPR contract first; the PE auto-enters EPR (PDO1 is EPR-capable).
    source_present();
    const bool c1 = run_until(src, hr, 4000, [&]() {
        auto s = status();
        return s.explicit_contract && s.in_epr_mode && s.epr_source_capable &&
               s.src_caps_count == 9u;
    });
    ASSERT_TRUE(c1) << "EPR mode not entered / EPR caps not received";
    EXPECT_EQ(src.epr_enter_attempts, 1);
    EXPECT_EQ(src.epr_enter_watts, 140); // DPM EPR watts in EPRMDO
    EXPECT_EQ(status().epr_auto_enter, 1u);

    // Request the AVS PDO (28 V, current implied from 140 W).
    EXPECT_EQ(pdport_request_epr_avs(28000, 0), 0);
    const bool c2 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.contract_position == 9 && s.in_epr_mode &&
               s.contract_mv == 28000u && s.contract_ma == 5000u;
    });
    ASSERT_TRUE(c2) << "no EPR AVS contract at 28 V/5 A";
    EXPECT_EQ(src.epr_requests_seen, 2); // 5 V entry + AVS request

    // Keep-alives flow while the EPR contract stands.
    const bool c3 = run_until(src, hr, 2000,
                              [&]() { return src.epr_keepalives_seen >= 2; });
    ASSERT_TRUE(c3) << "no EPR keep-alives on the AVS contract";
    EXPECT_EQ(pdport_test::sim().hr_events, 0);

    // Sink-initiated exit, step 1: must move to an SPR-level contract
    // first (PD 3.1).  The API says so instead of pretending.
    EXPECT_EQ(pdport_epr_exit(), PDPORT_EPR_EXIT_SPR_FIRST);
    const bool c4 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.in_epr_mode && s.explicit_contract &&
               s.contract_position <= 7u; // SPR-level contract inside EPR mode
    });
    ASSERT_TRUE(c4) << "no SPR-level contract after the exit pre-step";

    // Step 2: EPR_Mode(Exit); the source answers with SPR caps and the
    // sink re-contracts in SPR.  Auto entry stays disabled after exit.
    EXPECT_EQ(pdport_epr_exit(), PDPORT_EPR_EXIT_QUEUED);
    const bool c5 = run_until(src, hr, 4000, [&]() {
        auto s = status();
        return !s.in_epr_mode && s.explicit_contract &&
               s.contract_position == 1 && s.contract_mv == 5000u;
    });
    ASSERT_TRUE(c5) << "EPR exit did not return to an SPR 5 V contract";
    EXPECT_EQ(src.epr_exits_seen, 1);
    EXPECT_EQ(status().epr_auto_enter, 0u); // disabled after sink exit
    EXPECT_EQ(pdport_test::sim().hr_events, 0);

    // No EPR keep-alives in SPR mode.
    const int ka = src.epr_keepalives_seen;
    run_until(src, hr, 500, []() { return false; });
    EXPECT_EQ(src.epr_keepalives_seen, ka);

    // PPS still works after the exit (SPR + PPS perfect).
    EXPECT_EQ(pdport_request_pps(9000, 3000), 0);
    const bool c6 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.contract_position == 4 && s.in_pps_contract &&
               !s.in_epr_mode && s.contract_mv == 9000u;
    });
    ASSERT_TRUE(c6) << "no PPS contract after EPR exit";

    // "epr enter" again (no reset): re-enters EPR mode.
    EXPECT_EQ(pdport_epr_enter(), PDPORT_EPR_ENTER_QUEUED);
    const bool c7 = run_until(src, hr, 4000, [&]() {
        auto s = status();
        return s.in_epr_mode && s.src_caps_count == 9u;
    });
    ASSERT_TRUE(c7) << "EPR re-entry after exit failed";
    EXPECT_EQ(src.epr_enter_attempts, 2);
    EXPECT_EQ(status().epr_auto_enter, 1u);
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
}

TEST(PdportApp, EprEntryFailedStaysSprAndPpsStillWorks) {
    pdport_test::sim_reset();
    ASSERT_EQ(pdport_init(), 0);
    pdport_set_event_cb(ev_cb, &g_ev);
    memset(g_ev.counts, 0, sizeof(g_ev.counts));

    SourceEmu::Cfg cfg = epr_source_cfg();
    cfg.epr_enter_ack = false; // source answers Enter_Failed
    SourceEmu src(cfg);
    HrWatcher hr;

    source_present();
    // The sink attempts entry, the source refuses: SPR must stay intact
    // and the auto-enter latch must close (no retry loop).
    const bool c1 = run_until(src, hr, 4000, [&]() {
        return src.epr_enter_attempts >= 1 &&
               status().explicit_contract && !status().in_epr_mode;
    });
    ASSERT_TRUE(c1) << "no EPR entry attempt / SPR lost";
    const bool c1b = run_until(src, hr, 1000, [&]() {
        return g_ev.counts[PDPORT_EV_EPR_ENTRY_FAILED - 50] >= 1u;
    });
    ASSERT_TRUE(c1b) << "EPR entry failure was not reported to the app";

    run_until(src, hr, 1500, []() { return false; });
    EXPECT_EQ(src.epr_enter_attempts, 1); // no retry storm
    EXPECT_EQ(status().epr_auto_enter, 0u);
    EXPECT_EQ(pdport_test::sim().hr_events, 0);

    // PPS requests still work after the failed entry.
    EXPECT_EQ(pdport_request_pps(9000, 3000), 0);
    const bool c2 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.contract_position == 4 && s.in_pps_contract &&
               !s.in_epr_mode;
    });
    ASSERT_TRUE(c2) << "no PPS contract after refused EPR entry";
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
}

TEST(PdportApp, EprCapsStallHardResetsThenRecoversAndApiStaysAlive) {
    pdport_test::sim_reset();
    ASSERT_EQ(pdport_init(), 0);
    pdport_set_event_cb(ev_cb, &g_ev);
    memset(g_ev.counts, 0, sizeof(g_ev.counts));

    SourceEmu::Cfg cfg = epr_source_cfg();
    cfg.epr_skip_first_caps = true; // first EPR caps never arrive
    SourceEmu src(cfg);
    HrWatcher hr;

    source_present();
    // Entry succeeds but the EPR source caps stall: the sink must hard
    // reset and recover on its own, then reach an AVS contract on the
    // second attempt.
    const bool c1 = run_until(src, hr, 9000, [&]() {
        auto s = status();
        return s.in_epr_mode && s.src_caps_count == 9u &&
               s.explicit_contract;
    });
    ASSERT_TRUE(c1) << "no recovery to EPR after the stalled caps";

    EXPECT_GE(src.epr_enter_attempts, 2); // attempt after the HR
    EXPECT_GE((int)pdport_test::sim().hr_events, 1); // sink sent a hard reset
    EXPECT_GE(status().hr_sent, 1u);

    EXPECT_EQ(pdport_request_epr_avs(28000, 0), 0);
    const bool c2 = run_until(src, hr, 3000, [&]() {
        auto s = status();
        return s.contract_position == 9 && s.contract_mv == 28000u &&
               s.contract_ma == 5000u && s.in_epr_mode;
    });
    ASSERT_TRUE(c2) << "no AVS contract after recovery";

    // The C API keeps answering after the whole episode.
    pdport_status_t st;
    pdport_get_status(&st);
    EXPECT_EQ(st.initialised, 1u);
    EXPECT_EQ(pdport_epr_exit(), PDPORT_EPR_EXIT_SPR_FIRST);
}

TEST(PdportApp, RefusalPathsAreTruthful) {
    pdport_test::sim_reset();
    ASSERT_EQ(pdport_init(), 0);
    SourceEmu::Cfg cfg;
    cfg.caps = {make_fixed_pdo(5000, 3000), make_fixed_pdo(9000, 3000)};
    SourceEmu src(cfg);
    HrWatcher hr;

    // Nothing attached: every EPR verb must refuse, not pretend.
    EXPECT_EQ(pdport_epr_enter(), PDPORT_EPR_ENTER_REFUSED);
    EXPECT_EQ(pdport_epr_exit(), PDPORT_EPR_EXIT_NOT_ACTIVE);
    EXPECT_EQ(pdport_request_position(0, 0, 0), -1);

    // SPR-only source: entry refused after a contract too.
    source_present();
    const bool c1 = run_until(src, hr, 4000, [&]() {
        auto s = status();
        return s.explicit_contract && s.contract_position == 1;
    });
    ASSERT_TRUE(c1) << "no 5 V contract";
    EXPECT_EQ(status().epr_source_capable, 0u);
    EXPECT_EQ(pdport_epr_enter(), PDPORT_EPR_ENTER_REFUSED);

    // Position requests beyond the SPR range are refused up front.
    EXPECT_EQ(pdport_request_position(8, 0, 0), -1);
    EXPECT_EQ(pdport_request_any(0, 0), -1);
    EXPECT_EQ(pdport_test::sim().hr_events, 0);
}
