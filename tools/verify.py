#!/usr/bin/env python3
"""verify.py - post-link verification of the built ELF/map files.

Reads the ELF directly (section + program headers) plus the linker map
(memory regions, symbol addresses) and checks:
  * the memory regions the linker script defines
  * every ALLOC section lands inside its region; loadable regions do not overlap
  * the non-cacheable DMA window: contents, occupancy, MPU base/size alignment
  * heap + stack budgets against the cacheable RAM region
  * presence of the symbols the repaired project depends on
  * FLASH occupancy and the .bin payload size
"""
import os
import re
import struct
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()

AXI_SRAM_BASE = 0x24000000
AXI_SRAM_BUDGET = 0x72000          # 456 KiB - CubeMX AXI SRAM budget for H7R3

USB_DMA_BUFS = ()   # per-project, set in main()

# file-local symbols: kept in the symtab only when the image is built with -g
DEBUG_ONLY = ('MPU_Config', 'APP_PD_AutoApply')

APPLI_SYMBOLS = [
    'main', 'Reset_Handler',
    'APP_PD_Task', 'APP_PD_AutoApply', 'APP_CLI_Poll', 'APP_LOG_Flush',
    'APP_LED_Task', 'EXT_I2C_Poll', 'EXT_SPI_Poll', 'EXT_I2C_Init', 'EXT_SPI_Init',
    'PORT0_PDO_ListSNK',
    'USBPD_DPM_Run', 'USBPD_DPM_InitCore', 'USBPD_DPM_CADCallback',
    'g_usbpd_dbg', 'UCPD1_IRQHandler',
    'USBD_Start', 'USBD_LL_Init', 'CDC_Receive_HS', 'USBD_CDC_RegisterInterface',
    'UserRxBufferHS', 'UserTxBufferHS', 'OTG_HS_IRQHandler',
    'TRACER_EMB_Init', 'TRACER_EMB_SendData', 'TRACER_EMB_IRQHandlerDMA',
    'TRACER_EMB_IRQHandlerUSART', 'USART1_IRQHandler',
    'MX_USART1_UART_Init', 'MX_USART2_UART_Init', 'MX_DTS_Init', 'MX_CRC_Init',
    'MX_HASH_Init', 'MX_RNG_Init', 'MX_GPDMA1_Init', 'MX_I2C2_Init',
    # handles for the peripherals newly enabled in the updated CubeMX config
    'huart1', 'huart2', 'hcrc', 'hdts', 'hhash', 'hrng',
    'handle_GPDMA1_Channel2', 'handle_GPDMA1_Channel3',
    'MX_UCPD1_Init', 'MX_USB_DEVICE_Init', 'MX_USBPD_Init',
    'GPDMA1_Channel0_IRQHandler', 'GPDMA1_Channel1_IRQHandler',
    'GPDMA1_Channel2_IRQHandler', 'GPDMA1_Channel3_IRQHandler',
    'USART2_IRQHandler',
    '_estack', '_sidata', '_sdata', '_edata', '_sbss', '_ebss',
    '__NONCACHEABLEBUFFER_BEGIN', '__NONCACHEABLEBUFFER_END', '_Heap_Limit',
]

ALL_STATICS = ()
BOOT_SYMBOLS = ['main', 'Reset_Handler', 'MPU_Config', 'SystemClock_Config',
                'MX_XSPI1_Init', 'MX_EXTMEM_MANAGER_Init', 'MX_GPIO_Init',
                'MX_GPDMA1_Init', '_estack', '_sidata', '_sbss', '_ebss']


def elf_sections(path):
    """[(name, addr, size, sh_type, sh_flags)] for ALLOC sections, i.e. the
    sections that actually occupy memory on the target.  sh_type 1 = PROGBITS
    (contents loaded from the image), 8 = NOBITS (zero-filled at run time)."""
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 1, 'expected 32-bit ELF'
    (e_shoff,) = struct.unpack_from('<I', d, 0x20)
    (e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHH', d, 0x2e)
    (shstr_off,) = struct.unpack_from('<I', d, e_shoff + e_shstrndx * e_shentsize + 0x10)
    out = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        name_off, stype, flags, addr, offset, size = struct.unpack_from('<IIIIII', d, base)
        if stype not in (1, 8) or not flags & 0x2:      # PROGBITS/NOBITS + ALLOC
            continue
        end = d.index(b'\0', shstr_off + name_off)
        out.append((d[shstr_off + name_off:end].decode(), addr, size, stype, flags))
    out.sort(key=lambda s: (s[1], s[0]))
    return out


def elf_symbols(path):
    """{name: address} from .symtab - includes file-local symbols such as the
    static MPU_Config(), which never reach the linker map."""
    d = open(path, 'rb').read()
    (e_shoff,) = struct.unpack_from('<I', d, 0x20)
    (e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHH', d, 0x2e)
    (shstr_off,) = struct.unpack_from('<I', d, e_shoff + e_shstrndx * e_shentsize + 0x10)
    secs = []
    for i in range(e_shnum):
        b = e_shoff + i * e_shentsize
        name_off, stype, flags, addr, offset, size, link = struct.unpack_from('<IIIIIII', d, b)
        secs.append((stype, offset, size, link, d[shstr_off + name_off:d.index(b'\x00', shstr_off + name_off)].decode()))
    out = {}
    for stype, offset, size, link, name in secs:
        if stype != 2:                                   # SHT_SYMTAB
            continue
        str_off, str_size = secs[link][1], secs[link][2]
        for i in range(size // 16):
            n_off, value, sz, info, other, shndx = struct.unpack_from('<IIIBBH', d, offset + i * 16)
            if n_off == 0:
                continue
            end = d.index(b'\0', str_off + n_off)
            sym = d[str_off + n_off:end].decode()
            out.setdefault(sym, value)
    return out


def map_regions(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    regions = {}
    m = re.search(r'^Memory Configuration\n\n(.*?)\n\n', text, re.S | re.M)
    for line in m.group(1).splitlines()[1:]:
        p = line.split()
        if len(p) >= 3 and p[0] != '*default*':
            regions[p[0]] = (int(p[1], 16), int(p[2], 16))
    return regions


def region_for(regions, addr, size):
    for n, (o, l) in regions.items():
        if o <= addr and addr + size <= o + l:
            return n
    return None


def check(proj, config, wanted):
    if config != 'Debug':
        wanted = [w for w in wanted if w not in DEBUG_ONLY]
    else:
        wanted = wanted + [w for w in DEBUG_ONLY if w in ALL_STATICS]
    d = os.path.join(ROOT, 'USB_UCPD', proj, config)
    elf = os.path.join(d, '%s_%s.elf' % (proj, config))
    binf = os.path.splitext(elf)[0] + '.bin'
    mapf = os.path.splitext(elf)[0] + '.map'
    print('=' * 76)
    print('%s / %s' % (proj, config))
    print('=' * 76)
    problems = []
    regions = map_regions(mapf)
    symbols = elf_symbols(elf)
    alloc = [s for s in elf_sections(elf) if s[2]]

    print('  memory regions (from the linker map):')
    for n, (o, l) in regions.items():
        print('    %-24s 0x%08X .. 0x%08X  %7d B' % (n, o, o + l, l))

    print('  ALLOC sections:')
    for name, addr, size, stype, flags in alloc:
        host = region_for(regions, addr, size)
        kind = 'PROGBITS' if stype == 1 else 'NOBITS'
        if host is None:
            problems.append('%s 0x%08X+0x%X lies in no declared region' % (name, addr, size))
        print('    %-20s 0x%08X  0x%06X  %-8s -> %s'
              % (name, addr, size, kind, host or 'NONE'))

    # no two ALLOC sections may share bytes
    for (n0, a0, s0, _, _), (n1, a1, s1, _, _) in zip(alloc, alloc[1:]):
        if a1 < a0 + s0:
            problems.append('overlap: %s[0x%08X-0x%08X] and %s[0x%08X-0x%08X]'
                            % (n0, a0, a0 + s0, n1, a1, a1 + s1))

    # ---- non-cacheable DMA window -----------------------------------------
    if 'RAM_NONCACHEABLEBUFFER' in regions:
        o, l = regions['RAM_NONCACHEABLEBUFFER']
        beg = symbols.get('__NONCACHEABLEBUFFER_BEGIN', o)
        end = symbols.get('__NONCACHEABLEBUFFER_END', o)
        used = max(end - beg, 0)
        in_win = [n for n, a, s, _, _ in alloc if o <= a < o + l]
        print('  non-cacheable DMA window:')
        print('    0x%08X .. 0x%08X  size %d KiB, used 0x%X (%d B), free %d B (%.0f%%)'
              % (o, o + l, l // 1024, used, used, l - used, 100.0 * (l - used) / l))
        print('    sections: %s' % (', '.join(in_win) or '(none)'))
        if l & (l - 1):
            problems.append('window size 0x%X is not a power of two - unusable as an '
                            'MPU region size' % l)
        elif o % l:
            problems.append('window base 0x%08X is not aligned to its %d KiB size; an MPU '
                            'region of that size would be rounded down to 0x%08X'
                            % (o, l // 1024, o & ~(l - 1)))
        else:
            print('    MPU alignment OK: base is a multiple of the %d KiB region size'
                  % (l // 1024))
        if used > l:
            problems.append('non-cacheable content 0x%X overflows the 0x%X window' % (used, l))
        if o + l > AXI_SRAM_BASE + AXI_SRAM_BUDGET:
            problems.append('window top 0x%08X exceeds the AXI SRAM budget end 0x%08X'
                            % (o + l, AXI_SRAM_BASE + AXI_SRAM_BUDGET))
        else:
            print('    fits AXI SRAM: %d KiB used of the %d KiB budget'
                  % ((o + l - AXI_SRAM_BASE) // 1024, AXI_SRAM_BUDGET // 1024))
        for s in USB_DMA_BUFS:
            a = symbols.get(s)
            if a is None:
                problems.append('%s missing from the image' % s)
            elif not (o <= a < o + l):
                problems.append('%s at 0x%08X is OUTSIDE the non-cacheable window' % (s, a))
            else:
                print('    %-16s @ 0x%08X  (window offset 0x%X)' % (s, a, a - o))

        ram_o, ram_l = regions['RAM']
        if ram_o + ram_l != o:
            problems.append('cacheable RAM end 0x%08X != window start 0x%08X' % (ram_o + ram_l, o))
        hl, ebss = symbols.get('_Heap_Limit'), symbols.get('_ebss', 0)
        if hl is None:
            # Boot's STM32H7R3Z8JX_FLASH.ld does not define _Heap_Limit.
            print('  cacheable RAM: 0x%08X .. 0x%08X (%d KiB); _ebss 0x%08X '
                  '(no _Heap_Limit in this linker script)'
                  % (ram_o, ram_o + ram_l, ram_l // 1024, ebss))
        else:
            if hl > o:
                problems.append('_Heap_Limit 0x%08X runs into the non-cacheable window '
                                '(starts 0x%08X)' % (hl, o))
            print('  cacheable RAM: 0x%08X .. 0x%08X (%d KiB); _ebss 0x%08X, _Heap_Limit '
                  '0x%08X -> %d KiB of heap headroom'
                  % (ram_o, ram_o + ram_l, ram_l // 1024, ebss, hl, (hl - ebss) // 1024))
        for s in ('_Min_Heap_Size', '_Min_Stack_Size'):
            if s in symbols:
                print('    %s = 0x%X' % (s, symbols[s]))

    # ---- flash -------------------------------------------------------------
    for n, (o, l) in regions.items():
        if n != 'FLASH':
            continue
        load = [a + s for nm, a, s, t, f in alloc if t == 1 and o <= a < o + l]
        end = max(load, default=o)
        print('  FLASH 0x%08X: %d KiB used of %d KiB (%.1f%% free)'
              % (o, (end - o) // 1024, l // 1024, 100.0 * (1 - (end - o) / l)))
        if end > o + l:
            problems.append('FLASH overflow by %d B' % (end - o - l))
        if os.path.exists(binf):
            sz = os.path.getsize(binf)
            print('  payload %s: %d bytes' % (os.path.basename(binf), sz))
            if sz > l:
                problems.append('.bin (%d B) larger than the FLASH region (%d B)' % (sz, l))

    # ---- symbols -----------------------------------------------------------
    missing = [s for s in wanted if s not in symbols]
    print('  symbols: %d/%d expected present%s'
          % (len(wanted) - len(missing), len(wanted),
             '' if not missing else '  MISSING: ' + ', '.join(missing)))
    if missing:
        problems.append('missing symbols: %s' % ', '.join(missing))
    for s in ('PORT0_PDO_ListSNK', 'UserRxBufferHS', 'UserTxBufferHS',
              '__NONCACHEABLEBUFFER_BEGIN', '__NONCACHEABLEBUFFER_END',
              '_estack', '_Heap_Limit'):
        if s in symbols:
            print('    %-30s 0x%08X' % (s, symbols[s]))

    if problems:
        for p in problems:
            print('  !! %s' % p)
        print('  RESULT: FAIL')
        return False
    print('  all checks passed')
    return True


def main():
    global USB_DMA_BUFS, ALL_STATICS
    ALL_STATICS = ('MPU_Config',)
    good = True
    for proj, syms, bufs, statics in (
            ('Boot', BOOT_SYMBOLS, (), ('MPU_Config',)),
            ('Appli', APPLI_SYMBOLS, ('UserRxBufferHS', 'UserTxBufferHS'),
             ('MPU_Config', 'APP_PD_AutoApply'))):
        USB_DMA_BUFS, ALL_STATICS = bufs, statics
        for cfg in ('Debug', 'Release'):
            good &= check(proj, cfg, syms)
    print('=' * 76)
    print('OVERALL: %s' % ('PASS' if good else 'FAIL'))
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
