# The brick is found and fixed — plus the ST library was out of date

## Why the board locked up after `EPR_Mode(Enter)`

It was **not** bricked hardware and **not** the charger. It was a hard fault
caused by re-entering the policy engine from inside its own callback.

`APP_EPR_OnSrcPdo()` called `APP_EPR_ModeEnter()` directly. That function runs
inside `USBPD_DPM_SetDataInfo` — i.e. on the PE's own call stack while it is
mid-message. Calling `USBPD_PE_Request_EPRModeEnter()` there re-enters the
state machine already executing and corrupts its AMS bookkeeping.

Every fault handler in this project was a bare `while(1)`, so the fault looked
exactly like your symptom: console dead, no reaction to unplugging the source,
physical reset button required.

**Fixed:** entry now sets a flag and `APP_PD_Task()` performs it in task
context. This was my bug, introduced when I added auto-entry.

## Your hardware *does* support EPR — and the stack was stale

You were right to push on this. Your EPR charger set the EPR bit and the board
got all the way to `EPR_Mode(Enter) ... ACCEPTED by PE`, which is exactly the
point the fault hit.

I also found the shipped ST core library is an **older build**:

| | size | md5 |
|---|---|---|
| was | 341 634 | `2fb9c3f7…` |
| now | 344 742 | `82418ccd…` |

Upgraded to **official ST v5.4.1** (`stm32-mw-usbpd-core`, commit `aafa359`).
Its release notes list precisely the EPR defects we hit:

* *"EPR Mode Entry message sent by SNK, should contain EPR SNK Operational PDP
  value in Data field"* — the exact `DataId 0x1E` bug I found by disassembly
* *"EPR SRC state machine should check conditions on PDO and RDO are fulfilled
  prior answering Enter Acknowledged"*
* *"Correct Size value when storing received EPR PDOs"*
* *"Update management of SRC_CAP message by SNK when in EPR mode"*
* *"Trig Hard Reset when SPR SRC Capabilities is received while in EPR mode"*

**Verified safe before swapping:** identical exported symbols, EPR gates at the
same context offsets (so the boundary probe stays valid), and the two changed
headers only add types — the renamed `GiveBackFlag`/`SourceInfoDO` are used
**0 times** in this application. Compiles clean, 148/148 tests pass.

## Also fixed

| Item | Fix |
|---|---|
| Silent lock-ups | Fault handlers latch CFSR/HFSR/MMFAR/BFAR and blink a code (2=Hard, 3=MemManage, 4=Bus, 5=Usage) instead of dying quietly |
| Stack headroom | 4 KB → 8 KB in the **active** script only. EPR is the deepest call chain here and `APP_LOG_Printf` adds 256 B/frame. DTCM is 64 KB and holds only the stack. MPU/RAM layout, heap placement and ASSERTs untouched |
| `temp` missing | DTS decoupled from `APP_ENG_ANALYTICS` |

## Build and flash

```bash
python3 tools/build.py --project all --config all --jobs 8
python3 tools/verify.py
```

**Do a clean rebuild** — the middleware headers changed.

## What to check

With the **SPR-only** charger (regression):
```
req 2 / volt 20000 / pps 21000     # must still work
temp                                # must report a temperature now
diag all                            # cdc_tx_stalls, log_dropped = 0
```

With the **EPR** charger — this is the real test:
```
epr diag        # gates
epr             # 'src 5V PDO EPR bit' should be SET
```
Then just wait. Auto-entry fires ~300 ms after the explicit contract. Expected:

```
EPR_Mode: source replied Enter Acknowledged
EPR_Mode: source replied Enter Succeeded - EPR mode active
```

**The board must stay responsive either way.** If a fault does occur you'll now
see the LED blink a code instead of silence — tell me the count.

## Honest status

| Feature | Implemented | HW verified |
|---|---|---|
| SPR 5/9/12/15/20 V | YES | **YES** |
| PPS / AVS (to 21 V) | YES | **YES** |
| INA226, real VBUS | YES | **YES** |
| Diagnostics counters | YES | **YES** |
| CDC stability | YES | partly (Code 10 gone) |
| DTS temp | YES | not yet |
| **EPR entry** | YES — brick fixed, lib upgraded | **NOT YET** |

I cannot ARM-link or flash here, so I will not claim EPR works until your
capture shows `Enter Succeeded`. What I can say: the crash mechanism is
identified and removed, and the stack is now the current ST release with the
EPR fixes in it.
