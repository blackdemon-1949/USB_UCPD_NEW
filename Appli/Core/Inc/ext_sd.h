/**
  ******************************************************************************
  * @file    ext_sd.h
  * @brief   SD card (SDMMC1 + FAT32) extension footprint.
  *
   * ==========================================================================
   *  FOOTPRINT FOR SD-CARD-BACKED FEATURES
   * ==========================================================================
   * SDMMC1 is brought up by CubeMX in sdmmc.c (MX_SDMMC1_SD_Init: 4-bit bus,
   * polling I/O for bring-up, PA8 card-detect input), and FatFS middleware
   * is initialised in fatfs.c (MX_FATFS_Init).
   *
   * This module is the single place SD-backed features plug in:
   *    1. Implement EXT_SD_FeatureInit() - one-time setup after the FAT
   *       volume is mounted (first card insert after boot, or re-insert
   *       after a yank).
   *    2. Implement EXT_SD_FeaturePoll() - periodic work from the super
   *       loop; only runs while a card is present and mounted.
   *
   * Both hooks are weak: features override them in their own source files
   * without editing this module or main.c.
   *
   * -------------------------------------------------------------------------
   *  SAFETY / HOT-PLUG MODEL
   * -------------------------------------------------------------------------
   *  - Card presence is debounced (SD_DETECT_DEBOUNCE_MS) before mount /
   *    unmount.
   *  - f_mount()/f_mount(NULL,...) are only called from the main loop
   *    (never from an ISR).
   *  - File handles are OPENED/WRITTEN/CLOSED PER CALL (EXT_SD_AppendLine).
   *    A yanked card at any point therefore cannot leave a dangling handle
   *    or corrupt the filesystem: the worst case is one truncated record.
   *  - Every write uses a finite timeout; a wedged card returns HAL_ERROR
   *    rather than blocking the super loop.
   *  - No NOR flash writes are ever made from this path (see
   *    FLASH_ENDURANCE.md); all persistence is to the SD card only.
   *
   * Card-detect polarities:
   *    PA8 assumed ACTIVE-LOW (external pull-up, shell-switch pulls to GND
   *    when a card is seated).  VERIFY on real hardware with
   *    `ext-sd: SDMMC1 ready card=...` at boot and insert/remove watching
   *    the log; flip EXT_SD_ReadDetectRaw() if backwards.
   *
   * Note on DMA: STM32H7R3/H7S3 uses GPDMA1 (not classic DMA1/DMA2) for
   * any future DMA-mode SDMMC1 transfers.  Bring-up uses the HAL's internal
   * SDMMC IDMA path (single-buffer IDMA enabled inside HAL_SD_ReadBlocks/
   * WriteBlocks_DMA); polling mode is fine for log/energy writes, so we
   * leave GPDMA channel wiring for a later optimisation.
   * ==========================================================================
  */
#ifndef EXT_SD_H
#define EXT_SD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "sdmmc.h"
#include "fatfs.h"

/* --- Extension hooks (override to add SD-backed features) ------------------ */
void EXT_SD_FeatureInit(void);   /* weak - called once after a mount */
void EXT_SD_FeaturePoll(void);   /* weak - called each loop while mounted */

/* --- Footprint API -------------------------------------------------------- */
void              EXT_SD_Init(void);
void              EXT_SD_Poll(void);
uint8_t           EXT_SD_IsMounted(void);
uint8_t           EXT_SD_IsCardPresent(void);
SD_HandleTypeDef *EXT_SD_GetHandle(void);   /* used by sd_diskio_config.h */
void              EXT_SD_HardInit(void);    /* used by sd_diskio_config.h */

#define EXT_SD_TIMEOUT_MS         2000U
#define EXT_SD_DETECT_DEBOUNCE_MS 200U

/**
 * @brief  Append `len` bytes from `data` to the file at `path` on the SD
 *         card.  Opens, writes and closes the file in a single call so a
 *         yanked card cannot corrupt the FS.
 * @return HAL_OK on success; HAL_ERROR if unmounted / any FatFS call fails.
 */
HAL_StatusTypeDef EXT_SD_AppendLine(const char *path, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* EXT_SD_H */
