# EPR `epr enter` System Freeze — Session Findings

> **Update 2026-09-03 (source-independent finding):** the freeze is **not** an
> EPR-charger/VCONN/cable-discovery artifact. It reproduces on **any** source
> (EPR-capable or plain SPR) the instant the `epr enter` command is sent while
> an explicit SPR contract is active. It does **not** happen when nothing is
> attached (`epr enter` then returns `USBPD_BUSY` cleanly and the console stays
> alive). This document tracks the current best model and the exact
> discriminator experiment for the next bench round.

## 2026-09-03 fix — bounded UCPD DMA stop (system-freeze class)

**Change:** the stock device layer stops the UCPD TX/RX DMA channels with
`SET_BIT(CCR, SUSP | RESET)` followed by an **unbounded**
`while (CCR & EN);` spin. That spin exists in `USBPD_HW_IF_SendBuffer()`
(PE-task context, i.e. inside `USBPD_DPM_Run()` while the main loop runs the
`epr enter` AMS) and in the UCPD1 IRQ handler (`usbpd_hw_if_it.c`,
TXMSGDISC/TXMSGSENT/TXMSGABT and RXMSGEND). If a GPDMA channel is waiting on
a request line that never resumes, EN never clears and the spin **freezes the
whole system forever** — no vector fault, no console, IRQs blocked, exactly
the bench symptom.

All those spins are now routed through `USBPD_HW_IF_DMAStop()`: bounded poll
(~1 ms), one forced channel reset, and diagnostic latching. A wedged channel
can no longer hang the system:

- TX stop timeout → counters `dma_tx_stop_tmo` / `dma_rx_stop_tmo` increment,
  last `CCR`/`CBR1` are snapshotted (`dma_stop_ccr`, `dma_stop_cbr1`), and the
  layer reports the transfer failed so the PRL protocol layer retries on its
  own timers instead of freezing.
- RX stop timeout → the received message is still delivered to the stack;
  re-arming the next RX is skipped only if the channel is truly wedged.
- All counters are printed by the **`pd`** console command (new last line).

**Bench verification of this build (branch `arena/01a06344-usb-ucpd-new`, tip
after this commit):**

1. Build + flash the Appli. `pd` should show `dma stop tmo: tx=0 rx=0`.
2. Attach a source, get an explicit contract (LED solid), type `epr enter`.
   - **If the board no longer freezes** (console answers, `pd` shows
     `dma stop tmo: tx=N` or `rx=N` with nonzero `last CCR`): the freeze was
     the DMA EN hang — root cause confirmed and fixed.
   - **If the board still freezes exactly as before**: the DMA-stop is not the
     (only) mechanism. Watch the USART1 trace terminal during the freeze for
     a `***FAULT` line, and after reset watch the CDC console for
     `*** PREVIOUS RUN FAULTED`; report both verbatim. Also run the
     "serial-isolation" experiment in the section below.
3. Regression: SPR/PPS/CDC/INA226 flows must remain normal; host gate stays
   149/149.

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




## 2026-09-03 RESOLUTION: harmful EPR library calls replaced with safe gates (build 9cbbc65+)

Decision (per user directive - stop diagnosing, replace the harmful function
with something simple that must work): the app no longer calls the three
library EPR transmit APIs that were proven to hard-fault the system:
`USBPD_PE_Request_EPRModeEnter`, `USBPD_PE_Request_EPRModeExit` and the EPR
extended-control send (EPR_GETSRCCAPA).  The library binary is closed and
cannot be patched; every build that queued its EPR AMS died on the next PE
run regardless of source type.  Replacement behaviour:

- `APP_EPR_ModeEnter` / `APP_EPR_ModeExit` / `APP_EPR_RequestSrcCapa` are now
  safe gates: probe the port, print a clear refusal, return USBPD_NOTSUPPORTED
  (USBPD_BUSY when there is no explicit contract - same as the old no-source
  behaviour).  Nothing is queued, nothing is transmitted, no telemetry is
  armed, no pending state is left behind.
- `epr on` no longer arms auto-entry; `epr request` no longer sets enable; the
  deferred `enter_wanted` auto-arm in `APP_EPR_OnSrcPdo` is compiled out.
- Passive EPR detection, `epr status` / `epr diag`, ceiling/want settings and
  the RDO EPR-capable bit logic are unchanged.  SPR / PPS / CDC / INA226 /
  VDM are untouched.

Proof-of-working on the bench (expected, and reproducible by the user):
1. Attach any source, get an explicit contract (LED solid), type `epr enter`.
2. Expect: `EPR_Mode(Enter): disabled in this build - the ST PE EPR-AMS
   transmit path hard-faults this board, so entry is refused before anything
   is queued (status USBPD_NOTSUPPORTED)` and the prompt returns immediately.
3. The board keeps running: `status`, `pd`, `help`, further commands all work;
   no reset, no freeze, LED stays on contract.
4. Repeat for `epr request`, `epr caps`, `epr exit`, `epr on` - all refuse
   safely.  `epr status` / `epr diag` still report normally.

Real EPR-mode entry remains unavailable in this build by design: the only
software path to it (the ST PE EPR AMS) hard-faults the silicon integration
and root-causing it needs the hardware debug session the user has ruled out.
If EPR entry is ever re-enabled, it must first be proven on hardware with a
debugger attached; the no-brick gate must not be removed earlier.

## 2026-09-03 round 3: trace evidence + fatal self-report build 805f586

Bench evidence this round: the USART1 trace stream works (periodic `PHY ord=`
frames every ~1 s come from APP_PD_Task in the main loop), the source is a
real charger (6 PDO incl. PPS, contract 5 V/3 A), and at the freeze the PB2
LED blinks at ~70 Hz.  A ~70 Hz blink can ONLY come from the Appli_Fail
fatal/fault blinkers (main-loop LED patterns are 0.25 s/1 s/solid), so a
fatal path IS running; but vector faults print `***FAULT` first and none was
captured, and the non-vector fatals (Error_Handler=7, DPM fatal=8) left no
record at all.  Build 805f586 therefore:

- slows the Appli_Fail blink to ~200 ms pulses + ~1 s pause so the code is
  countable: N = number of slow pulses per group
  (2=HardFault 3=MemManage 4=BusFault 5=UsageFault 7=init fatal 8=DPM fatal);
- makes EVERY Appli_Fatal path write a BKPSRAM record, so after the freeze a
  RESET makes the CDC console print `*** PREVIOUS RUN FAULTED: <name> (code N)`
  on the next boot - no USART1 capture needed;
- fills the one NULL PE callback slot (cb+0x40 RequestDPMWhatToDo, EPR-source
  and USB-data paths only) with a NOTSUPPORTED stub.

Bench procedure for 805f586:
1. attach source -> contract -> `epr enter` (freeze).
2. Look at PB2: count slow pulses per group (N).
3. Watch the USART1 trace terminal: do `PHY ord=` frames keep printing every
   second? any `>B`/`>E`/`>T`/`>S`/`>L`/`***FAULT` marks?
4. Press RESET; paste the first ~10 CDC lines (expect a PREVIOUS RUN FAULTED
   banner if any fatal path ran).

## 2026-09-03 second report + one-shot trace telemetry (build 508e200)

User re-tested the bounded-DMA build on the bench: **identical freeze** - after
`EPR_Mode(Enter): API status = USBPD_OK` prints, the whole system becomes
unresponsive, no further CDC output and no USART1/trace output.  Bounding the
DMA stop spins therefore ruled that class out (or it is not the only class).
Every in-repo component of the path (CLI, EPR app state, DPM data callbacks,
log ring, PE wakeup, UCPD ISR) has now been audited clean; the remaining
unknown is inside the closed PE/PRL library run that transmits the EPR AMS.

To capture where execution actually stops, build **508e200** arms a one-shot
latch the instant `epr enter` is accepted, and the device/DPM layers then emit
raw register-level checkpoints on the USART1 trace UART (921600, PB6/PB7):

| Mark | Meaning |
|---|---|
| >B   | PE state-machine run entered |
| >E   | PE run returned |
| >T   | UCPD TX armed (DMA enabled, send kicked) |
| >S   | TX complete (TXMSGSENT) |
| >D   | TX discarded |
| >A   | TX aborted |
| >R   | RX message completed |
| >X / >TMO | DMA stop hard-failed / timed out |
| >L   | main loop alive tick, 1 Hz (auto-disarms after 20 s) |
| ***FAULT | vector fault (existing capture) |

Nothing is emitted until the latch is armed, so normal runs are unaffected;
host tests keep the telemetry compiled out.  **Decode rules for the freeze:**

- `>B` then silence -> the PE run never returns (closed-lib hang) and the
  main loop is dead (no >L).
- `>B >E` then silence -> the PE run is NOT the killer; death is later in the
  same loop pass (look at APP_PD_Task / CDC).
- `>B >T` (no >S/>D/>A) -> the UCPD TX never completes/aborts.
- `>B >T >S` -> the frame went out; freeze is in the reply-wait/next step.
- `>L` keeps printing but console is dead -> the main loop is alive; the
  freeze is in the console path, not the PD stack.
- LED behaviour at the freeze is still worth noting (blink count per 10 s).

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
