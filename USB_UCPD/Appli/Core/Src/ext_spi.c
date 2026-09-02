/**
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
 */
#include "ext_spi.h"
#include "app_log.h"

/* ==========================================================================
 *  FEATURE REGISTRATION
 *  --------------------------------------------------------------------------
 *  Future SPI feature projects override these two weak hooks with their own
 *  strong definitions (no edits needed here or in main.c):
 *
 *    void EXT_SPI_FeatureInit(void)  - one-time setup, called from
 *                                      EXT_SPI_Init() after MX_SPI2_Init().
 *    void EXT_SPI_FeaturePoll(void)  - periodic work, called from
 *                                      EXT_SPI_Poll() every super-loop pass.
 *
 *  Worked example (ST7789-style 4-wire SPI LCD on SPI2 + PA0/PA1/PA4):
 *
 *    #include "ext_spi.h"
 *
 *    static void lcd_cmd(uint8_t cmd)
 *    {
 *      EXT_SPI_LCD_DC_RESET();             // command phase
 *      (void)EXT_SPI_Transfer(&cmd, NULL, 1);
 *      EXT_SPI_LCD_DC_SET();               // back to data phase
 *    }
 *
 *    static void lcd_data(const uint8_t *d, uint16_t n)
 *    {
 *      EXT_SPI_LCD_DC_SET();
 *      (void)EXT_SPI_Transfer(d, NULL, n);
 *    }
 *
 *    void EXT_SPI_FeatureInit(void)
 *    {
 *      EXT_SPI_LCD_CS_SET();               // deselect until needed
 *      EXT_SPI_LCD_RST_RESET();            // hold reset > 10 us
 *      HAL_Delay(20);
 *      EXT_SPI_LCD_RST_SET();              // release reset
 *      EXT_SPI_LCD_CS_RESET();             // select
 *      lcd_cmd(0x01);                      // SWRESET
 *      HAL_Delay(150);
 *      lcd_cmd(0x11);                      // SLPOUT
 *      HAL_Delay(120);
 *      lcd_cmd(0x3A);                      // COLMOD 16 bpp
 *      { uint8_t f = 0x05; lcd_data(&f, 1); }
 *      lcd_cmd(0x29);                      // DISPON
 *      EXT_SPI_LCD_CS_SET();               // deselect
 *      APP_LOG_Printf("lcd: ST7789 init done\r\n");
 *    }
 *
 *    void EXT_SPI_FeaturePoll(void)
 *    {
 *      // e.g. periodic framebuffer refresh or backlight PWM updates.
 *    }
 * ========================================================================== */

__weak void EXT_SPI_FeatureInit(void)
{
  /* Add future SPI feature initialisation here (or override this hook). */
}

__weak void EXT_SPI_FeaturePoll(void)
{
  /* Add future SPI feature polling here (or override this hook). */
}

/* ==========================================================================
 *  FOOTPRINT API
 * ========================================================================== */

void EXT_SPI_Init(void)
{
#if EXT_SPI_HAVE_BUS
  /* The peripheral itself is brought up by CubeMX (MX_SPIx_Init) in main.c.
   * This hook is the application-level extension point that runs after it. */
  if (hspi2.Instance != SPI2)
  {
    APP_LOG_Printf("ext-spi: ERROR SPI2 not initialised\r\n");
    return;
  }

  /* Put the LCD control lines in a benign idle state (they come out of
   * MX_GPIO_Init held low): deselect the panel and release its reset. */
  EXT_SPI_LCD_CS_SET();
  EXT_SPI_LCD_RST_SET();
  EXT_SPI_LCD_DC_SET();

  APP_LOG_Printf("ext-spi: SPI2 ready  SCK=PD3 MOSI=PC1 MISO=PB14 37.5 MBit/s, "
                 "LCD CS=PA0 DC=PA1 RST=PA4\r\n");
#else
  /* No SPI peripheral in the current CubeMX configuration - keep the
   * footprint alive (the weak hooks still run) but do not pretend a bus
   * exists.  The `pd` / `board` CLI output stays honest. */
  APP_LOG_Printf("ext-spi: no SPI peripheral in this configuration - "
                 "footprint idle\r\n");
#endif

  /* Feature hook: future SPI projects add their one-time setup here. */
  EXT_SPI_FeatureInit();
}

void EXT_SPI_Poll(void)
{
  /* Feature hook: future SPI projects add their periodic work here.
   * Runs on every super-loop pass; keep it short (non-blocking or
   * time-gated) so the PD stack, USB CDC and CLI stay responsive. */
  EXT_SPI_FeaturePoll();
}

uint8_t EXT_SPI_IsReady(void)
{
#if EXT_SPI_HAVE_BUS
  return (hspi2.Instance == SPI2) ? 1U : 0U;
#else
  return 0U;
#endif
}

HAL_StatusTypeDef EXT_SPI_Transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
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
}
