# PDEngine port — STM32H7RS integration guide

This directory holds the board-independent pdsink port for this project:

| File | Purpose |
|------|---------|
| `include/pdport/pd_tr.h` | Transport contract between `pd_ucpd_driver.cpp` and the UCPD hardware binding |
| `include/pdport/pd_ucpd_driver.h` | pdsink `IDriver` (= ITCPC + ITimer) over the transport |
| `src/pd_ucpd_driver.cpp` | The driver (host-benched, IRQ-safe, single 1 ms service pump) |
| `src/pd_tr_st.c` | **M4**: STM32H7RS transport — binds the driver to the open ST USB-PD device layer (`Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX`) |
| `include/pdport/pdport_app.h` | **M4-app**: C seam for the application modules (status snapshot, request engine, EPR control, events) |
| `src/pdport_app.cpp` | **M4-app**: pdsink object graph for the board + implementation of the C seam (host-benched end-to-end) |

Nothing in `port/` touches STM32 registers except `pd_tr_st.c`, and
`pd_tr_st.c` uses only the open device layer (PHY API + `CAD_Init()` +
`HW_SignalAttachement()`/`HW_SignalDetachment()`), never the closed
USBPD core library.

## What replaces what

The pdsink path replaces the closed-core protocol machinery:

| Closed-core artefact | pdsink replacement |
|----------------------|--------------------|
| `Middlewares/ST/STM32_USBPD_Library/Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a` (link entry in `Appli/.cproject`) | pdsink core: `Middlewares/PDEngine/pdsink/src/pd/*.cpp` + ETL (`Middlewares/PDEngine/etl`) — compiled from source |
| Closed PE/PRL/CAD state machines | pdsink `TC` (CC scan/polarity), `PRL` (protocol), `PE` (policy engine) — see below |
| `Appli/USBPD/App/usbpd.c` (`MX_USBPD_Init`) | Same function name; in-tree pdsink branch (guarded) does `APP_PD_Init()` + `pdport_init()` |
| `Appli/USBPD/App/usbpd_dpm_core.c` + closed-core glue (`USBPD_DPM_InitCore/InitOS/Run/TimerCounter`, `USBPD_DPM_CADCallback`…) | pdsink init + one 1 ms pump (exclude the file) |
| `Appli/USBPD/Target/usbpd_dpm_user.c` (closed-core DPM user callbacks → closed PE/CAD/SVDM entry points) | seam event callback (registered in `APP_PD_Init()`) — exclude the file |
| ST PHY layer callbacks (wired by the closed core through `USBPD_PHY_Init`) | `pd_tr_st.c` PHY callbacks, same seams |

The open ST device layer **stays compiled exactly as today** (`_SNK`,
`USBPDCORE_LIB_PD3_FULL`, `USBPD_PORT_COUNT=1`, `USE_FULL_LL_DRIVER`,
include paths listed in `Appli/.cproject`).  `UCPD1_IRQHandler` in
`Appli/Core/Src/stm32h7rsxx_it.c` keeps calling `USBPD_PORT0_IRQHandler()`
(open `usbpd_hw_if_it.c`), which now dispatches into the `pd_tr_st.c`
callbacks through `Ports[0].cbs`.

## CubeIDE wiring steps (single port, Debug/Release both)

0. **Define `PDENGINE_PDSINK` project-wide** (Project → Properties → C/C++
   Build → Settings → MCU GCC Compiler → Preprocessor, add
   `PDENGINE_PDSINK`; it selects the in-tree `#if defined(PDENGINE_PDSINK)`
   branches in the app/glue files listed under step 4).  Without this
   define the build is byte-identical to the closed-core path.

1. **Compile pdsink + port sources** (C++17 — add `-std=gnu++17` to the
   C++ compiler options if not already set):

   - `Middlewares/PDEngine/pdsink/src/pd/*.cpp`
   - `Middlewares/PDEngine/pdsink/src/pd/utils/*.cpp`
   - `Middlewares/PDEngine/port/src/pd_ucpd_driver.cpp`
   - `Middlewares/PDEngine/port/src/pdport_app.cpp` (board glue, see below)

   Include paths to add:
   - `Middlewares/PDEngine/etl/include`
   - `Middlewares/PDEngine/pdsink/src`
   - `Middlewares/PDEngine/pdsink/include`
   - `Middlewares/PDEngine/port/include/pdport`

2. **Compile the transport** `Middlewares/PDEngine/port/src/pd_tr_st.c`
   as C.  It needs no new include paths beyond the ones already in the
   project (it includes `usbpd_devices_conf.h`, `usbpd_core.h`,
   `usbpd_phy.h`, `usbpd_hw_if.h`, `usbpd_cad_hw_if.h`).

3. **Remove the closed core from the link**: delete the
   `.../Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a` entry from
   `Appli/.cproject` (or uncheck it in Project → Properties → C/C++
   Build → Settings → MCU GCC Linker → Libraries, and remove the
   `Middlewares/ST/STM32_USBPD_Library/Core/lib` include path).

4. **Exclude the closed-core DPM/PE user glue from the build**
   (right-click → Resource Configurations → Exclude from build):

   - `Appli/USBPD/App/usbpd_dpm_core.c` (its exports — `USBPD_DPM_InitCore/
     InitOS/Run/TimerCounter`, `USBPD_TRACE_Init()` caller — are replaced
     by the pdsink init + 1 ms pump; keep the file excluded from now on).
   - `Appli/USBPD/Target/usbpd_dpm_user.c` (closed-core DPM user
     callbacks; each body calls into the closed PE/CAD library
     — `USBPD_PE_Request_HardReset`, `USBPD_CAD_EnterErrorRecovery`,
     SVDM requests, … — so it cannot link once the `.a` is gone.  Its app
     hooks are replaced on the pdsink path by the seam event callback
     registered in `APP_PD_Init()`, which the pdsink branch of
     `MX_USBPD_Init()` now calls explicitly).

   `Appli/USBPD/App/usbpd.c` **stays compiled**: its `MX_USBPD_Init()` has
   an in-tree pdsink branch (`pdport_init()` with `Appli_Fatal(8)` on
   failure).  Everything else in `Appli/USBPD/Target/*` and every
   `Appli/Core/Src/*.c` module is closed-core-free on the pdsink profile
   (verified by a symbol audit against the `.a`); the only closed-core
   registration left in the open ST layer is one line in
   `Core/src/usbpd_trace.c` (`USBPD_PE_SetTrace`, see the documented
   `#if !defined(PDENGINE_PDSINK)` seam guard there).

   **M4-app status**: complete and committed.  The app call sites are
   re-pointed onto the seam and guarded with `#if defined(PDENGINE_PDSINK)`
   (default OFF — closed path unchanged): `app_epr.c` EPR verbs/status
   read the seam snapshot and queue real pdsink requests (`epr enter`
   reports queued, never fabricated); `app_pd.c` request engine routes
   `req`/`pps`/`auto`/`sweep` through `pdport_*`; `app_vdm_target.c` /
   `app_cable_target.c` reject/register-nothing truthfully (the pdsink PE
   has no SVDM client — VDM/cable discovery stays "n/a" on a pdsink
   build); `app_pdcap.c` capture registration is compiled out; `main.c`
   pumps `pdport_service()`; `usbpd.c` inits the seam (see step 5).

5. **Init (one-time, before the main loop)**: done in-tree and guarded —
   the pdsink branch of `MX_USBPD_Init()` (`Appli/USBPD/App/usbpd.c`)
   calls `APP_PD_Init()` (arms the seam event callback) then
   `pdport_init()` (`pd_tr_st.c` transport init + pdsink `Task::start`),
   and fails with `Appli_Fatal(8)` if the transport init fails.
   `main.c` calls `MX_USBPD_Init()` before the loop as today; no manual
   externs needed.

6. **Main loop**: done in-tree and guarded — `main.c` `while (1)` calls
   `pdport_service()` instead of `USBPD_DPM_Run()` when
   `PDENGINE_PDSINK` is defined.  The pump keeps its own 1 ms cadence
   from `HAL_GetTick()` and must be called regularly (every loop pass is
   fine — it self-throttles), like the old `USBPD_DPM_Run()` slice.
   Everything else in the loop (`APP_PD_Task`, CLI, INA226, LED…) stays
   untouched.

## Board glue — `port/src/pdport_app.cpp` (committed, host-benched)

The glue is not a sample anymore: `src/pdport_app.cpp` (+
`include/pdport/pdport_app.h`) lives in this tree and the host gate
exercises it end-to-end (`test_pdport_app`; every scenario runs in its
own process = one cold boot, because the graph is a boot-time object
exactly like the board's).  It owns the single-port pdsink graph and
exposes the C seam the application modules call on the pdsink path:

| C function | Replaces (closed core) | App call sites |
|------------|------------------------|----------------|
| `pdport_init()` / `pdport_service()` | `MX_USBPD_Init` body; `USBPD_DPM_InitCore` + `USBPD_DPM_Run` | `main.c` |
| `pdport_get_status()` | `DPM_Params` / `USBPD_DPM_GetDataInfo` reads | CLI status tables (`pd`, `epr`) |
| `pdport_request_position/any/pps/epr_avs()` | `USBPD_PE_Send_Request`, PPS engine | `req`, `pps` command bodies |
| `pdport_epr_enter/exit/auto()` | `USBPD_PE_Request_EPRModeEnter/Exit` | `epr` command body |
| `pdport_set_event_cb()` | `USBPD_DPM_*` notify callbacks | `APP_PD_OnNotify`-style modules |

EPR verbs return explicit codes (`QUEUED` / `ALREADY` / `NOT_ACTIVE` /
`SPR_FIRST` / `REFUSED` / `FAILED`) so a CLI never reports a queued
request as an entered mode.  `epr exit` implements the PD 3.1 two-step
(SPR-level contract first) and says so via `PDPORT_EPR_EXIT_SPR_FIRST`.
With no trigger armed the pdsink default DPM policy applies: request the
first supported source PDO (vSafe5V).

The board DPM publishes the sink PDO table from `usbpd_pdo_defs.h`
(5/9/12/15/20 V + PPS) plus an EPR block derived from the compile-time
`PDPORT_CEILING_MV` (default 28 000 = `APP_EPR_DEFAULT_CEILING_MV`) and
`PDPORT_EPR_WATTS` (default 140) macros in `pdport_app.cpp` — review
those when the board's front-end rating changes.

Notes:
- `UcpdDriver::service()` must be called at least once per millisecond.
  It delivers the pdsink timer tick, drains the RX ring into the PRL on
  the stack's schedule, resolves deferred GoodCRC replies, watches TX
  timeouts and forwards latched IRQ events.  All of that is the same
  code the host benches (M2/M3/M4-app/M5) exercise.
- CC scanning is done by pdsink's TC through `pd_tr_read_cc()`
  (comparator band read); the Type-C event interrupt
  (`TR_CcWakeUp` → `pd_drv_on_cc_event`) only makes the poll happen
  sooner.  No CAD state machine runs.
- DPM triggers (which PDO to request, PPS set-point changes, EPR mode
  entry) come from the application modules through the `pdport_app.h`
  seam.  The pdsink DPM provides the underlying
  `trigger_any()/trigger_by_position()/trigger_variant()` sugar plus the
  project-local `request_epr_entry()/request_epr_exit()/
  enable_auto_epr_entry()` EPR controls (host-benched).

## Transport behaviour summary (pd_tr_st.c)

- **RX**: hardware CRC validated by the UCPD; frame length is
  `RX_PAYSZ` (same units as `TX_PAYSZ`, CRC excluded).  Frames land in
  the driver ring with the message header first (LE16, identical to the
  convention the closed core used on this device layer).
- **TX**: `USBPD_PHY_SendMessage(SOP)`; the device layer refuses while
  an RX transaction is in progress or the TX DMA is busy — mapped to the
  contract's `-1` so the driver defers/retries.  TX completion
  (`TXMSGSENT/DISC/ABT`) feeds `pd_drv_on_tx_done(status)`.
- **Hard reset**: `USBPD_PHY_ResetRequest(HARD_RESET)` (direct
  `LL_UCPD_SendHardReset`, no DMA); `TxHRSTSENT` feeds
  `pd_drv_on_hr_done()`, partner HR (`RxHRSTDET`) feeds
  `pd_drv_on_hr_rx()`.
- **SinkTxOK**: presented as the ST device layer does it
  (`SetResistor_SinkTxOK` = internal Rp 3.0 A class + factory trim)
  before a data frame goes out, dropped back to SinkTxNG when idle —
  the exact sequence the closed-core PRL used on this board.
- **CC levels**: decoded from the UCPD `SR` Type-C comparator bands in
  sink mode (vRdUSB → Rp 0.5 A, vRd1.5 → Rp 1.5 A, vRd3.0 → Rp 3.0 A).
  pdsink's PRL only starts an AMS when the active line reads the 3.0 A
  band (SinkTxOK) — the same condition the ST layer's
  `USBPD_HW_IF_IsResistor_SinkTxOk()` checked, so a source presenting a
  lower class simply never receives an AMS from this sink (identical to
  the previous stack's behaviour).
- **VBUS**: CC-only tester (no VBUS ADC); `pd_tr_vbus_ok()` follows the
  CC presence, as documented in `pd_tr.h`.

## Host gates

`tools/pdport_hosttest/run.sh` (from the repo root) runs, without an ARM
toolchain:

1. `test_pdport_driver` — M2 driver unit suite (15 tests),
2. `test_pdport_stack` — M3 full-stack SPR bench + M5 EPR bench over the
   simulated transport (10 tests: 6 SPR — fixed 12 V, vSafe5V fallback,
   PPS, PPS change, detach/reattach, silent-source hard reset — and 4
   EPR: Enter_Succeeded + AVS contract + keep-alives, sink-initiated
   exit to SPR then PPS, Enter_Failed disabling auto-entry with SPR
   intact, stalled-EPR-caps hard-reset recovery),
3. `test_pdport_app` — M4-app board-glue suite: `pdport_app.cpp` driven
   through its C seam (init/service, status snapshot, req/PPS/EPR-AVS
   requests, EPR enter/two-step exit/auto with truthful return codes,
   detach/reattach, refusals), one cold boot per scenario,
4. `pd_tr_st.c` syntax check against the real project headers/defines
   (`-Wall -Wextra`, no warnings from the transport file).

Milestones: M1 = vendored pdsink core + engine host gate (green), M2 =
UCPD driver + gate (green), M3 = full-stack SPR bench (green), M4 =
ST transport (green) + CubeIDE wiring (this guide) + app glue
(`pdport_app`, green on the host gate; the per-command app re-pointing
table is above), M5 = EPR bench (host: green — see the 4 EPR tests) +
board bench (pending, live hardware) + flash-safety summary
(`USB_UCPD/FLASHING.md` appendix).
