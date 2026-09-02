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
