# Build, Flash and Prove EPR — commands to run on your machine

I could not run these here: this sandbox has no ARM toolchain and cannot
obtain one. Confirmed dead end, not an assumption:

* `github.com` 302-redirects release downloads to
  `release-assets.githubusercontent.com`, which is **network-blocked**
  (`SSL_ERROR_SYSCALL`). Same for the Zephyr SDK.
* The npm package `@xpack-dev-tools/arm-none-eabi-gcc` downloads fine but is a
  **10 KB metadata stub** — the compiler payload is on that same blocked CDN.
* `developer.arm.com`, Debian/Ubuntu pools, jsdelivr, unpkg, conda,
  PlatformIO registry: all blocked. No `apt` privileges.
* No `/dev/bus/usb` and no ST-Link, so nothing could be flashed or captured.

Per your instruction I stopped rather than claim a firmware validation.

---

## 1. Build (STM32CubeIDE 2.2.0 / GNU ARM 14.3.1)

Preferred — the real managed build:

> STM32CubeIDE → import `USB_UCPD/Boot` and `USB_UCPD/Appli` →
> Project → Build All (Debug **and** Release)

Expect **0 errors, 0 linker errors**. Every changed file already passes
`-Wall -Wextra` cleanly with the project's exact define set
(`USBPDCORE_LIB_PD3_FULL USBPD_PORT_COUNT=1 STM32H7R3xx USE_HAL_DRIVER
USE_FULL_LL_DRIVER USE_FULL_LL_DRIVERS _SNK _TRACE`).

Equivalent headless build (same sources/defines/linker script, read from
`.cproject`), with the CubeIDE toolchain on PATH:

```bash
export PATH="$HOME/st/stm32cubeide_2.2.0/plugins/\
com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.\
linux64_1.0.0/tools/bin:$PATH"     # adjust to your install
arm-none-eabi-gcc --version        # must report 14.3.1

cd <repo>
python3 tools/build.py --project all --config all --jobs 8
```

Windows PATH is typically:
`C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.0\tools\bin`

## 2. ELF / MAP / symbol validation

```bash
python3 tools/verify.py
```

I added the new EPR symbols to its allowlist, so this now fails if the EPR
path is missing or compiled out. Manual cross-check:

```bash
arm-none-eabi-nm -C USB_UCPD/Appli/Debug/Appli.elf | grep -E \
 'APP_EPR_(OnSprSrcCaps|ModeEnter|ModeExit|RequestSrcCapa|Probe|Diag|PollTx)'
```

All seven must appear as `T` (defined in .text). Then confirm the ST EPR
objects are actually pulled from the library:

```bash
grep -E 'usbpd_pe_epr\.o|usbpd_pe_extctrl\.o' USB_UCPD/Appli/Debug/Appli.map
arm-none-eabi-size USB_UCPD/Appli/Debug/Appli.elf
```

Size should grow by roughly 1–3 KB versus the previous image. **A large jump
would be suspicious, and no growth at all means the EPR code was discarded.**

## 3. Flash

Unchanged, external-loader based — do not substitute an internal-flash script:

```
STM32_Programmer_CLI -c port=SWD mode=UR \
  -el "STM32H7R3Z8Jx_8MB_WeAct.stldr" \
  -d USB_UCPD/Appli/Release/Appli.bin 0x90000000 -v
```

## 4. Prove it on the wire

Attach the **28 V/5 A EPR source** with the **e-marked** cable, open the CDC
console, then:

```
epr diag        <-- run this FIRST, before anything else
```

`epr diag` prints the exact runtime state against each decoded ST gate:

```
  PE connected       : yes
  power role         : SNK
  contract           : EXPLICIT CONTRACT
  Is_EPR_Supported_SNK: 1 (enabled)      <-- was 0; this was the bug
  EPR_Get_Source_Cap : all gates PASS - API may queue
  EPR_Mode(Enter)    : all gates PASS - API may queue
```

**If any line reads `GATE CLOSED` or `BLOCKED`, stop and send me that output** —
it names the failing precondition directly, so no guessing is needed.

Then:

```
epr caps        # sends EPR_Get_Source_Cap
epr             # status
```

`epr caps` deliberately reports three separate facts and never conflates them:

* `ACCEPTED into PE AMS slot (queued, NOT yet sent)` — API only
* `UCPD TX observed (PD TX n -> n+1)` — a frame really left the transmitter
* `accepted by PE but NO UCPD TX observed - blocked between PE and PRL` — the
  precise failure mode you asked me to detect rather than guess at

## 5. Capture CC traffic

USART1 / PB6-PB7 @ **921600** into STM32CubeMonitor-UCPD (`_TRACE` is enabled
in both build configs, and the trace funnel is already registered).

Target sequence for this phase:

```
SPR Source_Capabilities -> explicit SPR contract
   -> EPR_Get_Source_Cap        <-- previously NEVER appeared
   -> EPR_Source_Capabilities
```

Only after that is proven should EPR entry / Request / Accept / PS_RDY /
KeepAlive / exit be evaluated.

## 6. Regression (must all still work)

`5 V`, `9 V`, `12 V`, `20 V`, PPS (`21 V/5 A`, `15.789 V/5 A`), INA226,
USB CDC enumeration, UCPD attach, XIP boot. Nothing in these changes touches
the DMA map, linker scripts, MPU config or USB stack.

## 7. Status after the first bench run

The first hardware run **confirmed the primary fix**: with an EPR-capable
source attached, `epr diag` reported

```
Is_EPR_Supported_SNK: 1 (enabled)
EPR_Get_Source_Cap : all gates PASS - API may queue
EPR_Mode(Enter)    : all gates PASS - API may queue
src 5V PDO EPR bit : SET (source is EPR capable)
```

All five ST gates that were previously closed are now open, and SPR
regression was clean (5 V contract, INA226 5.020 V).

It also exposed four defects, now fixed — see `EPR_FORENSIC_REPORT.md` §H.

| Stage | EPR |
|---|---|
| RESEARCHED | **YES** |
| IMPLEMENTED | **YES** — gates open, entry now well-formed |
| COMPILED | **PARTIAL** — `-Wall -Wextra` clean + 148 host tests; **no ARM link here** |
| FLASHED | **YES (by you)** — first run done, fixes not yet re-flashed |
| HARDWARE VERIFIED | **PARTIAL** — gates + SPR proven; EPR wire sequence still to confirm |
