# Flashing PD Bench (WeAct STM32H7R3Z8)

You need STM32CubeIDE (to build) and STM32CubeProgrammer (to write both
images). The Puya PY25Q64HA is **not** in ST's stock loader list — use the
WeAct external loader shipped in `tools/`.

## 1. Build

1. File → Open Projects from File System → select this `USB_UCPD` folder.
2. Build **USB_UCPD_Boot** (Debug). ELF: `Boot/Debug/USB_UCPD_Boot.elf`
3. Build **USB_UCPD_Appli** (Debug). ELF: `Appli/Debug/USB_UCPD_Appli.elf`

Both Debug configurations already point at:

- Boot → `STM32H7R3Z8JX_FLASH.ld` (`0x08000000`, 64 KB)
- Appli → `STM32H7R3Z8JX_ROMxspi1.ld` (`0x90000000`, 8 MB)

The `Appli` Debug configuration is pre-wired in the repo for the open
pdsink PD3.x profile (define, pdsink sources, C++ link, closed-core
`.a` removal and file exclusions are already in `.cproject`/`.project`
— see `Middlewares/PDEngine/port/README.md`).  No project-settings
steps between download and build.


## 2. Install the WeAct NOR loader

In STM32CubeProgrammer:

1. Open **External loaders**
2. Add `tools/STM32H7R3Z8Jx_8MB_WeAct.stldr`
3. Enable it for this session

The loader talks QuadSPI/XSPI to the Puya part using W25Q-compatible
commands. Stock ST SFDP loaders will fail or brick the map.

## 3. Connect

- SWD: `PA13` SWDIO, `PA14` SWCLK, GND, 3V3
- Board powered from USB-HS **or** a 3V3/5V header supply
- NRST if the probe provides it

## 4. Program Boot (internal FLASH)

- Address `0x08000000`
- File: `Boot/Debug/USB_UCPD_Boot.elf` (or `.bin`)
- Download

## 5. Program Appli (external NOR, mapped)

- Address `0x90000000`
- File: `Appli/Debug/USB_UCPD_Appli.elf`
- External loader **must** be selected
- Download

## 6. Reset and check

1. Reset the MCU.
2. PB2: brief flash during Boot, then Appli heartbeat (~1 Hz, 80 ms on).
3. Plug USB-HS Type-C into the PC. Device: **WeAct / PD Bench — H7R3 PD Sink**.
4. Open the serial port (115200 8N1). Banner + `help` should print.
5. Wire a powered USB-PD source to **one** MCU CC input and common ground:
   source CC1 → PM0 **or** source CC2 → PM1, plus GND. Do not connect both
   CC inputs together or connect the source to the USB-HS connector. Do not use the USB-HS
   Type-C for PD — that connector is CDC only.

### If it does not boot

Count PB2 blinks (see README fail codes).

| Symptom | Likely cause |
| --- | --- |
| 3 blinks | Appli not at `0x90000000`, or linker still on the MMT template |
| 1–2 blinks | NOR / mmap — wrong loader, or SFDP path re-enabled |
| CDC missing | `MX_USB_DEVICE_Init` / `USBD_Start` not in Appli, or USB-HS unplugged |
| PD silent | No powered USB-PD source, both CC inputs wired together, or source connected to USB-HS instead of one of PM0/PM1 |

## CubeIDE debug

- Debug **Boot** if you need the XiP bring-up (breakpoint before the jump).
- Debug **Appli** with “load image” disabled if Boot already mapped NOR;
  set VTOR/`0x90000000` in the debug configuration.
- Do not single-step across the Boot→Appli jump with caches enabled
  without invalidating I-cache.

## PDEngine pdsink path — flash-safety summary (2026-09-03)

This appendix is the M5 flash-safety guarantee for the pdsink switch-over
(milestone plan in `Middlewares/PDEngine/README.md` and
`Middlewares/PDEngine/port/README.md`).

### What a pdsink-path flash changes

- **Nothing about the memory map.** Boot stays in internal flash
  (`0x08000000`, untouched); the Appli keeps the XIP external-NOR layout
  (`Appli/STM32H7R3Z8JX_ROMxspi1.ld`, `0x90000000`) and the WeAct
  external loader above.  No new partitions, no relocations, no OTP.
- The Appli build drops the closed core library
  (`Middlewares/ST/STM32_USBPD_Library/Core/lib/*.a`) from the link and
  compiles the pdsink core + port sources instead (wiring steps 1–6 in
  `port/README.md`).  The application image is still a single ELF written
  with the same CubeProgrammer sequence used today.

### Why flashing cannot brick the board

1. **The runtime never writes the NOR.** The pdsink path performs no
   in-application flash writes of any kind (state lives in RAM/BKPSRAM);
   the external NOR is execute-only at runtime.  A bad build can only
   misbehave, not corrupt the image.
2. **The previous firmware is one reflash away.** The closed-core path is
   intact in this repository's history (and in the two backup zips at the
   repo root).  Rollback = check out the previous Appli state (or the
   `USB_UCPD_ORIGINAL_workingButminimal.zip` reference), rebuild, and
   re-flash with the exact same loader.  No special unlock is needed.
3. **Boot independence.** The Boot project and its flash content are not
   part of the switch-over; even a completely dead Appli still boots and
   can be re-flashed over SWD (PA13/PA14) at any time.
4. **Host-proven protocol behaviour first.** Every PD behaviour the board
   will run (SPR attach/contract/PPS, EPR enter/AVS/exit, hard-reset
   recovery, refusals) is exercised by `tools/pdport_hosttest/run.sh`
   (4 suites, green) before any ARM build is attempted, so the first
   board flash is a known stack, not an experiment.

### Board bench order (do not skip)

1. Flash the pdsink-path Appli (wiring steps 1–6 of `port/README.md`).
2. **SPR gate on the bench first**: attach an SPR charger → explicit
   contract → `req 2`, `pps 21000` behave as before; console and CLI
   stay alive; detach/reattach renegotiates; INA226/CLI untouched.
3. Only then attach the PD 3.1 EPR source and run the M5 acceptance:
   auto `Enter Succeeded` with the board alive, EPR source caps/AVS
   visible and requestable, truthful `epr`/`pd` status, `epr exit`
   (two-step) back to SPR with PPS working, and no hard fault / hang in
   any step (the firmware's existing `***FAULT` capture and fail-blink
   still apply if something ever faults — capture and report, then
   power-cycle).
4. If anything regresses: power-cycle, reflash the previous Appli build
   (step 2 of "cannot brick" above) — behaviour returns to the
   pre-switch-over state.
