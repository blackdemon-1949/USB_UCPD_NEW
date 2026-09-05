/**
  ******************************************************************************
  * @file    ext_sd.c
  * @brief   SD card extension footprint - mount/unmount + append-write helper.
  *
  * Mount/unmount with debounced hot-plug; open/write/close per call so a
  * yanked card can never corrupt the filesystem (the worst case is a
  * single truncated record).  Follows the same shape as ext_i2c/ext_dts/
  * ext_uart.
  *
  * Phase 1 bring-up uses the ST HAL_SD driver's internal IDMA (single-buffer
  * IDMA inside SDMMC1) accessed via polling HAL_SD_ReadBlocks/WriteBlocks
  * (see sd_diskio.c).  No GPDMA channel wiring is needed for log/energy
  * writes; a later optimisation can switch to HAL_SD_*_DMA and a GPDMA1
  * channel.
  ******************************************************************************
  */
#include "ext_sd.h"
#include "app_log.h"
#include <string.h>

/* ==========================================================================
 *  FEATURE REGISTRATION
 * --------------------------------------------------------------------------
 *  SD-backed features override these two weak hooks with their own strong
 *  definitions (no edits needed here or in main.c):
 *
 *    void EXT_SD_FeatureInit(void)  - one-time setup, called after a
 *                                     successful f_mount() on card insert.
 *    void EXT_SD_FeaturePoll(void)  - periodic work, called from the super
 *                                     loop only while mounted.
 *
 *  Worked example (energy checkpoints from ina226_energy.c):
 *
 *    void EXT_SD_FeatureInit(void) { INA226_Energy_OnSdMount(); }
 *    void EXT_SD_FeaturePoll(void) { INA226_Energy_PollCheckpoint(); }
 * ========================================================================== */

__weak void EXT_SD_FeatureInit(void)
{
  /* Add future SD feature initialisation here (or override this hook). */
}

__weak void EXT_SD_FeaturePoll(void)
{
  /* Add future SD feature polling here (or override this hook). */
}

/* ==========================================================================
 *  FOOTPRINT STATE
 * ========================================================================== */

static uint8_t  s_mounted = 0U;
static uint8_t  s_present_stable = 0U;
static uint8_t  s_present_raw = 0U;
static uint32_t s_debounce_t0 = 0U;

/* Storage for sd_diskio.c's extern sdmmc_handle pointer (see sd_diskio_config.h).
 * Kept as a pointer (rather than a second SD_HandleTypeDef) so the HAL state
 * and cached card info live in the single hsd1 declared in sdmmc.c. */
SD_HandleTypeDef *sdmmc_handle = NULL;

SD_HandleTypeDef *EXT_SD_GetHandle(void)
{
  return &hsd1;
}

/**
 * @brief  (Re)initialise the SDMMC peripheral + card.  Called from
 *         sd_diskio.c's SD_initialize() (ENABLE_SD_INIT path), and by
 *         EXT_SD_TryMount() on hot-plug.
 */
void EXT_SD_HardInit(void)
{
  HAL_SD_CardInfoTypeDef info;

  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    /* No card / wedge - do not stick in Error_Handler; hot-plug will retry. */
    return;
  }
  /* Switch to 4-bit bus after 1-bit init succeeded. */
  (void)HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B);
  (void)HAL_SD_GetCardInfo(&hsd1, &info);
  APP_LOG_Printf("ext-sd: card  type=%lu  blocks=%lu  blocksize=%lu  logblocks=%lu\r\n",
                 (unsigned long)info.CardType,
                 (unsigned long)info.BlockNbr,
                 (unsigned long)info.BlockSize,
                 (unsigned long)info.LogBlockNbr);
}

/**
 * @brief  Read the raw PA8 card-detect pin.
 *
 * ASSUMES active-low polarity (socket switch pulls PA8 low when a card is
 * seated, external pull-up holds it high when empty).  TODO: verify on
 * real hardware at Phase 1 bring-up; flip the return value if insert/remove
 * log lines are inverted.
 */
static uint8_t EXT_SD_ReadDetectRaw(void)
{
  GPIO_PinState pin = HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin);
  return (pin == GPIO_PIN_RESET) ? 1U : 0U;
}

/* ==========================================================================
 *  MOUNT / UNMOUNT
 * ========================================================================== */

static void EXT_SD_TryMount(void)
{
  FRESULT fr;
  /* Re-initialise the SDMMC peripheral before mounting in case the card
   * was re-inserted after a yank - the HAL_SD handle can be left in an
   * error state from the removal. */
  (void)HAL_SD_DeInit(&hsd1);
  EXT_SD_HardInit();

  fr = f_mount(&SDFatFS, (const TCHAR *)SDPath, 1U);
  s_mounted = (fr == FR_OK) ? 1U : 0U;
  if (s_mounted)
  {
    APP_LOG_Printf("ext-sd: mounted FAT volume at '%s'\r\n", SDPath);
    EXT_SD_FeatureInit();
  }
  else
  {
    APP_LOG_Printf("ext-sd: mount failed (fr=%d) - will retry on next insert\r\n",
                   (int)fr);
  }
}

static void EXT_SD_Unmount(void)
{
  if (s_mounted)
  {
    (void)f_mount(NULL, (const TCHAR *)SDPath, 0U);
    s_mounted = 0U;
    APP_LOG_Write("ext-sd: card removed, unmounted\r\n");
  }
}

/* ==========================================================================
 *  FOOTPRINT API
 * ========================================================================== */

void EXT_SD_Init(void)
{
  /* The SDMMC1 peripheral itself is brought up by MX_SDMMC1_SD_Init() /
   * MX_FATFS_Init() in main.c.  This hook is the application-level
   * extension point that runs after them. */
  sdmmc_handle = &hsd1;
  s_present_raw = s_present_stable = EXT_SD_ReadDetectRaw();
  s_debounce_t0 = HAL_GetTick();
  s_mounted = 0U;
  APP_LOG_Printf("ext-sd: SDMMC1 ready  card=%s  (PA8 detect, active-low, verify on hardware)\r\n",
                 s_present_stable ? "present" : "absent");
}

void EXT_SD_Poll(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t  raw = EXT_SD_ReadDetectRaw();

  if (raw != s_present_raw)
  {
    /* Glitch / edge - (re)start the debounce timer. */
    s_present_raw = raw;
    s_debounce_t0  = now;
  }
  else if ((raw != s_present_stable) &&
           ((now - s_debounce_t0) >= EXT_SD_DETECT_DEBOUNCE_MS))
  {
    /* Debounced state change. */
    s_present_stable = raw;
    if (s_present_stable)
    {
      APP_LOG_Write("ext-sd: card inserted\r\n");
      EXT_SD_TryMount();
    }
    else
    {
      EXT_SD_Unmount();
    }
  }

  if (s_mounted)
  {
    EXT_SD_FeaturePoll();
  }
}

uint8_t EXT_SD_IsMounted(void)
{
  return s_mounted;
}

uint8_t EXT_SD_IsCardPresent(void)
{
  return s_present_stable;
}

/**
  * @brief  Append bytes to a file on the SD card.  The file is opened,
  *         written and closed in a single call - handles are never held
  *         open across polls, so a physically yanked card cannot corrupt
  *         the FAT.
  * @retval HAL_OK on full write; HAL_ERROR on any FatFS failure.
  */
HAL_StatusTypeDef EXT_SD_AppendLine(const char *path, const uint8_t *data, uint16_t len)
{
  FIL      fil;
  UINT     written = 0U;
  FRESULT  fr;

  if ((s_mounted == 0U) || (path == NULL) || (data == NULL) || (len == 0U))
  {
    return HAL_ERROR;
  }

  fr = f_open(&fil, path, FA_OPEN_APPEND | FA_WRITE);
  if (fr != FR_OK)
  {
    APP_LOG_Printf("ext-sd: open '%s' failed (fr=%d)\r\n", path, (int)fr);
    return HAL_ERROR;
  }
  fr = f_write(&fil, data, (UINT)len, &written);
  /* Always close before checking the write result - we never want to leak
   * a handle on partial write. */
  (void)f_close(&fil);
  if ((fr != FR_OK) || (written != (UINT)len))
  {
    APP_LOG_Printf("ext-sd: write '%s' failed (fr=%d written=%u/%u)\r\n",
                   path, (int)fr, (unsigned)written, (unsigned)len);
    return HAL_ERROR;
  }
  return HAL_OK;
}
