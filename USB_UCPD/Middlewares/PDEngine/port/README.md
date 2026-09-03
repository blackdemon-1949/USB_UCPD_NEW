# PDEngine port — STM32H7RS integration guide

This directory holds the board-independent pdsink port for this project:

| File | Purpose |
|------|---------|
| `include/pdport/pd_tr.h` | Transport contract between `pd_ucpd_driver.cpp` and the UCPD hardware binding |
| `include/pdport/pd_ucpd_driver.h` | pdsink `IDriver` (= ITCPC + ITimer) over the transport |
| `src/pd_ucpd_driver.cpp` | The driver (host-benched, IRQ-safe, single 1 ms service pump) |
| `src/pd_tr_st.c` | **M4**: STM32H7RS transport — binds the driver to the open ST USB-PD device layer (`Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX`) |

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
| `Appli/USBPD/App/usbpd.c` (`MX_USBPD_Init`) | Same function name, new body (below) |
| `Appli/USBPD/App/usbpd_dpm_core.c` + closed-core glue (`USBPD_DPM_InitCore/InitOS/Run/TimerCounter`, `USBPD_DPM_CADCallback`…) | pdsink init + one 1 ms pump |
| ST PHY layer callbacks (wired by the closed core through `USBPD_PHY_Init`) | `pd_tr_st.c` PHY callbacks, same seams |

The open ST device layer **stays compiled exactly as today** (`_SNK`,
`USBPDCORE_LIB_PD3_FULL`, `USBPD_PORT_COUNT=1`, `USE_FULL_LL_DRIVER`,
include paths listed in `Appli/.cproject`).  `UCPD1_IRQHandler` in
`Appli/Core/Src/stm32h7rsxx_it.c` keeps calling `USBPD_PORT0_IRQHandler()`
(open `usbpd_hw_if_it.c`), which now dispatches into the `pd_tr_st.c`
callbacks through `Ports[0].cbs`.

## CubeIDE wiring steps (single port, Debug/Release both)

1. **Compile pdsink + port sources** (C++17 — add `-std=gnu++17` to the
   C++ compiler options if not already set):

   - `Middlewares/PDEngine/pdsink/src/pd/*.cpp`
   - `Middlewares/PDEngine/pdsink/src/pd/utils/*.cpp`
   - `Middlewares/PDEngine/port/src/pd_ucpd_driver.cpp`
   - new board glue (see “Board glue” below)

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

4. **Exclude the old closed-core glue from the build**
   (right-click → Resource Configurations → Exclude from build):

   - `Appli/USBPD/App/usbpd.c` (replace by the new `MX_USBPD_Init`, see
     below) — or edit its `MX_USBPD_Init` body in place.
   - `Appli/USBPD/App/usbpd_dpm_core.c` (its exports are replaced by the
     pdsink pump; keep the file excluded from now on).

   The rest of `Appli/USBPD/Target/*` and the `Appli/Core/Src/app_*.c`
   application modules still compile; **their closed-core call sites are
   the M4-app / M5 work items** (CLI status queries, `epr enter`,
   fixed-PDO/PPS request engine, VDM), tracked in the milestone plan —
   they get re-pointed at the pdsink DPM/PE state one by one.  Until
   then the board keeps its current behaviour on the closed path; switch
   over by doing steps 1–6.

5. **Init (one-time, before the main loop)**: in `main.c`, after
   `MX_UCPD1_Init()`/`MX_USBPD_Init()` (renamed entry, below) and before
   the super loop, run the pdsink graph:

   ```c
   /* usbpd.c MX_USBPD_Init() replacement body (keep the function name;
      main.c calls it before the loop).  Single port. */
   extern int  pdport_init(void);     /* pd_tr_init + pdsink Task::start   */
   extern void pdport_service(void);  /* 1 ms pump, call from the main loop */
   ```

6. **Main loop**: in `main.c` `while (1)` replace the
   `USBPD_DPM_Run();` line with `pdport_service();`.  The pump keeps its
   own 1 ms cadence from `HAL_GetTick()` and must be called regularly
   (every loop pass is fine — it self-throttles), like the old
   `USBPD_DPM_Run()` slice.  Everything else in the loop
   (`APP_PD_Task`, CLI, INA226, LED…) stays untouched.

## Board glue (single C++ file, e.g. `Appli/Core/Src/pdport_app.cpp`)

```cpp
// pdport_app.cpp - pdsink object graph for the board (sink, CC-only).
#include "pd_ucpd_driver.h"
#include "pd_tr.h"
#include "dpm.h"
#include "pe.h"
#include "port.h"
#include "prl.h"
#include "task.h"
#include "tc.h"

namespace {

pd::Port        g_port;
pdport::UcpdDriver g_driver{g_port};

class BoardDpm : public pd::DPM {
public:
    BoardDpm(pd::Port& p) : pd::DPM(p) {}
    // Optional: override get_sink_pdo_list()/get_epr_watts() to mirror the
    // board's declared sink capabilities (see pdsink dpm.cpp defaults).
};

BoardDpm    g_dpm{g_port};
pd::TC      g_tc{g_port, g_driver};
pd::PRL     g_prl{g_port, g_driver};
pd::PE      g_pe{g_port, g_dpm, g_prl, g_driver};
pd::Task    g_task{g_port, g_driver};

bool g_started = false;

} // namespace

extern "C" {

int pdport_init(void)
{
    if (g_started) { return 0; }
    if (pd_tr_init() != 0) { return -1; }
    g_task.start(g_tc, g_dpm, g_pe, g_prl, g_driver);
    g_started = true;
    return 0;
}

void pdport_service(void)
{
    if (!g_started) { return; }
    g_driver.service();   // 1 ms pdsink tick + IRQ-event processing
}

} // extern "C"
```

Notes:
- `UcpdDriver::service()` must be called at least once per millisecond.
  It delivers the pdsink timer tick, drains the RX ring into the PRL on
  the stack's schedule, resolves deferred GoodCRC replies, watches TX
  timeouts and forwards latched IRQ events.  All of that is the same
  code the host benches (M2/M3) exercise.
- CC scanning is done by pdsink's TC through `pd_tr_read_cc()`
  (comparator band read); the Type-C event interrupt
  (`TR_CcWakeUp` → `pd_drv_on_cc_event`) only makes the poll happen
  sooner.  No CAD state machine runs.
- DPM triggers (which PDO to request, PPS set-point changes, EPR mode
  entry) come from the application modules.  The default `pd::DPM`
  provides `trigger_any()/trigger_by_position()/trigger_variant()`; the
  CLI/PPS/EPR app re-wiring uses those (M4-app/M5 work items).

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
2. `test_pdport_stack` — M3 full-stack SPR bench over the simulated
   transport (6 tests: fixed 12 V, vSafe5V fallback, PPS, PPS change,
   detach/reattach, silent-source hard reset),
3. `pd_tr_st.c` syntax check against the real project headers/defines
   (`-Wall -Wextra`, no warnings from the transport file).

Milestones: M1 = vendored pdsink core + engine host gate (green), M2 =
UCPD driver + gate (green), M3 = full-stack SPR bench (green), M4 =
ST transport (this file, green) + CubeIDE wiring (this guide) + app
re-pointing, M5 = EPR bench + flash-safety summary.
