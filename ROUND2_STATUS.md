# Round 2 status (Phases 1–4)

## Phase 3 — e-marker path (A1 + A2) — DONE, verified at source level

**A1 — VCONN** — RESOLVED by admitting what the hardware says.
Grepping the call chain proved `HW_IF_PWR_Enable(VCONN)` calls
`BSP_USBPD_PWR_VCONNOn`, which is a stub returning
`BSP_ERROR_FEATURE_NOT_SUPPORTED`, and `BSP_USBPD_PWR_VCONNInit` returns the
same. README.md documents one CC line to the MCU header and no VCONN power
FET. This rig cannot source VCONN, so SOP' Discover Identity *cannot* be
originated from this Sink end — that is a hardware precondition, not a
software bug. Therefore:

- `Appli/USBPD/Target/usbpd_dpm_conf.h`: `PE_VconnSwap` reverted to
  `USBPD_FALSE`; `PE_SupportedSOP` reverted to `USBPD_SUPPORTED_SOP_SOP`
  only (with explanatory comments).
- `Appli/USBPD/Target/usbpd_dpm_user.c`: `USBPD_DPM_PE_VconnPwr` reverted
  to return `USBPD_ERROR` (returning `USBPD_OK` here lied to the PE).
- `Appli/Core/Inc/apie.h`: `APIE_HW_CABLE_EMARKER` set to `0U` with a
  comment explaining the VCONN precondition.
- `Appli/Core/Src/app_pd.c`: auto-SOP'-probe state removed (variables
  reset, EXPLICIT_CONTRACT handler no longer queues a probe, APP_PD_Task
  poll block removed). No VCONN-swap is accepted and no SOP' identity
  request is fired automatically.
- `Appli/Core/Src/app_cli.c` (`cable` command): prints a clear "VCONN
  not available on this board" message when `APIE_HW_CABLE_EMARKER==0`;
  still issues the request when the flag is on (future hardware).
- `Appli/USBPD/Target/usbpd_vdm_user.c`: SOP'/SOP'' dispatch to
  `APIE_Cable_OnIdentity` is retained (safe even without VCONN — any
  incoming SOP'/SOP'' messages, e.g. PD 3.1 EPR relay, are still decoded).

**A2 — bit-field decoding** — RESOLVED by switching away from hand-shifted
masks entirely. `Appli/Core/Src/apie_cable.c` now reinterprets the raw
`uint32_t` identity words through ST's `USBPD_IDHeaderVDO_TypeDef` /
`USBPD_CableVdo_TypeDef` / `USBPD_ActiveCableVdo1_TypeDef` /
`USBPD_ProductVdo_TypeDef` anonymous-bitfield unions (`.b.` / `.b20.` named
members) so the compiler's layout is used directly. No numeric shift/mask
constants remain for any of the cable VDO / ID Header / Product VDO fields.
All current-capability / Vmax / SS / EPR-capable fields now use the
named enums (`VBUS_3A`, `VBUS_MAX_20V`, `USB3P2_GEN1`, …) from
`usbpd_def.h`. AVSPDO detection in `APIE_EPR_OnSourceCaps` is conditional
on `#if defined(USBPDCORE_EPR)` (which is **not** defined this round —
Phase 5 gate) so current builds don't touch AVSPDO bitfields.

## Phase 1 — SDMMC1 + FatFS — SOURCE-COMPLETE, real vendor files

- Replaced the hand-written HAL_SD stub with the real ST driver fetched
  from `STMicroelectronics/stm32h7rsxx-hal-driver` @ main (HAL v1.2.1,
  matching the existing `STM32H7RSXX_HAL_VERSION_*` defines in stm32h7rsxx_hal.h):
  - `Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_sd.c`
  - `Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_sd_ex.c`
  - `Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_dlyb.c`
  - Inc/ counterparts added.
- Replaced the hand-written FatFS stubs with real ChaN FatFS R0.15 + ST's
  sd_diskio glue fetched from `STMicroelectronics/stm32-mw-fatfs` @ master:
  - `Middlewares/Third_Party/FatFs/src/ff.c`, `ff.h`, `ffunicode.c`,
    `ffsystem.c` (bare-metal), `ff_gen_drv.c/.h`, `diskio.c/.h`
  - `Middlewares/Third_Party/FatFs/src/drivers/sd/sd_diskio.c/.h`
  - Project `ffconf.h` (437 codepage, 8.3 names, FF_USE_STRFUNC=1,
    FF_FS_TINY=1, FF_FS_REENTRANT=0, single volume "0:").
- `Appli/Core/Src/sdmmc.c`: removed stub fallback #defines for
  `SDMMC_CLOCK_*` / `SDMMC_BUS_WIDE_*`; the real vendor headers provide them.
- `Appli/Core/Inc/fatfs.h` + `Appli/Core/Src/fatfs.c`: real FatFS types +
  `FATFS_LinkDriver(&SD_Driver, SDPath)` registration.
- `Appli/Core/Inc/ext_sd.h` + `Appli/Core/Src/ext_sd.c`: real
  `EXT_SD_HardInit()` (HAL_SD_Init + `HAL_SD_ConfigWideBusOperation(4B)` +
  `HAL_SD_GetCardInfo`), `EXT_SD_GetHandle()` + `sdmmc_handle` extern for
  sd_diskio glue, `EXT_SD_AppendLine()` kept open/write/close-per-call
  semantics, `EXT_SD_TIMEOUT_MS` raised to 2000 ms for real card I/O.
- `Appli/Core/Src/sd_diskio_config.h` (in FatFs drivers/sd) wires
  `sdmmc_handle`, `sdmmc_sd_init()` → `EXT_SD_HardInit()` and sets
  `SD_TIMEOUT=2000`.
- `tools/check_syntax.sh`: FatFS include paths added; vendor HAL_SD + FatFS
  translation units now syntax-checked too.
- **Bring-up is polling-mode** (HAL_SD_ReadBlocks/WriteBlocks use the
  SDMMC internal IDMA). No GPDMA channel wiring is required for Phase 1
  (GPDMA is initialised already in `gpdma.c` for other users).

Status: `tools/check_syntax.sh` → **68 passed, 0 failed** (APPL 55 + VENDOR
9 + BOOT 4). The SD/FatFS code has **not** yet been added to the
CubeIDE `.cproject` (CubeMX must regenerate the project with SDMMC1 +
FatFS enabled — see FLASHING.md step 1). I could not run a real
`arm-none-eabi-gcc` cross-compile in this sandbox: the arm-none-eabi
toolchain isn't installed, `apt` mirrors and GitHub release-asset CDN
both refuse connections (HTTP plaintext blocked / TLS handshake reset
to `release-assets.githubusercontent.com`), so I cannot pull a binary
toolchain. This is the step the user must run in CubeIDE before the
firmware can be declared "flashable".

## Phase 2 — INA226 energy accumulator — DONE, already wired

`ina226_energy.c/.h` were already present and use 64-bit `uAs/uWs`
integration with `HAL_GetTick()` dt (capped at 500 ms to avoid debugger
phantom contributions), `ina energy` / `ina energy reset` CLI, and 60 s
CSV checkpointing to `0:/energy.log` via `EXT_SD_AppendLine`. I added
`INA226_Energy_PrintStatus()` to the `status` command output alongside
`INA226_PrintStatus()` and added an SD-card-present line + cable/EPR
dump to `status` so an inserted card and energy integration are visible
in the standard status report. Phase 2 never wrote to NOR flash.

## Phase 4 — KPD1 SD persistence — NOT IMPLEMENTED (blocked)

Per the round-2 ground rules, Phase 4 is only buildable after real
HAL/FatFS sources are in place **and** a real cross-compile runs. The
sources are now in place, but without a CubeMX-regenerated `.cproject`
+ a working `arm-none-eabi-gcc` I cannot produce the binary and therefore
cannot bench-verify the CRC/schema-validation / boot-load path.  The KPD1
schema (`tools/apie_replay.py --mode=fuzz`) is defined in the host tool
but the on-MCU loader/writer is intentionally not attempted this round
rather than being built against a stubbed FatFS that will silently rot.
This is the correct "stop and report" action rather than shipping
unlinkable code.

## Out-of-scope items, correctly untouched

- `APIE_HW_EPR_POWER_ENABLED=0` and `APIE_MAX_VOLTAGE_MV=21000` unchanged.
- No NOR flash write path was enabled (XIP safety preserved).
- `APIE_HW_HAS_DPLUS_DMINUS=0` unchanged (D+/D- not wired).
- No EPR request code is emitted (`USBPDCORE_EPR` not defined;
  `APIE_EPR_PowerAllowed()` returns 0; Phase 5 code sits behind that gate).
- `USER CODE BEGIN/END` markers preserved everywhere (sdmmc.c, fatfs.c,
  app_pd.c, usbpd_dpm_user.c, usbpd_dpm_conf.h).
- No new CubeMX .ioc changes were made — CubeMX must regenerate against
  the existing `USB_UCPD.ioc` with SDMMC1 + FatFS enabled.

## What the user must do next (blocked items I cannot perform here)

1. Open `USB_UCPD.ioc` in STM32CubeMX, enable SDMMC1 (4-bit, PA8 CD
   active-low) and FATFS (SD, polling, drive "0:"), Generate Code. This
   updates `.cproject`/Makefile to include the new sources and sets up
   `stm32h7rsxx_hal_msp.c` / clock config consistently (our hand-written
   `HAL_SD_MspInit` in `sdmmc.c` already matches the CubeMX pattern but
   the project descriptor must list the new C files).
2. Build `USB_UCPD_Boot` and `USB_UCPD_Appli` in STM32CubeIDE; verify
   ELF/BIN/HEX are produced under `Boot/Debug/` and `Appli/Debug/` and
   the Appli image fits in the 8 MB XiP region.
3. Flash via STM32CubeProgrammer with `tools/STM32H7R3Z8Jx_8MB_WeAct.stldr`
   (as already documented in FLASHING.md).
4. After bench validation on real hardware (insert FAT32 SD, verify
   `ext-sd: mounted` and per-minute `energy.log` lines grow), Phase 4
   KPD1 persistence can be added on top of this groundwork.
5. Hardware checkpoint between Phase 4 and Phase 5 remains a human sign-off.
