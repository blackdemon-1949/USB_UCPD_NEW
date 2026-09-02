# EPR `epr enter` System Freeze — Session Findings

> **Update 2026-09-03 (source-independent finding):** the freeze is **not** an
> EPR-charger/VCONN/cable-discovery artifact. It reproduces on **any** source
> (EPR-capable or plain SPR) the instant the `epr enter` command is sent while
> an explicit SPR contract is active. It does **not** happen when nothing is
> attached (`epr enter` then returns `USBPD_BUSY` cleanly and the console stays
> alive). This document tracks the current best model and the exact
> discriminator experiment for the next bench round.

## TL;DR of the current model

The stack and the application code around `epr enter` are all clean and
non-blocking **except** for the closed-library PE/PRL TX path that actually
transmits the queued `EPR_Mode(Enter)` AMS. Everything observable points at a
failure **inside that transmit / PE state-machine run**, not at a plain
vector-table crash:

| Observable | Value | What it rules in/out |
|---|---|---|
| `epr enter` with **no source** | clean `USBPD_BUSY`, console alive | crash needs an attached source + SPR explicit contract |
| `epr enter` with any source | board appears frozen; no response on CDC **and** USART1 trace | PE/PRL/TX path wedged, OR console path wedged |
| LED after freeze | ~250 ms continuous blink (`APP_LED_PD_WAIT`) | **main loop still runs**; PE believes CC attached but **no explicit contract** (stuck pre-contract / negotiation) |
| `***FAULT` live line | never observed | no proof yet of vector-table HardFault |
| `PREVIOUS RUN FAULTED` on reboot | not observed by user | no BKPSRAM fault record yet |
| Debugger | available but user prefers not to use it | rely on serial experiments |

The 250 ms `APP_LED_PD_WAIT` blink is decisive in one direction: it is
programmed only in `APP_PD_OnCable(ATTACHED)` and on hard-reset notification
(see `app_pd.c`), and `APP_LED_Task()` runs only from the **main loop**. So the
main loop **keeps executing** after `epr enter`. A vector HardFault would jump
to the blink loop with `__disable_irq()` and a completely different LED
pattern (2 pulses + pause for HardFault code 2). A UCPD-ISR DMA hang would
freeze the LED solid. Neither is observed.

Yet the console does not answer. That combination points to one of:

1. **The main loop is spending nearly all its time inside
   `USBPD_DPM_Run()`** — i.e. `USBPD_PE_StateMachine_SNK()` (or the TX
   transmission inside it) hangs, or loops, or takes an extremely long time.
   The CLI poll / CDC flush / LED update would still run **sometimes**,
   producing a ~250 ms-looking blink, but a human typing a command in the few
   percent of available time never gets a response. EPR_Mode(Enter) is an
   **extended (chunked) message**, transmitted from the PE task context
   through the UCPD TX DMA with `while (CCR & EN)` spin-waits in
   `usbpd_phy_hw_if.c` `USBPD_HW_IF_SendBuffer()` and in the UCPD ISR
   (`usbpd_hw_if_it.c`). A stuck DMA channel / lost TX-complete / repeated
   chunked-message retry loop would produce exactly this symptom and **no**
   `***FAULT` line.
2. **The USB CDC host side stopped polling** (host/terminal wedged, or a
   missed IN-transfer with the 250 ms watchdog constantly resetting) while the
   board itself is alive and running normally. `APP_LOG_Flush` has a 250 ms
   IN-transfer watchdog and non-blocking ring, but if the host closes its
   terminal without a USB detach, the board can look dead while actually
   running.
3. A vector HardFault *inside* an IRQ that also re-enables/keeps running (not
   consistent with the LED evidence, since our fault handlers blink a distinct
   code and spin forever).

The three mechanisms are cleanly separated by the **serial-isolation
experiment** below.

## Facts established

- `epr enter` executes in the **main loop** (CDC CLI poll), not in an ISR.
- The app path (`app_cmd.c` → `APP_EPR_Cmd` → `APP_EPR_ModeEnter` →
  `USBPD_PE_Request_EPRModeEnter`) is clean: no waits, no IRQ games, bounded
  state. `APP_EPR_Ctx`, `APP_PD_Port`, the log ring (8 kB, non-blocking with a
  250 ms TX watchdog), and the CDC RX ring are all bounded and watchdogged.
- The PE (no-OS integration): `USBPD_DPM_Run()` runs one cooperative slice per
  main-loop pass — CAD, then per-port `USBPD_PE_StateMachine_SNK()`, then
  `USBPD_DPM_UserExecute()`. `USBPD_PE_TaskWakeUp` only clears
  `DPM_Sleep_time[port]` in no-OS builds (benign).
- Library decode (IAR objects from `USBPDCORE_PD3_FULL_CM7_wc32.a`, via
  `/home/user/scratch/venv`, capstone Thumb):
  - `USBPD_PE_Request_EPRModeEnter` gates on PE status/state; on acceptance it
    writes PE state byte `0xa8` (SEND_EPR_MODE), stores `1`, wakes the PE task,
    returns 0.
  - PE EPR AMS tables live in `usbpd_pe_epr.o`; the SNK entry table rows
    (13-byte rows with state codes 0xa8/0xa9/…) reference handlers
    `EPRMode_Enter`, `EPRMode_EnterAnswer`, etc. — all present. Static bounds
    so far look sound.
- UCPD TX is DMA-driven (GPDMA1 CH1), with **spin-waits on
  `hdmatx->CCR & EN`** in `USBPD_HW_IF_SendBuffer()` (PE-task context) and in
  the UCPD ISR for TXMSGDISC/TXMSGSENT/TXMSGABT. If a channel ever stays EN
  after `SUSP|RESET`, that code spins forever.
- UCPD ISR / main-loop liveliness: heartbeat LED (1 s) and PD_WAIT LED
  (250 ms) only run from the main loop; the UCPD ISR would freeze them solid.
- Fault-capture build: `***FAULT code=N CFSR=… HFSR=… PC=… LR=…` on USART1
  (PB6/PB7 @ 921600) at the fault, plus a BKPSRAM record
  (`0x38800200`) printed as `*** PREVIOUS RUN FAULTED` on the next boot.

## Decisive next experiment (takes ~2 minutes)

Two tests, done on the newest fault-capture build with a real source attached
and a normal contract established (LED solid = PD_CONTRACT):

### A. Is the console path wedged, or the whole system?
After `epr enter` appears to freeze the CDC console:

1. **Close the serial terminal completely** (disconnect the CDC COM port /
   close PuTTY/Tera Term) and **wait 2 seconds**.
2. Reopen the terminal (or reconnect). If you now get a **fresh banner +
   prompt** (or a backlog of lines), the board was alive the whole time and
   the freeze was a **host/CDC TX wedge** — the PD stack may be perfectly
   fine. That changes the fix completely (console path, not PE).
3. If the reopened terminal is **still dead**, the board really is stuck →
   proceed to B.

### B. Does the PE task ever return?
While the console appears dead, type `epr enter` again, then **press the
board reset button** and watch the USART1 trace terminal (not CDC): if the
`*** PREVIOUS RUN FAULTED` banner appears, the freeze was a vector fault and
the record now contains the exact PC. If no banner appears, the CPU is
spinning somewhere with interrupts disabled or in a non-faulting loop.

### C. USART1 trace observation
The `***FAULT` live print, when it happens, goes to **USART1 PB6/PB7 @ 921600**
(the PD trace UART) — NOT the CDC console. If the terminal software can run
both, keep USART1 open and watch it during the freeze.

## History of what was tried / refuted

- ~~EPR-charger-only / source VCONN swap / cable discovery~~ — **refuted**:
  reproduces on plain SPR sources.
- ~~Plain vector HardFault kills everything (LED solid / 2-pulse blink)~~ —
  inconsistent with the observed 250 ms blink continuing.
- ~~DMA stop-spin in the UCPD ISR~~ — would freeze the LED solid; not
  consistent unless the blink observation was of a different pattern.

## What to fix (depends on experiment A/B)

- If A shows a **host/CDC TX wedge**: fix the console path (the 250 ms
  watchdog / IN-transfer latching) — the PE never crashed.
- If B shows **no fault record but the system is stuck**: instrument
  `USBPD_DPM_Run()`/`USBPD_PE_StateMachine_SNK()` with a per-call entry/exit
  cycle marker on USART1 (bounded, register-level) to find whether the PE
  call ever returns; if it does not, the next step is to isolate
  `USBPD_HW_IF_SendBuffer()` and the UCPD TX DMA state at that moment.
- If a fault record appears: use the captured PC + `addr2line` against the
  Appli ELF.

## Build / flash / test reminders

- Branch: `arena/01a06344-usb-ucpd-new` (this branch).
- Build the Appli in CubeIDE (GNU ARM 14.3.1); the trace UART is USART1
  PB6/PB7 @ 921600. CDC console is the USB HS CDC.
- Host gate: `python3 tools/hosttest/run.py` → 149/149. Edited files must stay
  `-Wall -Wextra` clean. No HARDWARE VERIFIED without `Enter Succeeded` **and**
  the board still alive afterward.
