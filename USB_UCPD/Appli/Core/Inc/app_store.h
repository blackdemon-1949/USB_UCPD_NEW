/**
 * @file    app_store.h
 * @brief   Configuration and profile persistence.
 *
 * Storage medium - and why it is not NOR
 * --------------------------------------
 * The application executes in place from the external NOR at 0x90000000 (see
 * STM32H7R3Z8JX_ROMxspi1.ld: FLASH ORIGIN = __FLASH_BEGIN = 0x90000000).  A
 * program or erase on that same device would stall the XIP fetch path, so a
 * NOR write from application context needs a RAM-resident stub that runs with
 * interrupts off and the XIP region quiesced.  Writing the device the code is
 * executing from is not something to do casually, so this layer uses the
 * 4 KiB backup SRAM the linker already maps at 0x38800000 instead:
 *
 *   - it survives reset, and power-off while VBAT is present,
 *   - it has no erase-before-write requirement, so no wear-levelling is
 *     needed to protect it,
 *   - it cannot corrupt the running firmware image.
 *
 * The wear-awareness requirement is still honoured: writes only happen on an
 * explicit `store save`, never implicitly, and a write budget is tracked and
 * reported so a runaway caller is visible.
 *
 * Layout is versioned and CRC-protected with the hardware CRC engine, so a
 * half-written or stale record is rejected on load rather than trusted.
 */
#ifndef APP_STORE_H
#define APP_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define APP_STORE_VERSION     1u
#define APP_STORE_MAX_PROFILES 4u
#define APP_STORE_NAME_LEN    16u

/** One saved operating point. */
typedef struct
{
  char     name[APP_STORE_NAME_LEN];
  uint32_t mv;
  uint32_t ma;
  uint8_t  epr;            /* 1 = allow EPR for this profile            */
  uint8_t  ceiling_mv_hi;  /* APP_STORE_EPR_CEILING_MV is 32-bit; kept  */
  uint32_t epr_ceiling_mv;
  uint8_t  used;
} APP_STORE_Profile_t;

/** Everything that is persisted. */
typedef struct
{
  uint32_t version;
  uint32_t auto_mv;
  uint32_t auto_ma;
  uint8_t  auto_epr;
  uint8_t  remember;
  uint32_t epr_ceiling_mv;
  uint32_t write_count;    /* wear counter */
  APP_STORE_Profile_t profiles[APP_STORE_MAX_PROFILES];
  uint32_t crc;            /* over the preceding bytes, hardware CRC */
} APP_STORE_Cfg_t;

typedef struct
{
  uint8_t  valid;          /* a good record was loaded                  */
  uint8_t  dirty;          /* RAM copy differs from what is stored      */
  uint32_t write_count;
  uint32_t crc;
  uint8_t  crc_ok;
} APP_STORE_Status_t;

void APP_STORE_Init(void);

/** Explicit persistence.  Never called implicitly. */
int  APP_STORE_Save(void);

/** Re-read the stored record, discarding the RAM copy. */
int  APP_STORE_Load(void);

/** Erase the stored record (explicit). */
int  APP_STORE_Erase(void);

APP_STORE_Cfg_t *APP_STORE_Get(void);
void APP_STORE_GetStatus(APP_STORE_Status_t *out);

/** Profile management.  @return the slot index, or 0xFF when full. */
uint8_t APP_STORE_ProfileSave(const char *name);
uint8_t APP_STORE_ProfileLoad(uint8_t slot);
uint8_t APP_STORE_ProfileList(void);

int APP_STORE_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_STORE_H */
