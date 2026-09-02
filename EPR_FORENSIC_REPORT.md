# EPR Forensic Audit & Fix

## A. Verdict on the previous session

**The previous EPR work was superficial.** It added a CLI command, a context
struct, counters, and calls to two real ST APIs — but both API calls hit an
early-return inside the ST library and never queued a message. Nothing ever
reached PRL/UCPD/CC. That is why the build size barely moved and hardware
traces showed SPR only.

Note on the task brief: this checkout has only two commits
(`56688c0` upload, `2b25b08` previous EPR attempt). There is **no commit or
tag `5fd7901` / `golden-baseline-5fd7901`** in this repository, so the
requested baseline diff could not be performed. The audit was done against
`2b25b08` and against the binary library instead.

## B. Root causes (proven, not inferred)

I extracted `USBPDCORE_PD3_FULL_CM7_wc32.a` and disassembled the actual
Thumb-2 machine code, because the header text does not reveal the gating.

### Defect 1 — `Is_EPR_Supported_SNK` never set (the primary cause)

`USBPD_PE_Send_ExtendeControlMessage` (`usbpd_pe.o` +0x682):

```
ldr   r1, [r3]          ; DPM_Settings
ldrh  r2, [r1, #8]      ; offset 8 == PE_PD3_Support  (offsetof verified = 8)
ubfx  r1, r2, #0xb, #1  ; bit 11 == Is_EPR_Supported_SNK  (verified = bit 11)
cbz   r1, skip          ; -> skips the queue, message never built
```

The flag was absent from `usbpd_dpm_conf.h`, so it defaulted to 0. Every
`EPR_Get_Source_Cap` call silently did nothing. Same pattern gates
`EPR_GETSNKCAPA` on bit 12 (`Is_EPR_Supported_SRC`).

This flag is also what makes the stack set the **RDO EPR_Capable bit**, which
PD3.1 §6.4.10.1 says the source *Shall* have seen in the most recent Request
before it will accept EPR mode entry.

**Fix:** `.Is_EPR_Supported_SNK = USBPD_TRUE` (SRC stays FALSE — sink-only board).

### Defect 2 — circular capability discovery

`src_epr_capable` was set **only** from EPR AVS PDOs. Those arrive only in
`EPR_Source_Capabilities`, which is only sent *after* EPR mode entry, which
required `src_epr_capable`. Unresolvable loop.

Per PD3.1 §6.4.1.2.2 the EPR Mode Capable bit is **B23 of the 5 V Fixed PDO in
the ordinary SPR `Source_Capabilities`**. New `APP_EPR_OnSprSrcCaps()` reads it
there, called from `APP_PD_StoreSrcPDO()`.

### Defect 3 — entry attempted from the wrong context

`USBPD_PE_Request_EPRModeEnter` (`usbpd_pe.o` +0x48e) requires
`Params & 0x704 == 0x300` — i.e. `PE_IsConnected` **and** an explicit contract
**in the sink power role** (bit masks verified by compiling the real struct).
Calling it from inside the PE notification callback returns BUSY. Entry is now
driven from `APP_PD_Task()` 300 ms after SNK_READY with an explicit contract,
re-armed on attach/detach/hard-reset.

## C. Files changed

| File | Change |
|---|---|
| `Appli/USBPD/Target/usbpd_dpm_conf.h` | **the fix** — enable `Is_EPR_Supported_SNK` |
| `Appli/Core/Src/app_epr.c` | `APP_EPR_OnSprSrcCaps`, `APP_EPR_ModeEnter/ModeExit`, `APP_EPR_StatusName`, honest CLI |
| `Appli/Core/Inc/app_epr.h` | new state fields, EPR bit macro, prototypes |
| `Appli/Core/Src/app_pd.c` | SPR EPR-bit ingest; task-loop EPR entry + re-arm |
| `Appli/Core/Src/app_cmd.c` | CLI help text |
| `tools/hosttest/*` | new tests + `pe_stub.c` |

**No ST middleware source was modified** — only the project's own DPM
configuration. The library was audited read-only.

## D. Honesty changes

The CLI no longer prints `"EPR request sent"`. It prints the real status, e.g.
`EPR_Mode(Enter): API status = USBPD_BUSY (not connected / no SPR explicit
contract yet) (3) -> NOT queued`, plus separate lines for the source's 5 V-PDO
EPR bit, whether EPR AVS PDOs were received, and whether RDO B21 will be set.

## E. Status per feature

| Feature | Researched | Implemented | Compiled | HW verified |
|---|---|---|---|---|
| EPR discovery (SPR B23) | YES | YES | host only | **NO** |
| EPR mode entry | YES | YES | host only | **NO** |
| EPR AVS parse / power maths | YES | YES | YES (133 tests) | **NO** |
| EPR KeepAlive / exit | YES | partial (ST-driven) | host only | **NO** |
| VDM / cable / extended msgs | audited | unchanged | — | **NO** |

## F. What I could NOT do — and why

**The firmware was not built or flashed.** This sandbox has no ARM toolchain
and cannot obtain one: GitHub release assets, developer.arm.com, Debian pools,
and all npm/pip binary toolchain packages are network-blocked (only registry
metadata resolves). `tools/verify.py` also needs build artifacts. There is
likewise no USB/ST-Link device in the sandbox, so no board could be flashed or
traced.

Therefore, per the brief's own standard, **EPR is NOT hardware-verified.**

Evidence I *can* offer: all changed files pass `gcc -Wall -Wextra` semantic
checks with the real project defines and ST headers; 133/133 host tests pass;
the root cause is proven at machine-instruction level.

## G. Next step for you

Build in your CubeIDE 2.2.0 and flash, then with an EPR source (28 V/5 A) and
an e-marked cable:

```
epr            -> "src 5V PDO EPR bit : SET" confirms Defect 2 is fixed
epr caps       -> should now report "-> queued to PE" (was silently dropped)
epr enter
```

On the CC capture you should now see `EPR_Get_Source_Cap`,
`EPR_Source_Capabilities`, and `EPR_Mode(Enter/Enter Acknowledged/Enter
Succeeded)` — none of which the previous firmware could emit. If `epr caps`
still reports `-> queued to PE` but no CC traffic appears, the next suspect is
the AMS/interruptible-state check in `PE_SNK_READY`, not the settings flag.


---

# H. First hardware run — results and four further fixes

## What the bench proved (the primary fix works)

`epr diag` with the EPR source attached, `DPM_Params = 0x08043BA2`:

```
PE connected        : yes          Is_EPR_Supported_SNK: 1 (enabled)
power role          : SNK          EPR_Get_Source_Cap : all gates PASS
contract            : EXPLICIT     EPR_Mode(Enter)    : all gates PASS
src 5V PDO EPR bit  : SET (source is EPR capable)
```

Both root causes from the audit are confirmed fixed:

* the gate that silently discarded every EPR message is **open**;
* SPR discovery now reports the source as EPR-capable, so the circular
  dependency is **broken** and RDO B21 will be set.

SPR regression clean: 6 PDOs listed, Request accepted, 5 V explicit contract,
INA226 5.020 V. USB CDC, UCPD, XIP, I2C2 all fine.

## Why EPR still did not complete — and it was not a gate

`EPR_Mode(Enter)` returned `USBPD_OK` and the source never replied.
Disassembling `EPRMode_Enter` explains it exactly:

```
blx  GetDataInfo        ; DataId 0x1E = RCV_REQ_COPYPDO
ldr  r0,[sp]
bfi  r5,r0,#0x10,#8     ; EPRMDO.Data   <- must be Sink Operational PDP
bfi  r5,#1, #0x18,#8    ; EPRMDO.Action <- 1 (Enter)
```

`USBPD_DPM_GetDataInfo` had **no case for 0x1E**, so it fell to
`default: *Size = 0` and the Data byte was never written. The board was
transmitting a malformed `EPR_Mode(Enter)`. PD3.1 Fig 6-32 requires that field
to be the EPR Sink Operational PDP. **Fixed.**

## A defect in my own instrumentation

The probe printed `PD TX 0 / PD RX 0 / GoodCRC 0` while PD was demonstrably
working. Those counters are incremented **only** inside `APP_PDCAP_Trace()`,
which lives behind `APP_ENG_CAPTURE`; this build is `cap=0`, so no funnel was
installed and the counters were structurally dead. My previous report treated
them as evidence — that was wrong. A minimal counter funnel is now installed
whenever the capture engine is absent.

## Fake success in the CLI

`USBPD_OK == 0`, so untouched fields rendered as successes:
`last GetSrcCap st : USBPD_OK` for a call never made, and
`last action: RESERVED`. Both now say **not attempted** / **none**.
`USBPD_CORE_DATATYPE_EPRMODE` is now handled, so an *Enter Failed* and its
reason code (e.g. `0x01 cable not EPR capable`) are reported instead of
dropped.

## Overstated capability

`sink PDP : 140 W` assumed 5 A unconditionally — through a 3 A cable to a
100 W source. Now derived from the live e-marker rating (3 A default, 5 A only
when Discover Identity confirms it): 28 V x 3 A = **84 W**.

## Cosmetic issues from your log

* INA226 `cfg reads 0x4907 (want 0x2907)` — bits 15..12 are reset/reserved and
  read back device-specifically. Only the operating bits (0x0FFF) are compared
  now; they matched all along.
* INA226 printed an identical idle line every second, burying PD output. Now
  prints on >=50 mV / >=20 mA change or a 15 s heartbeat.

## Status

| Feature | Researched | Implemented | Compiled | Flashed | HW verified |
|---|---|---|---|---|---|
| EPR gate open | YES | YES | host | YES | **YES** |
| EPR SPR discovery | YES | YES | host | YES | **YES** (bit SET) |
| EPR_Mode(Enter) well-formed | YES | YES | host | not yet | **NO** |
| EPR contract / KeepAlive / exit | YES | partial | host | not yet | **NO** |
| SPR / PPS / INA226 / CDC | — | unchanged | host | YES | **YES** |
