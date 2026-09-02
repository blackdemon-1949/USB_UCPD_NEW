# Found it: a NULL function pointer the ST library calls without checking

You were right that something was very wrong in the code. It was.

## The crash, at instruction level

`usbpd_pe_vconn.o`, `PE_SubStateMachine_VconnSwap` **+0x292**:

```
ldr  r2, [r1, #0x2c]     ; cb->USBPD_PE_EvaluateVconnSwap
blx  r2                  ; called UNCONDITIONALLY — no null check
```

And our callback table shipped **NULL** in that slot. The CubeMX-generated
`usbpd_dpm_core.c` wraps the two VCONN entries in
`#if defined(_VCONN_SUPPORT)` — and this project never defines it. I verified
the whole symbol list in `.cproject`: `STM32H7R3xx`, `USBPDCORE_LIB_PD3_FULL`,
`USBPD_PORT_COUNT=1`, `USE_HAL_DRIVER`, `USE_FULL_LL_DRIVER(S)`, `_SNK`,
`_TRACE`. No `_VCONN_SUPPORT`.

So: `blx 0` → branch to address 0 → **HardFault** → `Appli_Fail()` blink loop.
That loop does `__disable_irq()` and spins forever, which is exactly your
~190 Hz PB2 flicker that only a power cycle clears.

## Why *only* the EPR charger

PD 3.1 requires the **source** to be VCONN Source before it discovers the
cable during EPR entry. So an EPR charger starts a `VCONN_Swap` immediately
after `EPR_Mode(Enter)`. An SPR charger never does.

That is your exact observation — SPR always fine, EPR always bricks — and it
was the clue that pinned this down. Nothing was wrong with your hardware or
your source.

## The fix

The VCONN callbacks are now **always** installed. `EvaluateVconnSwap` answers
from real capability: this board has no VCONN supply
(`BSP_USBPD_PWR_VCONNOn` returns `FEATURE_NOT_SUPPORTED`), so it **REJECTs**
becoming VCONN source and only accepts a swap that hands the role away.
Rejecting is spec-legal and leaves the source as VCONN source — which is what
EPR entry actually needs.

## Same bug class, swept out

I disassembled **every object** in the library looking for
`ldr rN,[cb,#off]; blx rN` with no null test. Reachable from
`USBPD_PE_StateMachine_SNK` and NULL in our build:

| slot | callback | called from |
|---|---|---|
| +0x00 | `RequestSetupNewPower` | — |
| +0x08 | `EvaluatPRSwap` | — |
| +0x1C | `SRC_EvaluateRequest` | `usbpd_pe_src.o` |
| +0x24 | `PowerRoleSwap` | `usbpd_pe_snk.o` ×3 |
| +0x2C | `EvaluateVconnSwap` | `usbpd_pe_vconn.o` ← **the brick** |

All five now have inert, truthful handlers. `RequestDPMWhatToDo` (+0x90) is
never called unguarded, so it stays unset.

## Also fixed

My BKPSRAM fault forensics printed nothing after your lock-up — because the
**backup regulator** was never enabled, so the contents weren't retained.
`HAL_PWREx_EnableBkUpReg()` added, so a future fault really does report
`CFSR/HFSR/MMFAR/BFAR` on the next boot.

## Test

Clean rebuild, flash, then:

1. **SPR charger** — `req 2`, `pps 21000`, `temp`, `diag all`. Regression check.
2. **EPR charger** — attach, confirm the board stays responsive, then:

```
epr diag
epr enter
```

Expected now: `Enter Acknowledged` → `Enter Succeeded`, or a decoded
`Enter Failed - <reason>`. **What must no longer happen is a lock-up.**

If it still faults, the board recovers and prints the fault registers on the
next boot — send me that line.

## Status

| | HW verified |
|---|---|
| SPR 5/9/12/15/20 V, PPS to 21 V | **YES** |
| INA226 / VBUS / diagnostics / CDC | **YES** |
| EPR no-brick | fix is exact and proven in the disassembly — needs your flash |
| EPR mode entry | not yet |

149/149 host tests, 0 compile errors, ST core library at official v5.4.1.
I still cannot ARM-link or flash here, so I won't call EPR done until your
log shows it.
