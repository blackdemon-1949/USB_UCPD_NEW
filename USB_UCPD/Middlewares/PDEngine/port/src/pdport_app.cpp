/*
 * pdport_app.cpp - pdsink object graph and C application glue (board).
 *
 * M4-app deliverable: the one new source file the CubeIDE build adds on
 * the pdsink path (see port/README.md, "Board glue").  It owns the
 * single-port pdsink graph (Port + UcpdDriver + TC/PRL/PE/DPM/Task) and
 * exposes the C seam declared in pdport_app.h for the existing C
 * application modules (main.c, app_pd.c, app_epr.c, app_pps.c,
 * app_cli.c).
 *
 * Host-benched: the same file is compiled by tools/pdport_hosttest and
 * exercised end-to-end by test_pdport_app (attach, SPR contract, EPR
 * entry over a scripted PD 3.1 source, AVS request, sink-initiated EPR
 * exit, PPS) so the API contract the app modules will call is proven
 * before the board build.
 *
 * Board configuration macros (compile-time, override as needed):
 *   PDPORT_CEILING_MV   highest voltage this board's VBUS path may be
 *                       asked for (default 28000, matching the app's
 *                       APP_EPR_DEFAULT_CEILING_MV).  The sink EPR PDO
 *                       block (fixed 28 V / AVS window) is derived from
 *                       it.
 *   PDPORT_EPR_WATTS    EPR operating power in watts (default 140 =
 *                       28 V * 5 A, matching DPM::epr_watts).
 */
#include <stdint.h>

#include "dpm.h"
#include "messages.h"
#include "pd_ucpd_driver.h"
#include "pd_tr.h"
#include "pdport_app.h"
#include "pe.h"
#include "port.h"
#include "prl.h"
#include "task.h"
#include "tc.h"
#include "utils/dobj_utils.h"

#ifndef PDPORT_CEILING_MV
#define PDPORT_CEILING_MV 28000u
#endif
#ifndef PDPORT_EPR_WATTS
#define PDPORT_EPR_WATTS 140u
#endif

namespace {

using pd::dobj_utils::get_src_pdo_variant;

pd::Port g_port;
pdport::UcpdDriver g_driver{g_port};

// ---------------------------------------------------------------------
// Board DPM: pdsink default policy + the board's sink capability table.
// ---------------------------------------------------------------------

class BoardDpm : public pd::DPM {
public:
    explicit BoardDpm(pd::Port& port) : pd::DPM(port) {}

    bool has_usb_comm() override { return true; } // CDC console is USB

    pd::PDO_LIST get_sink_pdo_list() override {
        // Cache: the sink demands must not change while the link is up
        // (pdsink dpm.cpp contract).
        if (!sink_pdo_list.empty()) { return sink_pdo_list; }

        // SPR block mirrors the board's declared sink table
        // (Appli/USBPD/App/usbpd_pdo_defs.h PORT0_PDO_ListSNK):
        // 5/9/12/15/20 V fixed + PPS 3.3-21 V.
        using pd::PDO_VARIANT;
        using pd::dobj_utils::set_snk_pdo_limits;
        using pd::dobj_utils::PDO_LIMITS;

        pd::SNK_PDO_FIXED p1{};
        p1.pdo_type = pd::PDO_TYPE::FIXED;
        p1.dual_role_power = 0;
        p1.higher_capability = 1;
        p1.unconstrained_power = 1;
        p1.usb_comms_capable = 1;
        p1.dual_role_data = 1; // USB device present (DRD capable)
        set_snk_pdo_limits(p1.raw_value, PDO_LIMITS().set_mv(5000).set_ma(3000));
        sink_pdo_list.push_back(p1.raw_value);

        const struct { uint32_t mv, ma; } spr_fixed[] = {
            {9000, 3000}, {12000, 3000}, {15000, 3000}, {20000, 5000}};
        for (auto& f : spr_fixed) {
            pd::SNK_PDO_FIXED p{};
            p.pdo_type = pd::PDO_TYPE::FIXED;
            set_snk_pdo_limits(p.raw_value, PDO_LIMITS().set_mv(f.mv).set_ma(f.ma));
            sink_pdo_list.push_back(p.raw_value);
        }

        pd::SNK_PDO_SPR_PPS pps{};
        pps.pdo_type = pd::PDO_TYPE::AUGMENTED;
        pps.apdo_subtype = pd::PDO_AUGMENTED_SUBTYPE::SPR_PPS;
        set_snk_pdo_limits(pps.raw_value,
                           PDO_LIMITS().set_mv_min(3300).set_mv_max(21000)
                               .set_ma(3000));
        sink_pdo_list.push_back(pps.raw_value);

        // EPR block starts at slot 8 (slot 7 stays zero-padded; the SPR
        // list above has 6 objects).  The board's front-end ceiling
        // decides what the sink advertises in EPR mode.
        const uint32_t ceiling =
            (PDPORT_CEILING_MV > 48000u) ? 48000u : PDPORT_CEILING_MV;
        while (sink_pdo_list.size() < 7u) { sink_pdo_list.push_back(0); }

        if (ceiling >= 28000u) {
            pd::SNK_PDO_FIXED epr_fixed{};
            epr_fixed.pdo_type = pd::PDO_TYPE::FIXED;
            set_snk_pdo_limits(epr_fixed.raw_value,
                               PDO_LIMITS().set_mv(28000).set_ma(5000));
            sink_pdo_list.push_back(epr_fixed.raw_value);
        } else {
            sink_pdo_list.push_back(0);
        }

        if (ceiling >= 15000u) {
            pd::SNK_PDO_EPR_AVS avs{};
            avs.pdo_type = pd::PDO_TYPE::AUGMENTED;
            avs.apdo_subtype = pd::PDO_AUGMENTED_SUBTYPE::EPR_AVS;
            set_snk_pdo_limits(avs.raw_value,
                               PDO_LIMITS().set_mv_min(15000).set_mv_max(ceiling)
                                   .set_pdp(PDPORT_EPR_WATTS));
            sink_pdo_list.push_back(avs.raw_value);
        } else {
            sink_pdo_list.push_back(0);
        }
        sink_pdo_list.push_back(0); // slot 10 stays free for 36 V fixed
        sink_pdo_list.push_back(0); // slot 11 stays free for 48 V fixed
        return sink_pdo_list;
    }

    uint32_t get_epr_watts() override { return PDPORT_EPR_WATTS; }
};

// ---------------------------------------------------------------------
// DPM notification router -> C event callback
// ---------------------------------------------------------------------

class DpmRouter
    : public etl::message_router<DpmRouter, pd::MsgToDpm_Startup,
                                 pd::MsgToDpm_CableAttached,
                                 pd::MsgToDpm_CableDetached,
                                 pd::MsgToDpm_SrcCapsReceived,
                                 pd::MsgToDpm_SelectCapDone,
                                 pd::MsgToDpm_SnkReady,
                                 pd::MsgToDpm_HandshakeDone,
                                 pd::MsgToDpm_TransitToDefault,
                                 pd::MsgToDpm_NewPowerLevelAccepted,
                                 pd::MsgToDpm_NewPowerLevelRejected,
                                 pd::MsgToDpm_EPREntryFailed, pd::MsgToDpm_Alert,
                                 pd::MsgToDpm_SrcDisabled> {
public:
    DpmRouter()
        : etl::message_router<DpmRouter, pd::MsgToDpm_Startup,
                              pd::MsgToDpm_CableAttached,
                              pd::MsgToDpm_CableDetached,
                              pd::MsgToDpm_SrcCapsReceived,
                              pd::MsgToDpm_SelectCapDone,
                              pd::MsgToDpm_SnkReady,
                              pd::MsgToDpm_HandshakeDone,
                              pd::MsgToDpm_TransitToDefault,
                              pd::MsgToDpm_NewPowerLevelAccepted,
                              pd::MsgToDpm_NewPowerLevelRejected,
                              pd::MsgToDpm_EPREntryFailed, pd::MsgToDpm_Alert,
                              pd::MsgToDpm_SrcDisabled>(1) {}

    void emit(uint32_t ev) {
        if (cb) { cb(ev, arg); }
    }
#define IMPL(MsgT, Ev)                     \
    void on_receive(const MsgT&) { emit(Ev); }
    IMPL(pd::MsgToDpm_Startup, PDPORT_EV_STARTUP)
    IMPL(pd::MsgToDpm_CableAttached, PDPORT_EV_CABLE_ATTACHED)
    IMPL(pd::MsgToDpm_CableDetached, PDPORT_EV_CABLE_DETACHED)
    IMPL(pd::MsgToDpm_SrcCapsReceived, PDPORT_EV_SRC_CAPS_RECEIVED)
    IMPL(pd::MsgToDpm_SelectCapDone, PDPORT_EV_SELECT_CAP_DONE)
    IMPL(pd::MsgToDpm_SnkReady, PDPORT_EV_SNK_READY)
    IMPL(pd::MsgToDpm_HandshakeDone, PDPORT_EV_HANDSHAKE_DONE)
    IMPL(pd::MsgToDpm_TransitToDefault, PDPORT_EV_TRANSIT_TO_DEFAULT)
    IMPL(pd::MsgToDpm_NewPowerLevelAccepted, PDPORT_EV_POWER_ACCEPTED)
    IMPL(pd::MsgToDpm_NewPowerLevelRejected, PDPORT_EV_POWER_REJECTED)
    IMPL(pd::MsgToDpm_EPREntryFailed, PDPORT_EV_EPR_ENTRY_FAILED)
    IMPL(pd::MsgToDpm_Alert, PDPORT_EV_ALERT)
    IMPL(pd::MsgToDpm_SrcDisabled, PDPORT_EV_SRC_DISABLED)
#undef IMPL
    void on_receive_unknown(const etl::imessage&) {}

    pdport_event_cb_t cb = nullptr;
    void* arg = nullptr;
};

BoardDpm g_dpm{g_port};
pd::TC g_tc{g_port, g_driver};
pd::PRL g_prl{g_port, g_driver};
pd::PE g_pe{g_port, g_dpm, g_prl, g_driver};
pd::Task g_task{g_port, g_driver};
DpmRouter g_dpm_router;
bool g_started = false;

// ---------------------------------------------------------------------
// Contract decoding helpers (status snapshot)
// ---------------------------------------------------------------------

void decode_contract(pdport_status_t* st) {
    const auto& caps = g_port.source_caps;
    const uint32_t raw = g_port.rdo_contracted;
    const pd::RDO_ANY rdo_any{raw};

    st->contract_position = rdo_any.obj_position; // 1-based, 0 = none
    if (st->contract_position == 0u ||
        st->contract_position > caps.size())
    {
        return;
    }
    const uint32_t pdo = caps[st->contract_position - 1u];

    switch (get_src_pdo_variant(pdo)) {
        case pd::PDO_VARIANT::FIXED: {
            const pd::RDO_FIXED r{raw};
            const pd::PDO_FIXED f{pdo};
            st->contract_mv = (uint32_t)f.voltage * 50u;
            st->contract_ma = (uint32_t)r.operating_current * 10u;
            break;
        }
        case pd::PDO_VARIANT::APDO_PPS: {
            const pd::RDO_PPS r{raw};
            st->contract_mv = (uint32_t)r.output_voltage * 20u;
            st->contract_ma = (uint32_t)r.operating_current * 50u;
            break;
        }
        case pd::PDO_VARIANT::APDO_EPR_AVS: {
            const pd::RDO_AVS r{raw};
            st->contract_mv = (uint32_t)r.output_voltage * 25u;
            st->contract_ma = (uint32_t)r.operating_current * 50u;
            break;
        }
        default:
            break; // battery/variable profiles are not requested by this DPM
    }
}

void fill_cc_level(pdport_status_t* st) {
    int cc1 = PD_CC_NONE, cc2 = PD_CC_NONE;
    pd_tr_read_cc(&cc1, &cc2);
    const int active = pd_tr_read_active_cc();
    if (active == 1) { st->active_cc = 1u; st->active_cc_level = (uint32_t)cc1; }
    else if (active == 2) { st->active_cc = 2u; st->active_cc_level = (uint32_t)cc2; }
    else { st->active_cc = 0u; st->active_cc_level = PD_CC_NONE; }
}

} // namespace

// ---------------------------------------------------------------------
// extern "C" application seam
// ---------------------------------------------------------------------

extern "C" {

int pdport_init(void)
{
    if (g_started) { return 0; }
    if (pd_tr_init() != 0) { return -1; }
    g_port.dpm_rtr = &g_dpm_router;
    g_task.start(g_tc, g_dpm, g_pe, g_prl, g_driver);
    g_started = true;
    return 0;
}

void pdport_service(void)
{
    if (!g_started) { return; }
    g_driver.service();
}

void pdport_set_event_cb(pdport_event_cb_t cb, void* arg)
{
    g_dpm_router.cb = cb;
    g_dpm_router.arg = arg;
}

void pdport_get_status(pdport_status_t* out)
{
    if (out == nullptr) { return; }

    out->initialised = g_started ? 1u : 0u;
    out->attached = (g_port.is_attached || pd_tr_vbus_ok()) ? 1u : 0u;
    out->vbus_ok = pd_tr_vbus_ok() ? 1u : 0u;
    out->active_cc = 0u;
    out->active_cc_level = PD_CC_NONE;
    if (out->attached) { fill_cc_level(out); }
    out->pe_state = (uint32_t)g_pe.get_state_id();
    out->revision = (uint32_t)g_port.revision;

    out->explicit_contract =
        g_port.pe_flags.test(pd::PE_FLAG::HAS_EXPLICIT_CONTRACT) ? 1u : 0u;
    out->rdo_contracted = g_port.rdo_contracted;
    out->contract_position = 0u;
    out->contract_mv = 0u;
    out->contract_ma = 0u;
    if (out->explicit_contract) { decode_contract(out); }
    out->in_pps_contract = g_pe.is_in_pps_contract() ? 1u : 0u;

    out->in_epr_mode = g_port.pe_flags.test(pd::PE_FLAG::IN_EPR_MODE) ? 1u : 0u;
    out->epr_auto_enter =
        g_port.pe_flags.test(pd::PE_FLAG::EPR_AUTO_ENTER_DISABLED) ? 0u : 1u;
    out->epr_source_capable = 0u;
    if (g_port.revision >= pd::PD_REVISION::REV30 &&
        !g_port.source_caps.empty())
    {
        const pd::PDO_FIXED first{g_port.source_caps[0]};
        if (first.pdo_type == pd::PDO_TYPE::FIXED && first.epr_capable) {
            out->epr_source_capable = 1u;
        }
    }

    out->src_caps_count =
        (uint32_t)g_port.source_caps.size() > PDPORT_MAX_SRC_PDOS
            ? PDPORT_MAX_SRC_PDOS
            : (uint32_t)g_port.source_caps.size();
    for (uint32_t i = 0; i < out->src_caps_count; i++) {
        out->src_caps[i] = g_port.source_caps[i];
    }

    const auto& c = g_driver.counters();
    out->rx_frames = c.rx_frames;
    out->rx_goodcrc = c.rx_goodcrc;
    out->tx_frames = c.tx_frames;
    out->tx_succeeded = c.tx_succeeded;
    out->tx_failed = c.tx_failed;
    out->hr_sent = c.hr_sent;
    out->hr_rx = c.hr_rx;
}

int pdport_request_position(uint32_t position, uint32_t mv, uint32_t ma)
{
    if (position == 0u || position > 7u) { return -1; }
    if (!g_port.pe_flags.test(pd::PE_FLAG::HAS_EXPLICIT_CONTRACT)) {
        // No contract yet: the trigger applies at the next capability
        // handshake (pdsink semantics).
        if (g_port.source_caps.empty() && !g_port.is_attached) { return -1; }
    }
    g_dpm.trigger_by_position((uint8_t)position, mv, ma);
    return 0;
}

int pdport_request_any(uint32_t mv, uint32_t ma)
{
    if (mv == 0u) { return -1; }
    g_dpm.trigger_any(mv, ma);
    return 0;
}

int pdport_request_pps(uint32_t mv, uint32_t ma)
{
    if (mv == 0u) { return -1; }
    g_dpm.trigger_variant(pd::PDO_VARIANT::APDO_PPS, mv, ma);
    return 0;
}

int pdport_request_epr_avs(uint32_t mv, uint32_t ma)
{
    if (mv == 0u) { return -1; }
    g_dpm.trigger_variant(pd::PDO_VARIANT::APDO_EPR_AVS, mv, ma);
    return 0;
}

int pdport_epr_enter(void)
{
    if (g_port.pe_flags.test(pd::PE_FLAG::IN_EPR_MODE)) {
        return PDPORT_EPR_ENTER_ALREADY;
    }
    if (!g_port.pe_flags.test(pd::PE_FLAG::HAS_EXPLICIT_CONTRACT) ||
        g_port.revision < pd::PD_REVISION::REV30)
    {
        return PDPORT_EPR_ENTER_REFUSED;
    }
    if (g_port.source_caps.empty()) { return PDPORT_EPR_ENTER_REFUSED; }
    const pd::PDO_FIXED first{g_port.source_caps[0]};
    if (!(first.pdo_type == pd::PDO_TYPE::FIXED && first.epr_capable)) {
        return PDPORT_EPR_ENTER_REFUSED;
    }
    g_dpm.request_epr_entry();
    return PDPORT_EPR_ENTER_QUEUED;
}

int pdport_epr_exit(void)
{
    if (!g_port.pe_flags.test(pd::PE_FLAG::IN_EPR_MODE)) {
        return PDPORT_EPR_EXIT_NOT_ACTIVE;
    }
    if (g_pe.is_in_spr_contract()) {
        g_dpm.request_epr_exit();
        return PDPORT_EPR_EXIT_QUEUED;
    }

    // EPR-level contract: request the first fixed SPR PDO of the current
    // list (PDO 1 when the list has no fixed SPR object), then ask the
    // caller to repeat the exit once the SPR contract is in place.
    uint8_t spr_pos = 1u;
    const auto& caps = g_port.source_caps;
    const uint32_t max = caps.size() > 7u ? 7u : (uint32_t)caps.size();
    for (uint32_t i = 0; i < max; i++) {
        if (caps[i] != 0u &&
            get_src_pdo_variant(caps[i]) == pd::PDO_VARIANT::FIXED)
        {
            spr_pos = (uint8_t)(i + 1u);
            break;
        }
    }
    g_dpm.trigger_by_position(spr_pos);
    return PDPORT_EPR_EXIT_SPR_FIRST;
}

int pdport_epr_auto(int enable)
{
    g_dpm.enable_auto_epr_entry(enable != 0);
    return 0;
}

// ---------------------------------------------------------------------
// Sink-initiated query / control messages (CLI diagnostics)
// ---------------------------------------------------------------------

static bool pdport_link_up(void) {
    return g_started && (g_port.is_attached || pd_tr_vbus_ok());
}

static bool pdport_contract_up(void) {
    return pdport_link_up() &&
        g_port.pe_flags.test(pd::PE_FLAG::HAS_EXPLICIT_CONTRACT);
}

int pdport_send_ctrl(uint32_t ctrl_msgt)
{
    // Get_Source_Cap is legal before the contract is up; the other
    // sink-initiated control queries need an explicit contract.
    if (!pdport_link_up()) { return -1; }
    if (ctrl_msgt != PDPORT_CTRL_GET_SOURCE_CAP && !pdport_contract_up()) {
        return -1;
    }
    g_pe.send_ctrl_msg((pd::PD_CTRL_MSGT::Type)ctrl_msgt);
    g_port.wakeup();
    return 0;
}

int pdport_send_data(uint32_t data_msgt, const uint32_t *dos, uint32_t ndo)
{
    if (!pdport_contract_up()) { return -1; }
    if (ndo == 0u || ndo > 7u || dos == nullptr) { return -1; }
    // The PRL keeps whatever payload is in tx_emsg when it sets the header
    // (same pattern the PE uses for EPR_Mode DOs).
    g_port.tx_emsg.clear();
    for (uint32_t i = 0; i < ndo; i++) { g_port.tx_emsg.append32(dos[i]); }
    g_pe.send_data_msg((pd::PD_DATA_MSGT::Type)data_msgt);
    g_port.wakeup();
    return 0;
}

int pdport_send_ext(uint32_t ext_msgt, const uint32_t *dos, uint32_t ndo)
{
    if (!pdport_contract_up()) { return -1; }
    if (ndo > 7u || (ndo > 0u && dos == nullptr)) { return -1; }
    g_port.tx_emsg.clear();
    for (uint32_t i = 0; i < ndo; i++) { g_port.tx_emsg.append32(dos[i]); }
    g_pe.send_ext_msg((pd::PD_EXT_MSGT::Type)ext_msgt);
    g_port.wakeup();
    return 0;
}

int pdport_hard_reset(void)
{
    // PE::request_hard_reset() re-checks the contract itself.
    if (!pdport_contract_up()) { return -1; }
    g_pe.request_hard_reset();
    g_port.wakeup();
    return 0;
}

} // extern "C"
