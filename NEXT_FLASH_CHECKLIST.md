# Status after the last bench run

## SPR / PPS / AVS: WORKING — hardware confirmed

Your log proves it, and this is the first run where I can say that honestly:

| Request | Contract | INA226 measured |
|---|---|---|
| `req 2` (9 V) | `explicit contract 9000 mV / 3000 mA` | **8.981 V** |
| `volt 20000` | `explicit contract 20000 mV / 5000 mA (PDO 6)` | **20.021 V** |
| `pps 15789 5000` | `explicit contract 15789 mV / 5000 mA` | **15.766 V** |

The VCONN-swap revert fixed the regression I introduced. Counters are real
now too: `neg_accept 13`, `neg_contract 9`, `attach 1`, `pd_tx 41`.

## EPR: your charger cannot do it, and the firmware is right

`epr` reported **`src 5V PDO EPR bit : clear (SPR-only source)`** — that is
correct, not a bug.

Your source advertises `5/9/12/15/20 V` and `PPS 3.3–21 V`. Maximum
**20 V × 5 A = 100 W**, no 28/36/48 V PDO.

PD 3.1 defines EPR as **above 100 W**, requiring a 28/36/48 V Fixed PDO *and*
the EPR Mode Capable bit in the 5 V PDO. This charger is an SPR-only PD 3.0
supply. **No firmware change — mine or ST's — can make it deliver EPR.** The
stack is genuinely PD 3.1: `usbpd_pe_epr.o` is linked, all five ST gates pass,
and the board correctly declines to send `EPR_Mode(Enter)` to a source that
never claimed EPR. Sending it anyway would be the fake behaviour you told me
to avoid.

`epr` now states the reason, quoting the source's own numbers.

### About the WeAct PowerMonitor

It doesn't do EPR either. Its own protocol spec (`com_PowerMonitorMiniV1.py`,
`CMD_PD_PDO_AVS = 0x0B`) *reads* AVS descriptors — `{"minvoltage": 15000,
"maxvoltage": 28000, "maxpower": 240}` in the docstring is an **example of the
data format**, not something it negotiated. It reports what a charger
advertises, exactly as our `caps` does. To see EPR you need a genuine EPR
charger: 140 W+ with a 28 V PDO (e.g. Apple 140 W, Framework 180 W, Delta
240 W) **and** a 5 A e-marked cable.

## Fixed this round

| Problem | Cause |
|---|---|
| **CDC unusable again** | Real deadlock: `s_tx_busy` is cleared only by `CDC_TransmitCplt`; if that is ever missed the console dies permanently. Added a 250 ms watchdog + `cdc_tx_stalls` counter |
| **Console flooding** | INA226 printed every second, corrupting PD output and keeping the IN endpoint busy. Periodic printing now **off** by default (`ina` on demand, `ina auto on` to restore). Sampling still runs for VBUS |
| **`temp` never worked** | DTS was clocked from **LSE**, but Appli configures no oscillator and no 32 kHz crystal exists → LSE never ready. Switched to **PCLK**. Also stopped a DTS failure calling `Error_Handler()`, which would kill PD/CDC/CLI |
| **`engines` unknown** | Command added |

## Engines

ON: `capture` `cable` `epr` `vdm` `diag` (+ DTS temp, INA226, PPS/AVS).
OFF: `txn` `ext` `analytics` — enabling all three together is what brought the
CDC instability back; `analytics` polls I2C and formats stats in the
super-loop. `fuzz`/`test`/`store` remain off.

## Build & flash

```bash
python3 tools/build.py --project all --config all --jobs 8
python3 tools/verify.py
```

## Check after flashing

```
engines          # confirm profile
temp             # must now report a temperature, not "not started"
req 2 / volt 20000 / pps 15789 5000
diag all         # cdc_tx_stalls and log_dropped must stay 0
cable status     # e-marker
epr              # will state why EPR is unavailable with THIS charger
```

## Honest status

| Feature | Researched | Implemented | Compiled | HW verified |
|---|---|---|---|---|
| SPR 5/9/12/15/20 V | YES | YES | host | **YES** |
| PPS / AVS | YES | YES | host | **YES** |
| INA226 + real VBUS | YES | YES | host | **YES** |
| Diagnostics counters | YES | YES | host | **YES** |
| USB CDC stability | YES | YES | host | fix unproven |
| DTS temperature | YES | YES | host | fix unproven |
| EPR | YES | YES | host | **blocked — needs a real EPR charger** |

I cannot ARM-link or flash in my sandbox, so the CDC and DTS fixes are
unproven until you flash. ST middleware and the core library are untouched
(`2fb9c3f7…`).
