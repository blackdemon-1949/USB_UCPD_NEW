#!/usr/bin/env python3
"""Repair Appli/Core/Src/main.c: restore the super-loop work items and the full
MPU configuration that the CubeMX regeneration dropped, adapted to the new
peripheral/DMA layout."""
import sys, io

path = sys.argv[1]
raw = open(path, 'rb').read()
crlf = b'\r\n' in raw
text = raw.decode('utf-8')
if crlf:
    text = text.replace('\r\n', '\n')

def sub(old, new, count=1):
    global text
    n = text.count(old)
    assert n == count, 'expected %d occurrence(s), found %d of:\n%r' % (count, n, old[:200])
    text = text.replace(old, new)

# ---------------------------------------------------------------- super loop
sub(
"""  while (1)
  {
    /* USER CODE END WHILE */
    USBPD_DPM_Run();

    /* USER CODE BEGIN 3 */
    APP_CLI_Poll();
    APP_LOG_Flush();
    APP_LED_Task();
  }
""",
"""  while (1)
  {
    /* USER CODE END WHILE */
    USBPD_DPM_Run();

    /* USER CODE BEGIN 3 */
    /* Fixed-PDO / PPS request engine and the PD state polling.  Without this
       the CLI can still read the stack but no Request is ever sent. */
    APP_PD_Task();

    /* I2C2 / SPI extension polling (see ext_i2c.c / ext_spi.c) */
    EXT_I2C_Poll();
    EXT_SPI_Poll();

    APP_CLI_Poll();
    APP_LOG_Flush();
    APP_LED_Task();
  }
""")

sub("""  /* I2C2 / SPI2 extension footprints (see ext_i2c.c / ext_spi.c) */
  EXT_I2C_Init();""",
    """  /* I2C2 / SPI extension footprints (see ext_i2c.c / ext_spi.c) */
  EXT_I2C_Init();""")

# ------------------------------------------------------------------ MPU_Config
sub(
"""  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
""",
"""  /** Region 0: 4 GB background, no access (subregions 0,1,2,7 disabled = 0x87)
   *
   * CubeMX only emits the XSPI1 window, so a regeneration drops everything
   * below.  Without region 0 the default memory map is left enabled and the
   * MPU stops protecting anything outside the regions that follow.
   */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 1: 8 MB XSPI1 NOR (PY25Q64HA) - XiP code + const
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 2: AXI SRAM (cacheable) - .data/.bss/heap
   *
   * The region 4 override below carves the DMA window out of the top of it.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 3: DTCM (always non-cacheable on M7, tightly coupled)
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x20000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_64KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 4: USB/CDC + USB-PD tracer DMA buffers (16 KiB, non-cacheable AXI SRAM)
   *
   * The linker places the CDC RX/TX buffers, the log TX buffer and the
   * TRACER_EMB context in `noncacheable_buffer`, which STM32H7R3Z8JX_ROMxspi1.ld
   * maps to __RAM_BEGIN + __RAM_SIZE = 0x2406E000 for
   * __RAM_NONCACHEABLEBUFFER_SIZE = 0x4000.  MPU regions are matched by
   * number, so this region overrides the cacheable AXI SRAM region above.
   * Without the override the USB DMA and the CM7 D-cache observe different
   * contents and enumeration / transfers fail.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress = 0x2406E000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_16KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
""")

if crlf:
    text = text.replace('\n', '\r\n')
open(path, 'wb').write(text.encode('utf-8'))
print('patched', path)
