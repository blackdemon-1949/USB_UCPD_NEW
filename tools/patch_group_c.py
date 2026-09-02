#!/usr/bin/env python3
"""Repair group C: Boot MPU regions, SPI footprint adaptation to the new
CubeMX configuration, TRACER_EMB de-duplication and the GPDMA DREQ fix."""
import sys, os, shutil

ROOT = sys.argv[1]
REF = sys.argv[2]

def patch(rel, pairs):
    p = os.path.join(ROOT, rel)
    raw = open(p, 'rb').read()
    crlf = b'\r\n' in raw
    t = raw.decode('utf-8')
    if crlf:
        t = t.replace('\r\n', '\n')
    for old, new in pairs:
        n = t.count(old)
        if n == 0 and t.count(new) == 1:
            print('  (already applied)', rel)
            continue
        assert n == 1, '%s: expected 1 occurrence, found %d of:\n%r' % (rel, n, old[:240])
        t = t.replace(old, new)
    if crlf:
        t = t.replace('\n', '\r\n')
    open(p, 'wb').write(t.encode('utf-8'))
    print('patched', rel)

# ------------------------------------------------------------- Boot main.c
patch('Boot/Core/Src/main.c', [
("""  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;""",
 """  /** Region 0: 4 GB background, no access (subregions 0,1,2,7 disabled = 0x87)
   *
   * Only subregions 3..6 (0x60000000-0xDFFFFFFF) stay enabled, and even those
   * are NO_ACCESS: everything the bootloader touches has its own region below.
   */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;"""),
("""  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x08000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_64KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
""",
 """  /** Region 1: internal 64 KB boot FLASH
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x08000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_64KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 2: 8 MB XSPI1 NOR (PY25Q64HA)
   *
   * Mandatory: the bootloader jumps to the application at 0x90000000, and the
   * background region above denies access to that address range.  Without this
   * region the very first XIP fetch of the application vectors faults.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 3: AXI SRAM (0x24000000, .data/.bss/stack/heap of both images)
   *
   * The background region disables subregion 1 (0x20000000-0x3FFFFFFF), so
   * without an explicit region here the bootloader cannot touch its own RAM.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
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

  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
"""),
])

# --------------------------------------------------------------- ext_spi.c
patch('Appli/Core/Src/ext_spi.c', [
("""/**
 * @file    ext_spi.c
 * @brief   SPI extension footprint - SPI2 (SCK PD3 / MOSI PC1 / MISO PB14).
 *
 * SPI2 is configured by CubeMX in spi.c (MX_SPI2_Init, 37.5 MBit/s master,
 * 8-bit, full duplex).  The board routes PA0 (LCD_CS), PA1 (LCD_DC/RS) and
 * PA4 (LCD_RST) for a future 4-wire SPI LCD panel.
 */""",
 """/**
 * @file    ext_spi.c
 * @brief   SPI extension footprint.
 *
 * The previous CubeMX configuration brought up SPI2 here (MX_SPI2_Init,
 * 37.5 MBit/s master, 8-bit, full duplex, SCK PD3 / MOSI PC1 / MISO PB14) with
 * PA0 (LCD_CS), PA1 (LCD_DC/RS) and PA4 (LCD_RST) for a 4-wire LCD panel.
 * The current USB_UCPD.ioc no longer instantiates SPI2 - those pins are free
 * and PD5/PD6 now carry USART2 - so CubeMX stopped generating spi.c/spi.h and
 * HAL_SPI_MODULE_ENABLED is off.
 *
 * The footprint (weak feature hooks + API) is intentionally preserved and
 * tracks the configuration: with no SPI bus this module reports "not ready",
 * EXT_SPI_Transfer() returns HAL_ERROR, and re-enabling SPI2 in CubeMX brings
 * the bus back with no edit here.  See ext_spi.h.
 */"""),
("""void EXT_SPI_Init(void)
{
  /* The peripheral itself is brought up by MX_SPI2_Init() in main.c.
   * This hook is the application-level extension point that runs after it. */
  if (hspi2.Instance != SPI2)
  {
    APP_LOG_Printf("ext-spi: ERROR SPI2 not initialised\\r\\n");
    return;
  }

  /* Put the LCD control lines in a benign idle state (they come out of
   * MX_GPIO_Init held low): deselect the panel and release its reset. */
  EXT_SPI_LCD_CS_SET();
  EXT_SPI_LCD_RST_SET();
  EXT_SPI_LCD_DC_SET();

  APP_LOG_Printf("ext-spi: SPI2 ready  SCK=PD3 MOSI=PC1 MISO=PB14 37.5 MBit/s, "
                 "LCD CS=PA0 DC=PA1 RST=PA4\\r\\n");

  /* Feature hook: future SPI projects add their one-time setup here. */
  EXT_SPI_FeatureInit();
}""",
 """void EXT_SPI_Init(void)
{
#if EXT_SPI_HAVE_BUS
  /* The peripheral itself is brought up by CubeMX (MX_SPIx_Init) in main.c.
   * This hook is the application-level extension point that runs after it. */
  if (hspi2.Instance != SPI2)
  {
    APP_LOG_Printf("ext-spi: ERROR SPI2 not initialised\\r\\n");
    return;
  }

  /* Put the LCD control lines in a benign idle state (they come out of
   * MX_GPIO_Init held low): deselect the panel and release its reset. */
  EXT_SPI_LCD_CS_SET();
  EXT_SPI_LCD_RST_SET();
  EXT_SPI_LCD_DC_SET();

  APP_LOG_Printf("ext-spi: SPI2 ready  SCK=PD3 MOSI=PC1 MISO=PB14 37.5 MBit/s, "
                 "LCD CS=PA0 DC=PA1 RST=PA4\\r\\n");
#else
  /* No SPI peripheral in the current CubeMX configuration - keep the
   * footprint alive (the weak hooks still run) but do not pretend a bus
   * exists.  The `pd` / `board` CLI output stays honest. */
  APP_LOG_Printf("ext-spi: no SPI peripheral in this configuration - "
                 "footprint idle\\r\\n");
#endif

  /* Feature hook: future SPI projects add their one-time setup here. */
  EXT_SPI_FeatureInit();
}"""),
("""uint8_t EXT_SPI_IsReady(void)
{
  return (hspi2.Instance == SPI2) ? 1U : 0U;
}""",
 """uint8_t EXT_SPI_IsReady(void)
{
#if EXT_SPI_HAVE_BUS
  return (hspi2.Instance == SPI2) ? 1U : 0U;
#else
  return 0U;
#endif
}"""),
("""HAL_StatusTypeDef EXT_SPI_Transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
  if (!EXT_SPI_IsReady())
  {
    return HAL_ERROR;
  }
  /* Single 8-bit fill so a NULL TX pointer still clocks the bus. */
  uint8_t fill = 0xFFU;
  const uint8_t *src = (tx != NULL) ? tx : &fill;
  if (rx != NULL)
  {
    return HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)src, rx, len, HAL_MAX_DELAY);
  }
  return HAL_SPI_Transmit(&hspi2, (uint8_t *)src, len, HAL_MAX_DELAY);
}""",
 """HAL_StatusTypeDef EXT_SPI_Transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
#if EXT_SPI_HAVE_BUS
  if (!EXT_SPI_IsReady())
  {
    return HAL_ERROR;
  }
  /* Single 8-bit fill so a NULL TX pointer still clocks the bus. */
  uint8_t fill = 0xFFU;
  const uint8_t *src = (tx != NULL) ? tx : &fill;
  if (rx != NULL)
  {
    return HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)src, rx, len, HAL_MAX_DELAY);
  }
  return HAL_SPI_Transmit(&hspi2, (uint8_t *)src, len, HAL_MAX_DELAY);
#else
  UNUSED(tx);
  UNUSED(rx);
  UNUSED(len);
  return HAL_ERROR;
#endif
}"""),
])

# ------------------------------------------------------- TRACER_EMB (shared)
# The Appli project now links Utilities/TRACER_EMB/*.c (see Appli/.project);
# the copies left behind in Appli/Core/Src would give duplicate definitions.
stale = [
    'Appli/Core/Src/tracer_emb.c',
    'Appli/Core/Src/tracer_emb_hw.c',
    'Appli/Core/Inc/tracer_emb.h',
    'Appli/Core/Inc/tracer_emb_hw.h',
]
for rel in stale:
    p = os.path.join(ROOT, rel)
    if os.path.exists(p):
        os.remove(p)
        print('removed stale duplicate', rel)

# Port the GPDMA DREQ fix from the previous hand-adapted copy into the shared
# middleware file that the build actually compiles.
patch('Utilities/TRACER_EMB/tracer_emb_hw.c', [
("""  LL_DMA_SetPeriphRequest(TRACER_EMB_DMA_INSTANCE, TRACER_EMB_TX_DMA_CHANNEL, TRACER_EMB_TX_DMA_REQUEST);

""",
 """  LL_DMA_SetPeriphRequest(TRACER_EMB_DMA_INSTANCE, TRACER_EMB_TX_DMA_CHANNEL, TRACER_EMB_TX_DMA_REQUEST);

  /* On GPDMA/HPDMA the transfer direction lives in CTR2.DREQ/SWREQ and is NOT
     set by LL_DMA_ConfigTransfer()/LL_DMA_SetPeriphRequest().  The trace TX is
     memory-to-peripheral (USART TDR), so DREQ must be set, exactly as the UCPD
     TX channel does in usbpd_hw.c.  Without this the GPDMA never services the
     USART TX request and no trace bytes leave the chip. */
  LL_DMA_SetDataTransferDirection(TRACER_EMB_DMA_INSTANCE, TRACER_EMB_TX_DMA_CHANNEL,
                                  LL_DMA_DIRECTION_MEMORY_TO_PERIPH);

"""),
])

print('OK')
