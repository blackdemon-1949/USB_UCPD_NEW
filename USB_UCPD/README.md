# PD Bench — STM32H7R3Z8J6 UCPD sink + USB CDC

WeActStudio STM32H7R3Z8 Core Board firmware.

The board enumerates as a USB CDC serial device on the **USB-HS Type-C**
connector. A PD source is wired **only** to the MCU header: **one CC line (CC1 or CC2), GND**.
Source capabilities print on the serial port; you pick a PDO / PPS voltage
and the UCPD sink negotiates it.

## Hardware (physical board, not the schematic silkscreen)

| Item | Reality |
| --- | --- |
| MCU | STM32H7R3Z8J6, 600 MHz, 64 KB internal FLASH, ~620 KB RAM |
| External NOR | **Puya PY25Q64HA-SUH-IR** 8 MB (schematic says Winbond W25Q64) |
| XiP | Boot in internal FLASH maps NOR at `0x90000000` and jumps |
| USB-HS Type-C (J1, PM5/PM6) | CDC serial to the PC — **not** the PD port |
| UCPD1 CC1 / CC2 | **PM0 / PM1** on the header |
| I2C2 | **PB10 (SCL) / PB11 (SDA)**, 400 kHz fast mode, wired as `INA226_SCL` / `INA226_SDA` |
| INA226 | module on I2C2, address `0x40` (0x41–0x43 probed as fallback), **5 mΩ shunt**, no external pull-ups (MCU internal pull-ups) |
| SPI2 | **PD3 (SCK) / PC1 (MOSI) / PB14 (MISO)**, 37.5 MBit/s master |
| LCD header | **PA0 `LCD_CS`**, **PA1 `LCD_DC/RS`**, **PA4 `LCD_RST`** (4-wire SPI LCD) |
| LED | PB2 |
| KEY | PC13, active low, external 10 kΩ pull-up |

PY25Q64HA SFDP tables are incomplete. Do **not** use ST ExtMem SFDP bring-up.
The Boot project uses the WeAct W25Qxx XSPI driver (reset, QE, `0xEB` 1-4-4
memory-mapped). The WeAct `.stldr` in `tools/` programs the same chip.

## Projects (CubeIDE)

Open the folder as an existing STM32CubeIDE workspace. Two projects:

| Project | Link | Runs from |
| --- | --- | --- |
| **USB_UCPD_Boot** | `Boot/STM32H7R3Z8JX_FLASH.ld` | Internal FLASH `0x08000000` / 64 KB |
| **USB_UCPD_Appli** | `Appli/STM32H7R3Z8JX_ROMxspi1.ld` | XiP `0x90000000` / 8 MB |

Build **Debug** for both. Flash Boot first, then Appli (see [FLASHING.md](FLASHING.md)).

## I2C2 / SPI2 extension layer

I2C2 and SPI2 were added on top of the working PD Bench firmware (same
`.ioc`, regenerated with `I2C2` + `SPI2` enabled). Both peripherals are
initialised by CubeMX code (`MX_I2C2_Init` / `MX_SPI2_Init`) and are
**not** consumed by the PD policy engine itself — the I2C2 bus is now used
by the INA226 monitor (see below), SPI2 stays free for feature projects:

| Peripheral | Pins | Config | CubeMX file |
| --- | --- | --- | --- |
| I2C2 | PB10 SCL / PB11 SDA | 400 kHz fast mode, 7-bit | `Appli/Core/Src/i2c.c` |
| SPI2 | PD3 SCK / PC1 MOSI / PB14 MISO | Master, 8-bit, 37.5 MBit/s, NSS soft | `Appli/Core/Src/spi.c` |
| LCD GPIO | PA0 CS / PA1 DC / PA4 RST | Push-pull outputs, medium speed | `Appli/Core/Src/gpio.c` |

### Footprints — `ext_i2c` / `ext_spi`

Two application modules (`Appli/Core/Src/ext_i2c.c`, `Appli/Core/Src/ext_spi.c`)
are the single extension point for future I2C/SPI features. They are already
wired into `main.c`:

- `EXT_I2C_Init()` / `EXT_SPI_Init()` run once after the peripheral init;
- `EXT_I2C_Poll()` / `EXT_SPI_Poll()` run every super-loop pass.

To add a feature (e.g. an **INA226** power monitor on I2C2, or an
**ST7789-style SPI LCD** on SPI2 + PA0/PA1/PA4), implement the two weak hooks
in your own files — no edits to `main.c` or the footprint modules:

```c
void EXT_I2C_FeatureInit(void) { /* one-time setup  */ }
void EXT_I2C_FeaturePoll(void) { /* periodic work   */ }
/* and the SPI equivalents: EXT_SPI_FeatureInit / EXT_SPI_FeaturePoll */
```

Generic helpers are provided: `EXT_I2C_ReadReg` / `EXT_I2C_WriteReg`
(8-bit register access) and `EXT_SPI_Transfer` + `EXT_SPI_LCD_CS/DC/RST`
pin macros. Worked examples are in the file headers of `ext_i2c.c` /
`ext_spi.c`. Boot-time status of both buses prints on the serial port
(`ext-i2c:` / `ext-spi:` lines). All I2C helpers use **finite timeouts**
(`EXT_I2C_TIMEOUT_MS`, or pass your own via the `EXT_I2C_*RegTO` variants)
so a missing or wedged device can never stall the super loop.

## INA226 output monitor (`ina226.c`)

An INA226 module on I2C2 (PB10/PB11, 5 mΩ shunt) measures the output
voltage and current. It plugs into the extension footprint above by
overriding `EXT_I2C_FeatureInit` / `EXT_I2C_FeaturePoll` — no changes in
`main.c`.

Behaviour:

* On boot addresses 0x40–0x43 are probed; a chip is accepted only if the
  Manufacturer ID (`0x5449` = "TI") **and** Die ID (`0x2260`) match.
* **If no INA226 is connected the firmware keeps running normally** — the
  console repeats `no ina226 connected` and the probe is retried every 5 s.
  Every I2C access uses a 10 ms timeout; a wedged bus (SDA stuck low) is
  recovered automatically by re-initialising I2C2. The PD and CDC stacks
  are never blocked by the monitor.
* Sampled every 250 ms (AVG 16, 1.1 ms conversion times — CFG `0x2907`),
  reported on the console every second by default. The configuration is
  read back after writing: only the MODE bits (continuous shunt+bus) are
  a hard requirement — cheap clone INA226s return non-zero values in the
  reserved bits [5:3] and are accepted with a logged note; a chip that
  refuses continuous mode is rejected with a diagnostic instead of being
  silently connected with frozen readings.

  ```
  [ina226] out 5.262 V   370.0 mA   1946 mW   (0x40)
  ```

* Scaling for the 5 mΩ shunt: bus LSB 1.25 mV, shunt LSB 2.5 µV →
  **0.5 mA per shunt LSB exactly** (±16.3 A range before the shunt
  amplifier clips). The chip calibration register is programmed with
  2560 (`0x0A00`) = `0.00512 / (400 µA × 5 mΩ)` so the INA226's own
  current/power registers read 400 µA / 10 mW LSBs as well.
* `ina vbus real` makes the PD stack use the measured rail instead of the
  synthetic CC-only-tester VBUS (falls back to synthetic when the data is
  stale). Default stays `synth`, which needs no wiring assumptions.

CLI commands:

```
ina                    one-shot V / I / P reading (or "no ina226 connected")
ina auto on|off        periodic reporting on/off (default on, 1 s)
ina period <ms>        periodic interval, 250..60000 ms
ina addr <hex>         use another 7-bit address (e.g. ina addr 41)
ina scan               scan the whole I2C2 bus (0x08-0x77)
ina vbus real|synth    what the PD stack reports as VBUS
```

`status` and the boot banner include the INA226 line.

## Serial CLI (115200 8N1, DTR opens a session)

```
help                 this list
status               PD + USB state (+ INA226 line)
info                 board / memory / clocks
caps                 list last source PDOs
getcaps              send Get_Source_Cap
req <n>              request source PDO n (1-based)
volt <mv> [ma]       closest fixed PDO or covering PPS
pps <mv> [ma]        request a PPS APDO
hardreset            PD hard reset
softreset            PD soft reset
ina [args]           INA226 output V/I monitor (see above)
led on|off|hb        LED override
```

First Request after attach auto-picks **5 V** (USB-PD tRequest). Then `req`,
`volt`, or `pps` to change the contract.

## CC-only tester

There is no VBUS ADC on this wiring. The firmware synthesizes VBUS so the
policy engine can complete an explicit contract:

- CC attach → report 5 V
- After a Request is built → report the requested voltage
- Detach / hard reset → 0 V or 5 V as appropriate

The source still drives real VBUS on its own cable. The MCU only talks CC.

## LED fail codes (PB2)

Boot blinks N times, pauses, repeats:

| N | Meaning |
| --- | --- |
| 1 | NOR init failed |
| 2 | Memory-mapped mode failed |
| 3 | No valid Appli vector at `0x90000000` |
| 4 | Jump returned (should never) |
| 5 | Boot `Error_Handler` |
| 7 | Appli `Error_Handler` |
| 8 | USBPD stack init failed (`usbpd.c` / DPM error handler) |

## USB + PD stability fixes (this revision)

* **USB PHY reference clock** — CubeMX on STM32H7RS never emits the
  `.ioc` setting `USB_OTG_HS.RefClockSelection` into the code, leaving
  `RCC_CCIPR1.USBREFCKSEL` at its reset value while the PHY runs from the
  24 MHz HSE. Symptom: CDC enumerates only after several replugs / shows
  as corrupt in the Windows device manager. `HAL_PCD_MspInit` now calls
  `LL_RCC_SetUSBREFClockSource(LL_RCC_USBREF_CLKSOURCE_24M)` explicitly
  (see the ST community thread "STM32H7RS USB_HS failure (Device
  Descriptor Request Failed)").
* **Bus reset with a garbage enumeration speed** no longer calls
  `Error_Handler()` from the USB interrupt (that bricked the board: solid
  PB2, dead PD stack). It falls back to full-speed descriptors instead.
* **Console TX stuck busy** — a bus reset / suspend during an IN transfer
  left the logger in `s_tx_busy = 1` forever, so the COM port looked dead
  until the disable/enable dance in the device manager. `CDC_Init_HS`,
  `CDC_DeInit_HS` and suspend now re-arm the TX path.
* **USB init failure is no longer fatal** — if `MX_USB_DEVICE_Init` fails,
  the PD bench keeps running and the failure is logged instead of a
  permanent blink loop.
* **Interrupt priorities rebalanced** — USB OTG_HS = 4, UCPD1 + its DMA
  channels = 5, PD trace = 6 (was: UCPD 0 = above everything, USB 6).
  PD negotiation bursts can no longer starve USB enumeration (and vice
  versa) when the cable and a PD source are connected at the same time.
* **Silent `while(1)` hangs removed** from the USBPD bring-up
  (`MX_USBPD_Init`, `USBPD_DPM_ErrorHandler`); they now blink LED code 8.


Healthy Appli: slow heartbeat (USB up, no PD), fast blink while negotiating,
solid on explicit contract.

## CubeMX notes (already patched — do not regenerate blindly)

- Appli `main` now calls `MX_USB_DEVICE_Init` + `USBD_Start`
- DPM user callbacks evaluate source PDOs and build RDOs
- Sink PDOs: 5/9/12/15/20 V + PPS 3.3–21 V / 3 A, Higher Capability set
- PC13 is `GPIO_NOPULL` (external pull-up) — the `.ioc` says `PULLDOWN`;
  `gpio.c` carries the working hand-edit (keep it if you regenerate)
- Debug linker scripts are the real FLASH / ROMxspi1 files, not MMT templates
- `gpdma.c` keeps the USBPD trace `GPDMA1_Channel2` NVIC setup **inside a
  USER CODE section** so regeneration does not drop it (the `.ioc` does not
  model that channel)
- I2C2/SPI2 are Appli-locked in the `.ioc` (`USB_UCPD.ioc`); regenerating
  restores `MX_I2C2_Init` / `MX_SPI2_Init` and the LCD/INA GPIO config, and
  re-links the HAL I2C/SPI/SMBUS driver sources into `Appli/.project`
- The extension hooks (`EXT_I2C_Init/Poll`, `EXT_SPI_Init/Poll`) live in
  USER CODE sections of `main.c` and survive regeneration
