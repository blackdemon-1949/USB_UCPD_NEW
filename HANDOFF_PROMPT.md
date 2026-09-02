# Handoff prompt — copy everything below the line into a new session

---

## Task

This repo is a USB-C Power Delivery **sink/analyser** on a WeAct
**STM32H7R3Z8J6** board (XIP from external NOR at `0x90000000`). Everything
works **except EPR (USB PD 3.1 Extended Power Range)**, which hard-faults the
MCU. Find the real cause and fix it.

**Do all research before changing anything.** Read the code, disassemble the
ST library, consult the PD 3.1 spec and ST's reference projects. Do not guess,
do not patch symptoms, and do not claim success you have not proven.

## Current behaviour — this is the exact failure

With an **SPR-only** charger (5/9/12/15/20 V + PPS 3.3–21 V, 100 W max)
everything is correct and stable:

```
[PD] explicit contract  5000 mV / 3000 mA  (PDO 1)
req 2        -> 9 V contract, INA226 reads 8.981 V
pps 21000    -> 21 V contract, INA226 reads 21.021 V
```

With an **EPR-capable** charger, detection is correct:

```
epr
  src 5V PDO EPR bit : SET (source is EPR capable)
  RDO B21 EPR_Capable: will be set
  verdict    : EPR available, not entered
```

Then this happens, **every time, 100 % reproducible**:

```
epr enter
EPR_Mode(Enter): API status = USBPD_OK (0) -> ACCEPTED by PE (queued, awaiting source reply)
>
```

…and the board **dies immediately**. No further console output, no response to
input, no recovery when the PD source is unplugged. Only a physical reset or
power cycle recovers it. The PB2 LED then blinks fast (~190 Hz), which is the
firmware's `Appli_Fail()` loop — so it is a **CPU fault**, not a hang.

An SPR charger never triggers this. Only EPR does.

## What is already known (verified — do not redo blindly, but do re-verify)

* `epr enter` is dispatched from `APP_CLI_Poll()` in the **main loop**, i.e.
  normal task context. It is **not** a PE callback, so simple re-entrancy is
  already ruled out. The fault happens *inside* `USBPD_PE_Request_EPRModeEnter`
  or in the PE state machine that runs immediately after it.
* The ST core library was upgraded to official **v5.4.1**
  (`stm32-mw-usbpd-core`, commit `aafa359`), md5 `82418ccd…`, 344 742 bytes.
* Several callback slots in `USBPD_PE_Callbacks` are invoked by the library as
  `ldr rN,[cb,#off]; blx rN` with **no NULL check**. `EvaluateVconnSwap`
  (+0x2C) was NULL and is now populated, along with `RequestSetupNewPower`,
  `EvaluatPRSwap`, `SRC_EvaluateRequest`, `PowerRoleSwap`. **This fix did not
  cure the fault** — the latest log above is from the build that contains it.
  Re-examine that conclusion; the sweep may have missed a path.
* `EPRMode_Enter` (in `usbpd_pe_epr.o`) does
  `push {r1..r7,lr}; Ptr = sp; Size = sp+4` then calls GetDataInfo with
  **DataId 0x1E = `USBPD_CORE_DATATYPE_SNK_PDP_EPR`**. The DPM may write
  **exactly 4 bytes**; more corrupts saved registers and the return address.
  Verify enum values by **compiling** them, never by counting the enum by hand
  (it contains `#if`-guarded members and hand-counting gave a wrong answer
  earlier).
* `Send_EPR_SnkCapa` (`usbpd_pe_extctrl.o`) does `memclr(ctx+0x14, 0x104)`,
  fills SPR PDOs at `ctx+0x14`, then requests `SNK_PDO_EPR` (0x19) at
  `ctx+0x30` = slot 8. Bounded by `USBPD_MAX_NB_EPRPDO` (6).
* This board has **no VCONN supply**: `BSP_USBPD_PWR_VCONNOn()` returns
  `BSP_ERROR_FEATURE_NOT_SUPPORTED`. PD 3.1 requires the **source** to be
  VCONN Source before cable discovery during EPR entry, so the EPR charger
  starts a `VCONN_Swap` right after `EPR_Mode(Enter)`. Our
  `USBPD_DPM_EvaluateVconnSwap` currently REJECTs. **Investigate whether the
  reject path itself is what faults**, and whether the board must instead
  support VCONN or answer differently.
* Fault handlers stash code + `CFSR/HFSR/MMFAR/BFAR` into BKPSRAM
  (`0x38800000`) and the next boot prints them via `APP_FaultReportBoot()`.
  **This has never actually printed** — confirm the mechanism really works
  (backup regulator, clock, MPU attributes, linker) before relying on it. It
  is the single most valuable diagnostic available.

## Strong suggestions for the next attempt

1. **Get the fault registers first.** Everything else is guesswork without
   them. If BKPSRAM is unreliable, use SWD/GDB, or write the fault record to
   the trace UART directly from the handler.
2. **Attach a debugger.** `STM32CubeIDE` / `openocd` + `gdb` with a breakpoint
   on `HardFault_Handler` gives the faulting PC and stack in one run. That
   ends the guessing permanently.
3. **Capture the CC wire.** USART1 **PB6/PB7 @ 921600** into
   STM32CubeMonitor-UCPD (`_TRACE` is enabled in both build configs). Seeing
   whether `EPR_Mode(Enter)` even reaches the wire, and what the source
   replies, splits the problem in half.
4. Consider disabling `APP_ENG_CAPTURE` temporarily — it owns the trace funnel
   and runs on every PD message, so it is on the crash path.
5. Check the **stack**: `_Min_Stack_Size` is `0x2000` in
   `Appli/STM32H7R3Z8JX_ROMxspi1.ld`. The EPR path is the deepest chain in the
   firmware and `APP_LOG_Printf` adds a 256-byte frame.

## Hard constraints — do not break these

* **Never fake anything.** No invented counters, PDOs, EPR state or "success"
  messages. The CLI must report real API status and real PE state. If
  something cannot be verified, say so.
* Toolchain is **STM32CubeIDE 2.2.0 / GNU ARM 14.3.1**. Do not substitute a
  different compiler. If you cannot invoke it, say so and give exact build
  commands instead of claiming a build.
* Preserve: external loader `STM32H7R3Z8Jx_8MB_WeAct.stldr`; XIP at
  `0x90000000`; the real linker script `STM32H7R3Z8JX_ROMxspi1.ld` (**not**
  `__default_MMT_TEMPLATE.ld`); `__RAM_BEGIN=0x24000000`,
  `__RAM_SIZE=0x68000`, `__RAM_NONCACHEABLEBUFFER_SIZE=0x8000`; the linker
  `ASSERT`s; heap in AXI SRAM (UCPD DMA cannot reach DTCM).
* Preserve DMA map: GPDMA1 CH0 = UCPD RX, CH1 = UCPD TX, CH2/3 = trace UART.
* **Do not regress what works**: 5/9/12/15/20 V fixed requests, PPS/AVS up to
  21 V, INA226 monitoring, USB CDC console, UCPD attach, XIP boot, DTS `temp`.
  Re-test all of these after any change.
* No dedicated VBUS ADC exists. VBUS comes from INA226 (`ina vbus real`, the
  default) or a synthetic value.
* Middleware edits are allowed but must be **minimal and documented** with the
  evidence that justifies them.
* Commit in logical steps with real messages. Do not rewrite history.

## Verification required before you tell me to flash

* `python3 tools/hosttest/run.py` — currently **149/149 pass**; keep it green.
* Compile every file in `Appli/` with `-Wall -Wextra`; currently **0 errors**
  and exactly **3 pre-existing warnings** (`app_diag.c`,
  `system_stm32h7rsxx.c`, `usbpd_dpm_core.c`).
* `python3 tools/build.py --project all --config all` then
  `python3 tools/verify.py` (needs the real ARM toolchain).
* State plainly, per feature: RESEARCHED / IMPLEMENTED / COMPILED / FLASHED /
  HARDWARE VERIFIED. **Do not mark EPR verified without a log showing
  `Enter Succeeded` and a board that stays alive.**

## Repo orientation

* `USB_UCPD/Appli/` — the application (CubeIDE project).
  * `Core/Src/app_epr.c`, `Core/Inc/app_epr.h` — EPR engine, `epr` CLI, probe.
  * `Core/Src/app_pd.c` — requests, notifications, task loop.
  * `USBPD/Target/usbpd_dpm_user.c` — DPM callbacks (`GetDataInfo`,
    `SetDataInfo`, `EvaluateVconnSwap`).
  * `USBPD/App/usbpd_dpm_core.c` — the `USBPD_PE_Callbacks` table.
  * `USBPD/Target/usbpd_dpm_conf.h` — `DPM_Settings`, `Is_EPR_Supported_SNK`.
* `USB_UCPD/Middlewares/ST/STM32_USBPD_Library/Core/lib/` — prebuilt ST core.
* `EPR_FORENSIC_REPORT.md`, `NEXT_FLASH_CHECKLIST.md` — prior findings.
  **Treat them as leads, not facts; earlier sessions made mistakes that these
  documents record honestly.**
* `tools/hosttest/run.py`, `tools/build.py`, `tools/verify.py`.

## Useful CLI commands on the board

`epr` · `epr diag` (per-gate PE state) · `epr enter` · `caps` · `req <n>` ·
`pps <mv> [ma]` · `diag all` · `cable status` · `temp` · `engines` · `ina`

---

**Start by reproducing my analysis and challenging it.** The EPR fault has
survived several plausible fixes, so at least one assumption above is probably
still wrong. Find which one.
