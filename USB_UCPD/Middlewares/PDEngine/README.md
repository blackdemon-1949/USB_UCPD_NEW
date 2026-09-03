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

- [x] M1 host bring-up: core vendored; upstream suites green on host.
- [ ] M2 UCPD driver (`pd::IDriver` over UCPD1 + GPDMA1 CH0/CH1).
- [ ] M3 SPR bench milestone (ST core removed from the link).
- [ ] M4 feature parity (CLI/EPR app tables, VDM/cable re-pointed).
- [ ] M5 EPR bench milestone (Enter Succeeded + board alive).

Target build notes (CubeIDE 2.2.0 / GNU ARM 14.3.1): add
`Middlewares/PDEngine/pdsink/src` + `.../pdsink/include` +
`.../etl/include` to the C++ include path, compile
`pdsink/src/pd/*.cpp` + `pdsink/src/pd/utils/dobj_utils.cpp` as C++17,
define `PD_USE_CONFIG_FILE` with a project `pd_config.h` (see
`USB_UCPD/Appli/.../pd_config.h` when the driver lands).
