# PDEngine — open USB-PD sink protocol engine (pdsink core + ETL)

Full-source replacement for the closed ST USBPD PE/PRL core (see
`EPR_SESSION_FINDINGS.md`, "PIVOT" section).  The closed ST core's EPR AMS
freezes this board in every reproduction; per the user's directive the PD
protocol engine is replaced with an open, MIT-licensed stack and a driver
written for the STM32H7R3 UCPD.

## Layout

    PDEngine/
      pdsink/            pdsink USB-PD sink stack (MIT), core only
                         (no FUSB302 driver, no ESP32 example)
        LICENSE          MIT license, Copyright (c) 2025 The Project Authors
        include/pd/      public umbrella header
        src/pd/          tc / prl / pe / dpm / port / task + utils
        test/            upstream unit-test suites (googletest)
      etl/               Embedded Template Library (MIT), headers only
        LICENSE
        include/etl/     ETL 20.43.0 headers

## Provenance

- pdsink: https://github.com/pdsink/pdsink , commit `df7e126` (2026),
  MIT.  Sink-only USB PD 3.2 stack: SPR + PPS + EPR, sink role.  Layered
  TC (CC/attach), PRL (protocol incl. chunking + retries), PE (policy,
  EPR entry/exit states), DPM (request policy), Port (shared state),
  Task (event loop).  Driver abstraction: `pd::IDriver` (`ITCPC` +
  `ITimer`) with a per-PHY implementation; the STM32H7R3 UCPD driver is
  the project-owned piece under development.
- ETL: https://github.com/ETLCPP/etl , tag `20.43.0`, MIT.

## Host test gate

    tools/pdengine_hosttest/run.sh

Compiles the core with the vendored ETL and runs the six upstream test
suites (afsm, atomic_bits, leapsync, spsc_overwrite_queue, timer_pack,
validate_source_caps).  GoogleTest is fetched into the user cache on
first run (tests only; not vendored).  79 tests, all green.

## Integration status (milestones, see EPR_SESSION_FINDINGS.md)

All milestones are complete **on the host bench** (no ARM toolchain is
available in this environment; "green" = the host gates below + the
`tools/pdport_hosttest` suites).  What remains is board-side: flashing
the pdsink build and running the live-hardware acceptance listed under
M5 (see also the flash-safety appendix in `USB_UCPD/FLASHING.md`).

- [x] M1 host bring-up: core vendored; upstream suites green on host.
- [x] M2 UCPD driver + transport contract (`port/pd_ucpd_driver.cpp`,
      15 host tests).
- [x] M3 SPR bench milestone (host): attach/Source_Cap/Request/PPS/
      detach/HR flows over the simulated UCPD, 6 tests.
- [x] M4 ST transport `port/pd_tr_st.c` (syntax-gated against the real
      project headers) + CubeIDE switch-over guide (`port/README.md`) +
      **M4-app** board glue `port/pdport_app.{h,cpp}` with the C seam
      for the CLI/status modules (5 host scenarios, cold boot each).
      Remaining board-side: re-pointing the app call sites
      (`app_cli.c`, `app_pd.c`, `app_epr.c`, `main.c`) onto the seam and
      compiling in CubeIDE; VDM/structured-Get tester commands have no
      pdsink engine and report n/a on that path.
- [x] M5 EPR bench milestone (host): pdsink PE auto-entry via an
      EPR-capable source, EPR_Mode Enter_Succeeded, chunked EPR
      Source_Capabilities, AVS request, EPR keep-alive, sink-initiated
      EPR exit (project-local core addition), Enter_Failed recovery and
      stalled-caps hard-reset recovery — 4 stack tests + the M4-app glue
      scenarios.  **Board acceptance remains** (no live board here):
      flash the pdsink build, then confirm on the bench:
      1. SPR + PPS behaviour is unchanged and `pdport_service` never
         hangs (console alive through detach/reattach),
      2. `Enter Succeeded` on a PD 3.1 EPR source with the board alive
         (auto entry; `epr` status shows EPR mode),
      3. EPR Source_Capabilities/AVS receive + requestable (`epr` table,
         `pdport_request_epr_avs`), truthful `epr`/`pd` status,
      4. `epr exit` (two-step via `pdport_epr_exit`) returns to SPR with
         PPS still working,
      5. no hard fault / hang in any of the above (fail-safe = the
         closed-core path is still in git; see the flash appendix).

Target build notes (CubeIDE 2.2.0 / GNU ARM 14.3.1): add
`Middlewares/PDEngine/pdsink/src` + `.../pdsink/include` +
`.../etl/include` to the C++ include path, compile
`pdsink/src/pd/*.cpp` + `pdsink/src/pd/utils/dobj_utils.cpp` as C++17,
define `PD_USE_CONFIG_FILE` with a project `pd_config.h` (see
`USB_UCPD/Appli/.../pd_config.h` when the driver lands).
