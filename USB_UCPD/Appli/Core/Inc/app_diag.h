/**
 * @file    app_diag.h
 * @brief   Diagnostic counters and health snapshot for the whole instrument.
 *
 * Counters are bumped from whichever context observes the event, so every
 * increment is a single non-blocking integer operation.  Nothing here prints,
 * allocates or blocks; the CLI reads a snapshot on demand.
 */
#ifndef APP_DIAG_H
#define APP_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Counter identifiers.  Grouped so the CLI can print them in blocks. */
typedef enum
{
  /* PHY / PD protocol */
  APP_DIAG_PD_RX = 0,
  APP_DIAG_PD_TX,
  APP_DIAG_PD_GOODCRC_RX,
  APP_DIAG_PD_GOODCRC_TX,
  APP_DIAG_PD_RETRY,
  APP_DIAG_PD_SOFT_RESET,
  APP_DIAG_PD_HARD_RESET,
  APP_DIAG_PD_PROTOCOL_ERR,
  APP_DIAG_PD_CRC_ERR,
  APP_DIAG_PD_TIMEOUT,
  APP_DIAG_PD_UNMATCHED,
  APP_DIAG_PD_MALFORMED,

  /* negotiation */
  APP_DIAG_NEG_CAPS,
  APP_DIAG_NEG_REQUEST,
  APP_DIAG_NEG_ACCEPT,
  APP_DIAG_NEG_REJECT,
  APP_DIAG_NEG_WAIT,
  APP_DIAG_NEG_CONTRACT,
  APP_DIAG_NEG_EPR,

  /* attach */
  APP_DIAG_ATTACH,
  APP_DIAG_DETACH,
  APP_DIAG_CAD_EVENT,

  /* capture */
  APP_DIAG_CAP_RECORDS,
  APP_DIAG_CAP_DROPS,
  APP_DIAG_CAP_CLIPPED,

  /* USB CDC transport */
  APP_DIAG_CDC_TX,
  APP_DIAG_CDC_RX,
  APP_DIAG_CDC_TX_BUSY,
  APP_DIAG_CDC_TX_FAIL,
  APP_DIAG_CDC_OVERFLOW,
  APP_DIAG_CDC_BUS_RESET,
  APP_DIAG_CDC_SUSPEND,

  /* I2C / INA226 */
  APP_DIAG_I2C_ERR,
  APP_DIAG_I2C_TIMEOUT,
  APP_DIAG_INA_MISSING,

  /* DMA / cache coherency */
  APP_DIAG_DMA_ERR,
  APP_DIAG_DMA_TC,
  APP_DIAG_CACHE_INV,
  APP_DIAG_CACHE_CLEAN,

  /* power / thermal */
  APP_DIAG_PWR_SAMPLES,
  APP_DIAG_TEMP_SAMPLES,
  APP_DIAG_TEMP_ALERT,

  APP_DIAG_COUNT
} APP_DIAG_Id_t;

typedef struct
{
  uint32_t c[APP_DIAG_COUNT];
  uint32_t first_ts;
  uint32_t last_ts;
} APP_DIAG_Snapshot_t;

void APP_DIAG_Init(void);

/** Increment a counter by one.  Safe from task and ISR context. */
void APP_DIAG_Inc(APP_DIAG_Id_t id);
/** Increment a counter by an arbitrary amount. */
void APP_DIAG_Add(APP_DIAG_Id_t id, uint32_t n);

uint32_t APP_DIAG_Get(APP_DIAG_Id_t id);
void     APP_DIAG_GetAll(APP_DIAG_Snapshot_t *out);
void     APP_DIAG_Clear(void);

/** Human-readable counter name, for script-friendly output. */
const char *APP_DIAG_Name(APP_DIAG_Id_t id);

/** `diag` CLI command: counters, health snapshot, coherency checks. */
int APP_DIAG_Cmd(int argc, char *argv[]);

/**
 * Verify that the non-cacheable DMA window really is non-cacheable, by reading
 * the MPU region the application programmed.  Returns 1 when the buffers that
 * USB DMA touches fall inside a device/strongly-ordered or shareable
 * non-cacheable region, 0 otherwise.  Used by `diag coherency`.
 */
int APP_DIAG_CheckCoherency(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DIAG_H */
