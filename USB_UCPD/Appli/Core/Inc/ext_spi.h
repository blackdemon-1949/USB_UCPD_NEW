#ifndef EXT_SPI_H
#define EXT_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/**
 * @file    ext_spi.h
 * @brief   SPI extension footprint.
 *
 * ============================================================================
 *  FOOTPRINT FOR FUTURE SPI FEATURED PROJECTS
 * ============================================================================
 *  This module is the single place where future SPI-based features plug in.
 *
 *  ------------------------------------------------------------------------
 *  CONFIGURATION NOTE
 *  ------------------------------------------------------------------------
 *  The previous CubeMX configuration instantiated SPI2 as a 37.5 MBit/s
 *  full-duplex master (SCK PD3 / MOSI PC1 / MISO PB14) plus three LCD control
 *  GPIOs (PA0 CS, PA1 DC/RS, PA4 RST).  The current USB_UCPD.ioc does not
 *  instantiate any SPI peripheral - PB14/PC1/PD3 are released and PD5/PD6 now
 *  carry USART2 - so CubeMX no longer generates spi.c/spi.h and
 *  HAL_SPI_MODULE_ENABLED is off in stm32h7rsxx_hal_conf.h.
 *
 *  The footprint itself is deliberately kept: the public API and both weak
 *  feature hooks are unchanged, so main.c and any feature built on top of it
 *  still compile and run.  Availability is derived from the CubeMX
 *  configuration, so re-enabling SPI2 in CubeMX (which regenerates spi.c/spi.h
 *  and turns the HAL SPI module back on) restores the bus and the transfer
 *  helper without editing this file.
 *  ------------------------------------------------------------------------
 *
 *  To add a feature:
 *    1. Implement EXT_SPI_FeatureInit()  - one-time setup after the SPI
 *       peripheral is up.  Called from EXT_SPI_Init().
 *    2. Implement EXT_SPI_FeaturePoll()  - periodic work from the super
 *       loop.  Called from EXT_SPI_Poll().
 *    Both hooks are weak: a future project simply defines strong versions
 *    in its own source files without editing this module or main.c.
 *
 *  Low-level helpers (EXT_SPI_Transfer, LCD pin control) are provided below.
 *  See ext_spi.c for a worked ST7789-style LCD example.
 * ============================================================================
 */

/* --- Configuration-derived availability ----------------------------------- */
#if defined(HAL_SPI_MODULE_ENABLED)
#  include "spi.h"
#  define EXT_SPI_HAVE_BUS  1U
#else
#  define EXT_SPI_HAVE_BUS  0U
#endif

/* The LCD panel GPIOs exist only if the .ioc still labels them. */
#if defined(LCD_CS_Pin) && defined(LCD_DC_RS_Pin) && defined(LCD_RST_Pin)
#  define EXT_SPI_HAVE_LCD_PINS  1U
#else
#  define EXT_SPI_HAVE_LCD_PINS  0U
#endif

/* --- Extension hooks (override to add features) --------------------------- */
void EXT_SPI_FeatureInit(void);   /* weak - feature one-time init   */
void EXT_SPI_FeaturePoll(void);   /* weak - feature periodic poll   */

/* --- Footprint API -------------------------------------------------------- */
void EXT_SPI_Init(void);          /* call once from main()          */
void EXT_SPI_Poll(void);          /* call from the super loop       */
uint8_t EXT_SPI_IsReady(void);    /* 1 if an SPI bus is initialised */

/**
 * @brief  Full-duplex SPI transfer (blocking).
 * @param  tx   TX data (NULL -> sends 0xFF).
 * @param  rx   RX data (NULL -> discards).
 * @param  len  Number of bytes.
 * @return HAL status (HAL_OK on success, HAL_ERROR when no bus is present).
 */
HAL_StatusTypeDef EXT_SPI_Transfer(const uint8_t *tx, uint8_t *rx, uint16_t len);

/* --- LCD control pins (PA0 CS, PA1 DC, PA4 RST) --------------------------- */
#if EXT_SPI_HAVE_LCD_PINS
#define EXT_SPI_LCD_CS_SET()   HAL_GPIO_WritePin(LCD_CS_GPIO_Port,    LCD_CS_Pin,    GPIO_PIN_SET)
#define EXT_SPI_LCD_CS_RESET() HAL_GPIO_WritePin(LCD_CS_GPIO_Port,    LCD_CS_Pin,    GPIO_PIN_RESET)
#define EXT_SPI_LCD_DC_SET()   HAL_GPIO_WritePin(LCD_DC_RS_GPIO_Port, LCD_DC_RS_Pin, GPIO_PIN_SET)
#define EXT_SPI_LCD_DC_RESET() HAL_GPIO_WritePin(LCD_DC_RS_GPIO_Port, LCD_DC_RS_Pin, GPIO_PIN_RESET)
#define EXT_SPI_LCD_RST_SET()  HAL_GPIO_WritePin(LCD_RST_GPIO_Port,   LCD_RST_Pin,   GPIO_PIN_SET)
#define EXT_SPI_LCD_RST_RESET() HAL_GPIO_WritePin(LCD_RST_GPIO_Port,  LCD_RST_Pin,   GPIO_PIN_RESET)
#else
#define EXT_SPI_LCD_CS_SET()    ((void)0)
#define EXT_SPI_LCD_CS_RESET()  ((void)0)
#define EXT_SPI_LCD_DC_SET()    ((void)0)
#define EXT_SPI_LCD_DC_RESET()  ((void)0)
#define EXT_SPI_LCD_RST_SET()   ((void)0)
#define EXT_SPI_LCD_RST_RESET() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* EXT_SPI_H */
