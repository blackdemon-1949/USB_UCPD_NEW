# USB-C PD/UCPD Analyzer — Final Software Audit

Branch `arena/01a060c4-usb-ucpd-new`, release checkpoint **`e245309`**.
Golden baseline `golden-baseline-5fd7901`.

> **Nothing in this document has run on hardware.** Every classification below
> rests on ARM build output, ELF symbol reachability, and host tests. Hardware
> operation is explicitly *not* verified for any feature.

---

## 1. Audit evidence

```
1. ARM builds (real ARM GCC 13.2.1, both configs)
   Boot_Debug / Boot_Release / Appli_Debug / Appli_Release   all OK

2. Application warnings                                      NONE
   (only pre-existing at the golden baseline: W25QXX_Wait_Busy in Boot,
    startup_stm32h7r3z8jx.s trailing-newline note)

3. Host tests, ASan + UBSan, 7 suites                        ALL PASS
   app_dec 101 | app_cap 58 | engines 94 | vdm | fuzz | vectors 52 | pps 105

4. Fuzz / malformed regression                               PASS
   11 mutation classes, 24 seeds x 600 cases = 14400 frames,
   0 unflagged, 0 sanitizer faults

5. ELF reachability (tools/verify.py)                        OVERALL: PASS
   Boot   Debug 13/13     Release 11/11
   Appli  Debug 170/170   Release 156/156

6. Linker / XIP / MPU / DMA / cache                          PASS
   XIP FLASH 0x90000000: 142 KiB Debug / 129 KiB Release of 8192 KiB
   MPU alignment OK for every region (32 KiB Appli, 1 KiB Boot)
   non-cacheable DMA window: 448 KiB of 456 KiB budget
   UserRxBufferHS 0x24068900 / UserTxBufferHS 0x24068100, inside the window
   cacheable RAM 0x24000000..0x24068000 (416 KiB), 384 KiB heap headroom

7. Binary .cpd round-trip                                    PASS
   5 records written, 5 parsed, 0 framing errors
   encoder matches ST's own OverFlow_String byte for byte

8. PDO/RDO/PPS/EPR/Cable vectors                             PASS
   tools/pdtools/test_pdtools.py + pps/epr/cable host suite (105 assertions)

9. CLI / HELP coverage                                       14/14 engines
   cap diag epr power txn cable pps temp integ test store ext fuzz vdm

10. Hidden NOR writes                                        NONE
    grep for FLASH_Program / W25QXX_PageProgram / W25QXX_Erase / NOR_Write
    across Appli/Core: zero matches. Appli contains no NOR programming path.

11. Regression vs golden-baseline-5fd7901                     NO REGRESSION
    Boot_Release text/data/bss = 11320 / 12 / 2188 — identical
    `git diff --stat golden-baseline-5fd7901 HEAD -- USB_UCPD/Boot` — EMPTY:
    the entire Boot tree is byte-identical to the baseline.
    Appli_Release text/data/bss = 132848 / 1800 / 56764
```

## 2. VDM and cable re-discovery — how they were resolved

Both were previously reported as gaps. Neither turned out to need new
mechanisms; both are real ST library entry points.

**Located in the installed middleware.** `usbpd_core.h` declares, under
`#if defined(USBPDCORE_SVDM) || defined(USBPDCORE_VCONN_SUPPORT)`:

```c
USBPD_StatusTypeDef USBPD_PE_SVDM_RequestIdentity(uint8_t PortNum, USBPD_SOPType_TypeDef SOPType);
USBPD_StatusTypeDef USBPD_PE_SVDM_RequestSVID   (uint8_t PortNum, USBPD_SOPType_TypeDef SOPType);
USBPD_StatusTypeDef USBPD_PE_SVDM_RequestMode   (uint8_t PortNum, USBPD_SOPType_TypeDef SOPType, uint16_t SVID);
USBPD_StatusTypeDef USBPD_PE_SVDM_RequestModeEnter(uint8_t PortNum, USBPD_SOPType_TypeDef SOPType, uint16_t SVID, uint8_t ModeIndex);
USBPD_StatusTypeDef USBPD_PE_SVDM_RequestModeExit (uint8_t PortNum, USBPD_SOPType_TypeDef SOPType, uint16_t SVID, uint8_t ModeIndex);
```

**The guards are satisfied.** `usbpd_def.h:148-150` defines `USBPDCORE_SVDM`,
`USBPDCORE_UVDM` and `USBPDCORE_VCONN_SUPPORT` under `USBPDCORE_LIB_PD3_FULL`,
and `tools/build.py` parses `USBPDCORE_LIB_PD3_FULL` from the Appli `.cproject`.

**The symbols genuinely exist.** The PE is precompiled, so a declaration proves
nothing on its own. Parsing the archive's global symbol table directly (`ar`
and `nm` are absent from this toolchain) shows all five are **PRESENT** in
`Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a`. They link, and `verify.py` now
asserts all five survive `--gc-sections` in the final ELF.

**Cable re-discovery is therefore genuinely supported:** `RequestIdentity` on
`USBPD_SOPTYPE_SOP1` (=1, SOP') is the ST-supported entry point for a SOP'
Discover Identity. Exposed as `vdm discover`.

**Mode enter/exit** responses arrive through
`USBPD_VDM_InformModeEnter` / `USBPD_VDM_InformModeExit`, which were already
registered positionally in `vdmCallbacks` but had empty bodies. Their
`USER CODE` blocks now record the responder status
(`SVDM_RESPONDER_ACK`/`NAK`/`BUSY` = `0x01`/`0x02`/`0x03`). Only an ACK'd
Enter sets `in_alt_mode`, and only an ACK'd Exit clears it — a NAK or BUSY can
never falsely report the mode as active.

No Policy Engine logic was reimplemented. The ST PE owns the SVDM transaction,
its retries and its timeouts.

## 3. Feature matrix

### IMPLEMENTED + VERIFIED
*ARM build, ELF reachability, and host assertions exercising the real code.*

| Feature | Evidence |
|---|---|
| PD 2.0/3.0/3.1 message decoder | 101 host assertions; control wire values corrected |
| Lossless RAM ring capture | 58 host assertions |
| Transaction / state reconstruction | 94 + 52 assertions, incl. failure paths and both timeouts |
| Chunked extended-message reassembly | gap/dup/out-of-order/type-change/overflow all detected by the fuzzer |
| PPS analysis, validation, RDO construction | 105 assertions (pps/epr/cable suite) |
| Cable / E-marker VDO decode and verdict | VDO, identity, all four verdicts |
| EPR AVS PDO build, parse, clamp, ceiling | same suite |
| Power / energy / charge analytics | statistics and integration |
| Diagnostics counters | 42 counters; `diag all/health/coherency/clear` |
| CRC / SHA-256 / RNG | all reachable and used (replay digest, store integrity, fuzz seeding) |
| Deterministic on-target vector suite | 52 assertions, runs on target and host |
| Capture replay with divergence reporting | re-derives the transaction, compares with live state |
| Fuzz / malformed-message engine | 11 mutation classes, 14400 frames, 0 unflagged |
| **VDM alternate-mode request path** | validation, marshalling and counters host-tested against recording stubs |
| **VDM response classification** | ACK/NAK/BUSY state transitions asserted; NAK cannot enter the mode |
| **ST PE SVDM entry points linked** | all five `USBPD_PE_SVDM_*` present in the ELF |
| CLI + HELP coverage | 14/14 engines, one `s_cmds[]` entry each |
| Binary `.cpd` writer and reader | byte-for-byte match against ST's `OverFlow_String`; round-trips |
| Host decoder / JSONL / `.cpd` tooling | independent PDO/RDO tests built from spec field positions |
| Live VDM callback registration | full chain proven in the ELF |

### IMPLEMENTED + NOT HARDWARE TESTED
*Built, linked and reachable. Needs a real source, cable or peripheral.*

| Feature | What hardware would prove |
|---|---|
| **VDM mode enter/exit against a real partner** | An actual ACK/NAK from a partner that supports an alternate mode. No SVDM transaction has been exchanged. |
| **Cable re-discovery (`vdm discover`)** | An actual SOP' Discover Identity response from an E-marker cable. |
| EPR entry / exit / keepalive | A 28/36/48 V source accepting RDO bit 21. |
| Live SOP'/SOP'' cable identity | A real E-marker responding. |
| Cable verdict against a live contract | A real cable producing one. |
| DTS temperature | A reading from the sensor. |
| CRC / HASH / RNG on silicon | A conversion on the real peripherals. |
| INA226 correlation | A measurement. |
| TRACER_EMB forensics | A captured trace at 921600 baud. |
| Backup-SRAM persistence | A write, verify and CRC on hardware. |
| On-target fuzz | `fuzz run` on the board. |
| USB HS CDC / CLI / fixed PDO / PPS / INA226 on Appli | Regression re-proof on hardware. Boot is byte-identical; the Appli paths have not been re-run on the board this session. |

### NOT AVAILABLE IN ST LIBRARY

None. Both items previously in this class were found to be genuine, supported
ST entry points and are now implemented — see §2.

### NOT IMPLEMENTED

| Feature | Status and reason |
|---|---|
| **NOR persistence** | **Deliberately disabled, by design.** The application XIPs from `0x90000000`; programming that device requires a RAM-resident programming stub running with the XIP region quiesced. No such stub exists, so **no NOR write is performed anywhere in Appli** — verified by grep across `Appli/Core` returning zero matches. `store status` states on the console that the medium is backup SRAM and that NOR persistence is DISABLED. Backup SRAM is offered as the working medium and is labelled as *not NOR*. Implementing it safely means writing a RAM-resident XSPI programmer with interrupts and fetches away from NOR during erase/program. |
| Byte-exact vendor `.cpd` container | The TLV stream STM32CubeMonitor-UCPD records is implemented and verified against ST's own emitter. Any vendor-specific framing layered on top of that stream is not. |

## 4. Defects found and fixed this session

1. **`APP_VDM_Prepare` / `APP_VDM_CompleteCall` missing from `app_vdm.h`.**
   ARM GCC defaulted them to `int`, which matched their return type, so the
   build passed with the declarations absent — the same class of bug as the
   earlier `APP_CAP_ElapsedUs` call. Caught by the host build's `-Werror`.
2. **`APP_VDM_Get` had no caller**, so `--gc-sections` dropped it in both Debug
   and Release. The CLI status print now goes through the accessor.
3. **Unsolicited PS_RDY counted as a negotiated contract** (found by the fuzz
   engine). Legal after a Hard Reset, but indistinguishable from a real
   negotiation; now has its own `n_unsolicited_psr` counter.
4. **Control-message off-by-one** in `app_dec`/`app_txn` — would have misnamed
   every control message and prevented any transaction reaching contract.
5. **`pdtools.py` `.cpd` was CSV** — now real binary TLV.
6. **`-Wconversion` violation** in `app_ext.c`, caught only by the stricter
   host build.

## 5. Release checkpoint for first hardware validation

**`e245309`**

| Artifact | Destination |
|---|---|
| `USB_UCPD/Boot/Release/Boot_Release.bin` | internal flash, as before |
| `USB_UCPD/Appli/Release/Appli_Release.bin` (134664 bytes) | XSPI NOR, `0x90000000` |

Release is the configuration the golden baseline was validated in, and the Boot
tree is byte-identical to it, so any failure localises to the Appli.

Suggested sequence, one engine confirmed per step:

```
help              CLI registry and HELP coverage
diag coherency    MPU / non-cacheable DMA window
diag health       counters and thermal state
getcaps           source capabilities arrive
pps               PPS windows from the real source
vdm status        VDM state and counters
vdm discover      cable re-discovery: SOP' Discover Identity
cable status      live E-marker identity and verdict
vdm svids         SVIDs the partner supports
vdm enter <svid> 1   alternate-mode entry   <-- needs a partner with a mode
vdm status        confirm ACK and in_alt_mode
epr status        EPR capability and ceiling
integ status      CRC / HASH / RNG ready
temp              DTS reading
test suite        on-target vector suite
fuzz run 200 1    on-target fuzz, seed 1
ext               extended-message reassembly state
store status      persistence state (read-only)
```

Do **not** raise the EPR ceiling above 28 V until 28 V is proven on the board.
