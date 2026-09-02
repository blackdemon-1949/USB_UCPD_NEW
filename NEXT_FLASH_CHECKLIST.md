# Next flash — the SPR/PPS regression was mine, and it is reverted

## What broke, precisely

`REQUEST not accepted by stack (3)` for **every** voltage — 9 V, 12 V, 20 V,
PPS and EPR — was caused by my previous commit setting
`PE_VconnSwap = USBPD_FALSE`.

My reasoning was wrong on two counts:

1. VCONN for the cable e-marker is supplied by the **source** through the
   cable, not by this board's rail. Accepting the swap costs us nothing.
2. That flag feeds `USBPD_DPM_EvaluateVconnSwap()` directly, which turns it
   into a **REJECT**. Rejecting made the source hard-reset in a loop.

The consequence is visible in your own counters: `hard_reset 6`,
`neg_accept 0`, and `PE_Power` stuck at `DEFAULT 5V`. And
`USBPD_PE_Send_Request` (`usbpd_pe.o +0x526`) gates on the **same**
`(Params & 0x704) == 0x300` as the EPR entry points:

```
ubfx r3,r7,#0xc,#1        ; PE_IsConnected
movw r3,#0x704 ; ands r7,r3 ; cmp.w r7,#0x300
movs r4,#3                ; -> USBPD_BUSY
```

No explicit contract ⇒ every request returns 3. One bad flag took out the
whole request path. **Reverted to `USBPD_TRUE`**, matching ST's own H7RS
reference sink, which I cloned from `STM32CubeH7RS` and diffed against ours.

## Everything else fixed this round

| Area | Change |
|---|---|
| **Dead diagnostics** | `APP_ENG_CAPTURE=0` meant the ST trace funnel was never installed, so `pd_rx/pd_tx/goodcrc` froze and every negotiation counter read 0 while PD ran. Analyzer engines now **on** |
| **Counters that could not count** | `attach`, `detach`, `cad_event`, `neg_caps`, `neg_request`, `neg_accept`, `neg_reject`, `neg_wait`, `neg_contract`, `hard_reset`, `goodcrc_tx` now wired to real events, classified from the message header |
| **VBUS lie** | Synthetic VBUS was set to the **requested** voltage the instant an RDO was built, so between Accept and PS_RDY the PE believed the rail had already moved. INA226 measurement is now the default source (`ina vbus synth` restores the old behaviour) |
| **Unhelpful errors** | A refused request now prints the missing precondition, not just `(3)` |

## Engines now enabled

`cap=1 txn=1 ext=1 anal=1 cable=1 epr=1 vdm=1 diag=1`

Left **off** deliberately, as you asked (test-only / instability):
`FUZZ` (deliberately transmits malformed frames), `TEST` (on-target vector
suite), `STORE` (backup-SRAM persistence).

## Build

```bash
export PATH="<CubeIDE>/plugins/...gnu-tools-for-stm32.14.3.rel1.../tools/bin:$PATH"
arm-none-eabi-gcc --version      # must be 14.3.1
python3 tools/build.py --project all --config all --jobs 8
python3 tools/verify.py
```

Flash unchanged (external loader, XIP `0x90000000`).

## What to check, in order

```
epr diag        # BEFORE attaching anything
```
then attach the source and let it settle, then:

```
diag all        # attach/cad_event/neg_caps/neg_accept must now be NON-zero
req 2           # 9 V   <- these must work again first
volt 20000      # 20 V
pps 15789 5000  # PPS
epr diag        # contract must read EXPLICIT CONTRACT
cable status    # e-marker identity
```

**If `req 2` still fails, stop there and send me the output** — `epr diag` and
the failure message now name the exact missing precondition, so no guessing is
needed. EPR cannot work until SPR requests do, because they share the gate.

## Honest status

| Stage | SPR/PPS | EPR |
|---|---|---|
| RESEARCHED | YES | YES |
| IMPLEMENTED | YES (regression reverted) | YES |
| COMPILED | host only — 148/148 tests, 0 errors, **no ARM link here** | same |
| FLASHED | not yet | not yet |
| HARDWARE VERIFIED | **regressed last flash, fix unproven** | **NO** |

Preserved: DMA map, linker script, MPU layout, XIP, USB CDC (your Code 10 fix
stays), INA226. ST library byte-identical (`2fb9c3f7…`).
