# Next flash — what changed and what to check

## First, the most important correction

**Your board was never rebooting.** The banner reappearing right after
`EPR_Mode(Enter): USBPD_OK` looked like a crash, but it was the CDC console
re-greeting. Evidence:

* the only code that prints that banner is `APP_CLI_OnHostOpen()`, called from
  `CDC_SET_CONTROL_LINE_STATE` when Windows asserts DTR;
* this firmware contains **no** `NVIC_SystemReset` and no watchdog
  (`HAL_IWDG`/`HAL_WWDG` are both disabled);
* every fault handler (`HardFault`, `MemManage`, `BusFault`, `UsageFault`,
  `NMI`) is a `while(1)` — a fault would **hang**, not reboot.

The banner + full help is ~2.5 kB written into a 2 kB queue, so the tail was
silently discarded — that is the text chopped mid-word in your log
(`countryinfo <XX>       read country info (XX = two letter[ina226]...`).

## The five fixes

| # | Symptom | Actual cause |
|---|---|---|
| 1 | "console dies / board reboots" | DTR re-greet + 2 kB queue overflow. Now: 3 s re-greet holdoff, banner no longer dumps help, queue 8 kB, drops counted not silent |
| 2 | **Windows Code 10**, macOS fine | BOS advertised **LPM** while the PCD had `lpm_enable = DISABLE`. Windows enforces it, macOS ignores it. LPM now off; `USBD_MAX_POWER` pinned (was undefined → 100 mA default vs self-powered claim) |
| 3 | EPR entry never answered | `PE_VconnSwap = TRUE` but `BSP_USBPD_PWR_VCONNOn()` returns `FEATURE_NOT_SUPPORTED` — **no VCONN supply on this board**. Source could hand us VCONN duty for cable discovery; cable stays unpowered, AMS stalls. Now declares the truth |
| 4 | latent crash (mine, last round) | `APP_CBL_GetLive()` returns a static — **never NULL** — so my null check let an all-zero struct read as a real cable. It also ran *inside* the PE's `GetDataInfo` callback. Now uses `APP_CBL_IsLive()`, task context only |
| 5 | "queued" forever | Entry now retries 4×, and a 1200 ms watchdog prints `NO REPLY from source` |

## What your log already proved works

* `PD TX 6 / RX 3 / GoodCRC 1` — my new counter funnel is live (it read 0
  before because the counters only existed inside the disabled capture engine).
* The INA226 `0x4907` note is gone.
* `src 5V PDO EPR bit : SET` — SPR EPR discovery works.
* All five ST gates open once the contract is explicit.
* 20 V PDO went 3 A → 5 A between SrcCaps: **your e-marked cable is detected**.

## After flashing

```
epr diag        # expect all gates PASS once contract is EXPLICIT
epr             # 'src 5V PDO EPR bit : SET', PDP should now read 140 W
                # (5 A cable) or 84 W (3 A) - not a fixed 140 W
```

Then just attach the source and watch. Auto-entry fires 300 ms after the
explicit contract. Expect **one** of:

* `EPR_Mode: source replied Enter Acknowledged` → `Enter Succeeded` → EPR active
* `EPR_Mode: source replied Enter Failed - <reason>` (reason now decoded)
* `EPR_Mode(Enter): NO REPLY from source within 1200 ms`

All three are diagnostic. Send me whichever you get, plus the CubeMonitor-UCPD
capture (USART1 PB6/PB7 @ 921600).

**On Windows:** uninstall the old device once (Device Manager → right-click →
Uninstall device, tick "delete the driver software" if offered) before
replugging, so Windows re-reads descriptors instead of using its cached
LPM-advertising copy.

## Honest status

| Stage | EPR |
|---|---|
| RESEARCHED | YES |
| IMPLEMENTED | YES |
| COMPILED | host only — 148/148 tests, 0 errors; **no ARM link in my sandbox** |
| FLASHED | by you |
| HARDWARE VERIFIED | gates + SPR discovery **YES**; EPR contract **NOT YET** |

Regressions guarded: 5/9/12/20 V, PPS, INA226, CDC, UCPD, DMA map, XIP,
linker script and MPU layout are untouched. ST library byte-identical
(`2fb9c3f7…`).
