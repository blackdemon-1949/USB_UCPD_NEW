# Flashing PD Bench (WeAct STM32H7R3Z8)

You need **STM32CubeIDE** (to build) and **STM32CubeProgrammer** (to write both
images). The Puya PY25Q64HA is **not** in ST's stock loader list — use the
WeAct external loader shipped in `tools/`.

> **This-session build status**: real ST HAL_SD + ChaN FatFS sources have been
> dropped into the tree (`Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_sd*.c`,
> `Middlewares/Third_Party/FatFs/...`).  They compile cleanly under the host
> `gcc -fsyntax-only` check (`tools/check_syntax.sh`: **68 passed, 0 failed**).
> The CubeIDE project metadata (`.cproject`) has **not** been regenerated yet to
> add those source files to the build — CubeMX must re-run from
> `USB_UCPD.ioc` with SDMMC1 + FatFS selected so the project descriptors and
> makefiles pick them up before a real `arm-none-eabi` .elf can be produced.
> Until then the firmware is **source-level complete for Phases 1–3** but NOT
> flashable: CubeMX regeneration is the remaining step before "real cross-
> compile → .elf/.bin" can be claimed. (See ROUND2_STATUS.md in the repo.)

## 1. Build (after CubeMX regeneration)

1. Open `USB_UCPD.ioc` in STM32CubeMX:
   - Pinout & Configuration → enable **SDMMC1** (4-bit, PC12/PD2/PC8–PC11, PA8 as GPIO_Input for card detect),
   - Middleware → enable **FATFS** with SD Card user driver (polling, single drive "0:"),
   - Clock tree: SDMMC12 source = PLL2S (already wired by existing `HAL_SD_MspInit`),
   - Generate code.
2. Re-open in STM32CubeIDE.
3. Build **USB_UCPD_Boot** (Debug). Expected ELF: `Boot/Debug/USB_UCPD_Boot.elf`.
4. Build **USB_UCPD_Appli** (Debug). Expected ELF: `Appli/Debug/USB_UCPD_Appli.elf`,
   binary `Appli/Debug/USB_UCPD_Appli.bin`, hex `Appli/Debug/USB_UCPD_Appli.hex`.

Both Debug configurations already point at:

- Boot → `STM32H7R3Z8JX_FLASH.ld` (`0x08000000`, 64 KB internal flash)
- Appli → `STM32H7R3Z8JX_ROMxspi1.ld` (`0x90000000`, 8 MB external XiP NOR)

Sanity check after build: `arm-none-eabi-size Appli/Debug/USB_UCPD_Appli.elf`
must show `.text` < 8 MB (the PY25Q64HA size used by the bootloader).

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
3. Plug USB-HS Type-C into the PC. CDC device: **WeAct / PD Bench — H7R PD Sink**.
4. Open the serial port (115200 8N1). The first-boot banner should read:

   ```
   ========================================
     PD Bench  —  STM32H7R3 UCPD sink
     WeAct H7R3Z8  |  XiP PY25Q64HA
   ========================================
   USB CDC up. Type help

   ina226: initialised at 0x40 on I2C2 (sampling every 1000 ms)
   ext-sd: SDMMC1 ready  card=<present|absent>  (PA8 detect, active-low, verify on hardware)
   fatfs: SD disk driver linked as '0:'
   waiting for PD source on CC1/CC2...
   >
   ```

5. Wire a powered USB-PD source to **one** MCU CC input and common ground:
   source CC1 → PM0 **or** source CC2 → PM1, plus GND. Do **not** connect both
   CC inputs together, and do not use the USB-HS Type-C receptacle for PD —
   that connector is CDC only.

6. After inserting a FAT32-formatted micro-SD card the log should add:
   ```
   ext-sd: card inserted
   ext-sd: card  type=...  blocks=...  blocksize=512  logblocks=...
   ext-sd: mounted FAT volume at '0:'
   ```
   The first `ina energy` after one minute's integration should show an
   accumulated mAh/mWh with `checkpoints=1` if a card is present.

### If it does not boot

Count PB2 blinks (see README fail codes).

| Symptom | Likely cause |
| --- | --- |
| 3 blinks | Appli not at `0x90000000`, or linker still on the MMT template |
| 1–2 blinks | NOR / mmap — wrong loader, or SFDP path re-enabled |
| CDC missing | `MX_USB_DEVICE_Init` / `USBD_Start` not in Appli, or USB-HS unplugged |
| PD silent | No powered USB-PD source, both CC inputs wired together, or source connected to USB-HS instead of one of PM0/PM1 |
| No `fatfs: SD disk driver linked` banner | CubeMX did not regenerate with FatFS enabled, or `Middlewares/Third_Party/FatFs/src/...` sources are not in the .cproject |

## CubeIDE debug

- Debug **Boot** if you need the XiP bring-up (breakpoint before the jump).
- Debug **Appli** with "load image" disabled if Boot already mapped NOR;
  set VTOR/`0x90000000` in the debug configuration.
- Do not single-step across the Boot→Appli jump with caches enabled
  without invalidating I-cache.
