# EPR crash — session findings & the missing discriminator

Branch `arena/01a06344-usb-ucpd-new`. Commit `790fbd2`.

## What this session established

### 1. The EPR fault record — why it never printed (now fixed)

The one instrument that would end the guessing — the boot-time
"PREVIOUS RUN FAULTED" report — was broken by **three independent
defects**:

1. **Layout collision.** The fault record sat at BKPSRAM offset 0
   (`0x38800000`), on top of `APP_STORE_Cfg_t` (`app_store.c`). A
   `store save` erased a crash record; a crash destroyed the saved
   profile. Nobody could observe either.
2. **Cache.** The whole BKPSRAM bank falls inside MPU region 0
   subregion 1 (`0x20000000..0x3FFFFFFF`), which the 4 GB background
   region **disables**. A disabled subregion is *not protected*: it
   falls through to the Cortex-M7 **default** memory map — Normal,
   write-back, **cacheable**. With D-cache enabled, the fault-record
   stores stayed dirty in cache and were lost on warm reset. That is
   why the report never appeared.
3. **No PC captured, and the entry clobbered the frame.** The old
   handler stored only CFSR/HFSR/MMFAR/BFAR. The C function entry ran
   before anything read the hardware exception frame, so the stacked
   PC/LR were unrecoverable even with a debugger attached at the
   handler.

### 2. Fix delivered (`790fbd2`, 3 files)

- **`main.c`**
  - Fault record moved to `0x38800200` (clear of the store config at
    `0x38800000`, which spans about 156 bytes) with magic
    `0xFA017EDD`, plus PC/LR/xPSR slots.
  - Fault vectors (`HardFault_Handler`, `MemManage_Handler`,
    `BusFault_Handler`, `UsageFault_Handler`) are now **naked
    trampolines**: they read SP from EXC_RETURN bit 2 *before* any C
    prologue and tail-branch into `APP_FaultReportCore(frame, code)`,
    so the 8-word hardware exception frame (R0..R3,R12,LR,PC,xPSR) is
    captured intact. PC = frame[6], LR = frame[5].
  - `APP_FaultReportCore` stores the record, prints it live on
    **USART1** (the PD trace UART, PB6/PB7 @ 921600) via a bounded,
    register-level, interrupt-free poll — so the PC survives even a
    power cycle that clears backup SRAM — then blinks as before.
  - New **MPU region 5**: `0x38800000`, 4 KiB, non-cacheable, so the
    bank is no longer on the default write-back map.
  - `APP_FaultReportBoot` prints the faulting PC/LR/xPSR, decodes
    MMFAR/BFAR/HFSR validity bits, and prints the exact decode
    command: `arm-none-eabi-addr2line -e
    Appli/Release/Appli_Release.elf -f -C <PC>`.
- **`stm32h7rsxx_it.c`** — stock C bodies #define-renamed away so a
  CubeMX regeneration cannot reintroduce duplicate vector symbols.
- **`main.h`** — prototypes updated.

Host gate: 149/149.  Edited files pass `gcc -Wall -Wextra` against the
real repo headers under the project's exact define set. No ARM build or
flash was possible in this sandbox.

### 3. Static root-cause hunt — everything still open

Remaining runtime-only discriminator: the faulting PC/CFSR on the next
`epr enter`. Statically cleared this session (each is evidence-backed,
details in the code comments):

- **PE context overrun**: the per-port PE handle is `malloc(0x4A0)`
  (1184 bytes, `USBPD_PE_Init` +0x2A, NULL-checked); the deepest
  context write found anywhere is `PE_SubStateMachine_VconnSwap`
  `strb +0x49E` — fits exactly. The library cannot overrun its own
  handle.
- **Callback-table NULLs**: all 17 `USBPD_PE_Callbacks` slots are
  installed and bounded (verified map in the repo notes); VCONN
  returns are spec-legal.
- **DPM scratch overrun / DataId mismatch**: measured DataId enum
  values by compiling against the real headers under the exact
  `.cproject` defines: `SNK_PDO_EPR`=25, `SNK_PDP_EPR`=30,
  `RCV_*_EPR`=26/27, etc.  `USBPD_DPM_GetDataInfo` writes exactly
  bounded values.
- **App-side EPR parsing**: `APP_EPR_OnSrcPdo` bounds AVS to
  `src_avs[7]`; `APP_EPR_GetSinkEprPdos` bounds to
  `USBPD_MAX_NB_EPRPDO`; `OnModeDo` requires 4 bytes. No overflow.
- **Same ST lib runs the reference flow** on other STM32s (G071
  thread trace), so the library's EPR path is not inherently fatal.

So the crash is still pinned to something that only a fault PC (now
guaranteed to be captured) can localize. **Do not flash and call EPR
done**: flash the `790fbd2` build, reproduce `epr enter`, and read the
record.

## Next steps (in order)

1. Rebuild in CubeIDE (GNU ARM 14.3.1) and flash.
2. Reproduce: EPR charger → `epr enter` → board faults.
3. Read the record — **two independent channels**:
   - On the trace UART (USART1 PB6/PB7 @ 921600, any terminal): the
     `***FAULT code=2 CFSR=… PC=… LR=…` line prints live at the
     moment of the fault, before the blink loop.
   - Next USB CDC boot banner: `*** PREVIOUS RUN FAULTED: HardFault
     … PC=0x9000xxxx` plus the decode command.
4. Decode the PC: `arm-none-eabi-addr2line -e
   Appli/Release/Appli_Release.elf -f -C 0x9000xxxx` (or the Debug
   ELF).  Re-derive the call target at that site from the library's
   relocations (pyelftools ground truth — do NOT trust the old
   `RELOC[n]` dump labels or unlinked capstone `bl` targets).
5. Only then write the evidence-backed fix, and only claim EPR
   HARDWARE VERIFIED with an `Enter Succeeded` log plus a board that
   stays alive.
