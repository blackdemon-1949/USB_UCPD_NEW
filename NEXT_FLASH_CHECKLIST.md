# EPR crash: found, and the board can no longer brick itself

## The LED flicker told us what it was

That fast PB2 blink **is** `Appli_Fail()` — the fault handler I added last
round. So the board is not hanging or losing power: it takes a **CPU fault**
and parks in the blink loop. That narrowed everything.

## Defect 1 — I used the wrong DataId

`EPRMode_Enter` does `movs r1,#0x1e`. Last round I mapped that to
`RCV_REQ_COPYPDO` by hand-counting the enum, and **the count was wrong** — it
skipped the `#if`-guarded members. Compiling the real v5.4.1 enum gives:

| value | actual constant |
|---|---|
| **0x1E** | `SNK_PDP_EPR` ← what Enter really asks for |
| 0x1C | `RCV_REQ_COPYPDO` |
| 0x17 | `EPRMODE` |
| 0x19 | `SNK_PDO_EPR` |

The bogus case is removed and every size is now justified against the
disassembly that establishes it.

## Defect 2 — the EPR sink list was unbounded

`Send_EPR_SnkCapa` does `memclr(ctx+0x14, 0x104)`, writes the SPR PDOs at
`ctx+0x14`, then asks the DPM for `SNK_PDO_EPR` at `ctx+0x30` — exactly slot 8,
as PD3.1 requires. Our handler answered with **one** 4-byte AVS object and no
bound at all.

Replaced with `APP_EPR_GetSinkEprPdos()`: a correctly shaped EPR sink list
(28/36/48 V Fixed below your ceiling, then one AVS APDO), hard-bounded by
`USBPD_MAX_NB_EPRPDO` so it can never overrun the library's buffer. Current
comes from the e-marker, so it never claims 5 A on a 3 A cable.

## The board can no longer brick itself

**Automatic EPR entry is now OFF by default.** That is the important change.
Previously it armed itself the instant an EPR source attached — so when entry
faulted, the board rebooted, auto-entered, and faulted again. An
unrecoverable loop needing the reset button. A feature that can brick the
instrument must not arm itself.

* `epr` / `epr diag` — detection and reporting, always automatic
* `epr enter` — **one** manual attempt
* `epr on` — arm automatic entry (only after a manual attempt succeeds)

I also split `enable` (auto-enter) from `allow` (advertise EPR capability).
Gating the RDO bit on `enable` would have made a manual entry fail with the
source's own reason `0x03, EPR Mode Capable bit not set in the RDO`.

## Crash forensics — no more guessing

Fault handlers now stash the fault code plus **CFSR / HFSR / MMFAR / BFAR** in
BKPSRAM, which survives a warm reset, and the next boot prints:

```
*** PREVIOUS RUN FAULTED: HardFault (code 2)
    CFSR=0x........ HFSR=0x........ MMFAR=0x........ BFAR=0x........
```

That decodes to the exact fault type and faulting address.

## Test sequence — please follow this order

Clean rebuild (middleware headers changed), flash, then:

**1. SPR charger — regression.** `req 2`, `pps 21000`, `temp`, `diag all`.
Everything that worked must still work. Nothing auto-enters now.

**2. EPR charger — attach and WAIT.** The board must stay fully responsive
and must **not** enter EPR on its own. Run `epr` and `epr diag`.

**3. Only then:** `epr enter`

If it still faults, the board will come back and print the fault registers on
the next boot. **Send me that line** — it identifies the faulting instruction
directly.

## Honest status

| Feature | HW verified |
|---|---|
| SPR 5/9/12/15/20 V, PPS to 21 V | **YES** |
| INA226, real VBUS, diagnostics | **YES** |
| CDC stability (Code 10 gone) | **YES** |
| No-brick guarantee with EPR source | expected — not yet proven |
| EPR mode entry | **NOT YET** |

I cannot ARM-link or flash here. What I will commit to: the fault is now
**self-reporting**, and with auto-entry disarmed an EPR charger should no
longer be able to lock the bench. 149/149 host tests, 0 compile errors,
ST core library at official v5.4.1.
