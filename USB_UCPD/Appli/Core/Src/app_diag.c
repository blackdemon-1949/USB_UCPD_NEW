/**
 * @file    app_diag.c
 * @brief   Diagnostic counters and health snapshot (see app_diag.h).
 */
#include "app_diag.h"
#include "main.h"          /* CMSIS core: MPU_Type and MPU_RASR_* masks */
#include "app_log.h"
#include "app_cap.h"
#include "app_txn.h"
#include "app_epr.h"
#include "app_temp.h"
#include "app_cable.h"

#include <string.h>
#include <stdio.h>

/* Provided by the linker (STM32H7R3Z8JX_ROMxspi1.ld) and by usbd_cdc_if.c */
extern uint32_t _Heap_Limit;                 /* = __RAM_BEGIN + __RAM_SIZE  */
extern uint8_t  UserRxBufferHS[];
extern uint8_t  UserTxBufferHS[];

static volatile uint32_t s_c[APP_DIAG_COUNT];

static const char *const s_name[APP_DIAG_COUNT] =
{
  "pd_rx", "pd_tx", "goodcrc_rx", "goodcrc_tx", "pd_retry",
  "soft_reset", "hard_reset", "protocol_err", "crc_err", "pd_timeout",
  "pd_unmatched", "pd_malformed",
  "neg_caps", "neg_request", "neg_accept", "neg_reject", "neg_wait",
  "neg_contract", "neg_epr",
  "attach", "detach", "cad_event",
  "cap_records", "cap_drops", "cap_clipped",
  "cdc_tx", "cdc_rx", "cdc_tx_busy", "cdc_tx_fail", "cdc_overflow",
  "cdc_bus_reset", "cdc_suspend",
  "i2c_err", "i2c_timeout", "ina_missing",
  "dma_err", "dma_tc", "cache_inv", "cache_clean",
  "pwr_samples", "temp_samples", "temp_alert"
};

void APP_DIAG_Init(void)
{
  memset((void *)s_c, 0, sizeof(s_c));
}

void APP_DIAG_Inc(APP_DIAG_Id_t id)
{
  if ((unsigned)id < (unsigned)APP_DIAG_COUNT)
  {
    s_c[id]++;
  }
}

void APP_DIAG_Add(APP_DIAG_Id_t id, uint32_t n)
{
  if ((unsigned)id < (unsigned)APP_DIAG_COUNT)
  {
    s_c[id] += n;
  }
}

uint32_t APP_DIAG_Get(APP_DIAG_Id_t id)
{
  return ((unsigned)id < (unsigned)APP_DIAG_COUNT) ? s_c[id] : 0u;
}

const char *APP_DIAG_Name(APP_DIAG_Id_t id)
{
  return ((unsigned)id < (unsigned)APP_DIAG_COUNT) ? s_name[id] : "?";
}

void APP_DIAG_GetAll(APP_DIAG_Snapshot_t *out)
{
  int i;

  if (out == NULL)
  {
    return;
  }
  for (i = 0; i < (int)APP_DIAG_COUNT; i++)
  {
    out->c[i] = s_c[i];
  }
}

void APP_DIAG_Clear(void)
{
  memset((void *)s_c, 0, sizeof(s_c));
}

/* ------------------------------------------------------------------ */
/* DMA / cache coherency self-check                                    */
/* ------------------------------------------------------------------ */

int APP_DIAG_CheckCoherency(void)
{
  uint32_t target = (uint32_t)&UserRxBufferHS[0];
  uint32_t rnr;
  int found = 0;
  uint8_t i;

  for (i = 0u; i < 8u; i++)
  {
    uint32_t rbar;
    uint32_t rasr;
    uint32_t size;
    uint32_t base;

    MPU->RNR = i;
    rbar = MPU->RBAR;
    rasr = MPU->RASR;

    if ((rasr & MPU_RASR_ENABLE_Msk) == 0u)
    {
      continue;
    }

    size = 1uL << ((((rasr & MPU_RASR_SIZE_Msk) >> MPU_RASR_SIZE_Pos) + 1u));
    base = rbar & ~(size - 1u);

    if ((target >= base) && (target < (base + size)))
    {
      uint32_t tex = (rasr & MPU_RASR_TEX_Msk) >> MPU_RASR_TEX_Pos;
      uint32_t c   = (rasr & MPU_RASR_C_Msk) >> MPU_RASR_C_Pos;
      uint32_t b   = (rasr & MPU_RASR_B_Msk) >> MPU_RASR_B_Pos;
      uint32_t s   = (rasr & MPU_RASR_S_Msk) >> MPU_RASR_S_Pos;

      found = 1;

      APP_LOG_Printf("  region %u covers the USB DMA buffers\r\n", (unsigned)i);
      APP_LOG_Printf("    base 0x%08lX size %lu KiB\r\n",
                     (unsigned long)base, (unsigned long)(size / 1024u));
      APP_LOG_Printf("    TEX=%lu C=%lu B=%lu S=%lu\r\n",
                     (unsigned long)tex, (unsigned long)c,
                     (unsigned long)b, (unsigned long)s);

      /* A region is base-aligned only when the base is a multiple of its own
       * size; otherwise the hardware silently rounds the base down. */
      if ((base & (size - 1u)) != 0u)
      {
        APP_LOG_Write("    !! base is NOT aligned to the region size\r\n");
        return 0;
      }
      /* TEX=0 C=0 B=1 S=1 is non-cacheable, shareable: DMA and CPU agree. */
      if ((tex == 0u) && (c == 0u) && (b == 1u) && (s == 1u))
      {
        APP_LOG_Write("    non-cacheable shareable: coherency OK\r\n");
        return 1;
      }
      APP_LOG_Write("    !! attributes are not non-cacheable shareable\r\n");
      return 0;
    }
  }

  (void)rnr;
  if (found == 0)
  {
    APP_LOG_Write("  no enabled MPU region covers the USB DMA buffers\r\n");
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void dump_range(APP_DIAG_Id_t lo, APP_DIAG_Id_t hi, const char *title)
{
  APP_DIAG_Id_t i;

  APP_LOG_Printf("%s:\r\n", title);
  for (i = lo; i <= hi; i++)
  {
    APP_LOG_Printf("  %-16s %lu\r\n", APP_DIAG_Name(i),
                   (unsigned long)s_c[i]);
  }
}

int APP_DIAG_Cmd(int argc, char *argv[])
{
  APP_CAP_Stats_t cap;
  const char *sub;

  sub = (argc >= 2) ? argv[1] : "all";

  if (strcmp(sub, "coherency") == 0)
  {
    APP_LOG_Write("DMA/cache coherency\r\n");
    APP_LOG_Printf("  UserRxBufferHS @ 0x%08lX\r\n",
                   (unsigned long)&UserRxBufferHS[0]);
    APP_LOG_Printf("  UserTxBufferHS @ 0x%08lX\r\n",
                   (unsigned long)&UserTxBufferHS[0]);
    APP_LOG_Printf("  _Heap_Limit    @ 0x%08lX\r\n",
                   (unsigned long)&_Heap_Limit);
    (void)APP_DIAG_CheckCoherency();
    return 1;
  }

  if ((strcmp(sub, "all") == 0) || (strcmp(sub, "counters") == 0))
  {
    dump_range(APP_DIAG_PD_RX, APP_DIAG_PD_MALFORMED, "PD protocol");
    dump_range(APP_DIAG_NEG_CAPS, APP_DIAG_NEG_EPR, "negotiation");
    dump_range(APP_DIAG_ATTACH, APP_DIAG_CAD_EVENT, "attach");
    dump_range(APP_DIAG_CDC_TX, APP_DIAG_CDC_SUSPEND, "USB CDC");
    /* Console health, measured rather than assumed: bytes lost to a full
     * queue and IN transfers the stall watchdog had to abandon.  Both being
     * zero is what "the CDC is healthy" actually means. */
    APP_LOG_Printf("  log_dropped      %lu\r\n",
                   (unsigned long)APP_LOG_Dropped());
    APP_LOG_Printf("  cdc_tx_stalls    %lu\r\n",
                   (unsigned long)APP_LOG_TxStalls());
    dump_range(APP_DIAG_I2C_ERR, APP_DIAG_INA_MISSING, "I2C / INA226");
    dump_range(APP_DIAG_DMA_ERR, APP_DIAG_CACHE_CLEAN, "DMA / cache");
    dump_range(APP_DIAG_PWR_SAMPLES, APP_DIAG_TEMP_ALERT, "power / thermal");
  }
  else if (strcmp(sub, "health") == 0)
  {
    uint32_t err;

    err = s_c[APP_DIAG_PD_PROTOCOL_ERR] + s_c[APP_DIAG_PD_CRC_ERR] +
          s_c[APP_DIAG_PD_TIMEOUT] + s_c[APP_DIAG_PD_MALFORMED] +
          s_c[APP_DIAG_CDC_TX_FAIL] + s_c[APP_DIAG_DMA_ERR] +
          s_c[APP_DIAG_I2C_TIMEOUT];

    APP_LOG_Write("health\r\n");
    APP_LOG_Printf("  contracts      : %lu (EPR %lu)\r\n",
                   (unsigned long)s_c[APP_DIAG_NEG_CONTRACT],
                   (unsigned long)s_c[APP_DIAG_NEG_EPR]);
    APP_LOG_Printf("  rejects/waits  : %lu / %lu\r\n",
                   (unsigned long)s_c[APP_DIAG_NEG_REJECT],
                   (unsigned long)s_c[APP_DIAG_NEG_WAIT]);
    APP_LOG_Printf("  protocol errors: %lu\r\n",
                   (unsigned long)(s_c[APP_DIAG_PD_PROTOCOL_ERR] +
                                   s_c[APP_DIAG_PD_CRC_ERR]));
    APP_LOG_Printf("  timeouts       : %lu\r\n",
                   (unsigned long)s_c[APP_DIAG_PD_TIMEOUT]);
    APP_LOG_Printf("  total errors   : %lu\r\n", (unsigned long)err);

    APP_CAP_GetStats(&cap);
    APP_LOG_Printf("  capture        : %lu records, %lu dropped, %lu clipped\r\n",
                   (unsigned long)cap.total, (unsigned long)cap.dropped,
                   (unsigned long)cap.clipped);
    {
      int32_t mc = APP_TEMP_Get();

      APP_LOG_Printf("  die temp       : %s\r\n",
                     (mc < 0) ? "not sampled" :
                     ((mc >= APP_TEMP_CRIT_MC) ? "CRITICAL" :
                      ((mc >= APP_TEMP_WARN_MC) ? "warm" : "normal")));
    }
    APP_LOG_Printf("  cable identity : %s\r\n",
                   APP_CBL_IsLive() ? "discovered" : "none");
    APP_LOG_Printf("  verdict        : %s\r\n",
                   (err == 0u) ? "CLEAN" : "ERRORS PRESENT");
  }
  else if (strcmp(sub, "clear") == 0)
  {
    APP_DIAG_Clear();
    APP_LOG_Write("diagnostic counters cleared\r\n");
  }
  else
  {
    APP_LOG_Write("usage: diag [all|health|coherency|clear]\r\n");
  }
  return 1;
}
