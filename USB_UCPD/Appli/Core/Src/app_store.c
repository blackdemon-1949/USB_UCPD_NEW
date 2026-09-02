/**
 * @file    app_store.c
 * @brief   Configuration and profile persistence (see app_store.h).
 */
#include "app_store.h"
#include "main.h"          /* HAL, HAL_PWR_EnableBkUpAccess */
#include "app_log.h"
#include "app_integ.h"
#include "app_pd.h"
#include "app_epr.h"

#include <string.h>
#include <stdio.h>

/** Backup SRAM base, matching BKPSRAM in STM32H7R3Z8JX_ROMxspi1.ld. */
#define APP_STORE_BKPSRAM  0x38800000uL
#define APP_STORE_SIZE     sizeof(APP_STORE_Cfg_t)

/** Refuse to write past this many times: a runaway caller must be visible
 *  rather than silently wearing the medium. */
#define APP_STORE_WRITE_BUDGET 100000uL

static APP_STORE_Cfg_t s_cfg;
static uint8_t  s_valid;
static uint8_t  s_dirty;
static uint8_t  s_bkup_ok;

static volatile APP_STORE_Cfg_t *bkp(void)
{
  return (volatile APP_STORE_Cfg_t *)APP_STORE_BKPSRAM;
}

static uint32_t cfg_crc(const APP_STORE_Cfg_t *c)
{
  /* CRC everything up to but not including the stored crc field. */
  return APP_CRC_Calc((const uint8_t *)c,
                      (uint32_t)((const uint8_t *)&c->crc - (const uint8_t *)c));
}

void APP_STORE_Init(void)
{
  HAL_PWR_EnableBkUpAccess();
  s_bkup_ok = 1u;

  memset(&s_cfg, 0, sizeof(s_cfg));
  s_cfg.version = APP_STORE_VERSION;
  s_cfg.auto_mv = 9000u;
  s_cfg.auto_ma = 3000u;
  s_cfg.auto_epr = 0u;
  s_cfg.remember = 0u;
  s_cfg.epr_ceiling_mv = APP_EPR_DEFAULT_CEILING_MV;
  s_cfg.write_count = 0u;
  s_valid = 0u;
  s_dirty = 1u;

  (void)APP_STORE_Load();
}

int APP_STORE_Load(void)
{
  volatile APP_STORE_Cfg_t *b = bkp();
  APP_STORE_Cfg_t tmp;
  uint32_t crc;

  if (s_bkup_ok == 0u)
  {
    return 0;
  }
  if (b->version != APP_STORE_VERSION)
  {
    s_valid = 0u;
    return 0;
  }

  memcpy(&tmp, (const void *)b, sizeof(tmp));
  crc = cfg_crc(&tmp);
  if (crc != tmp.crc)
  {
    APP_LOG_Write("store: CRC mismatch, record discarded\r\n");
    s_valid = 0u;
    return 0;
  }

  s_cfg = tmp;
  s_cfg.crc = crc;
  s_valid = 1u;
  s_dirty = 0u;

  /* Push the loaded settings into the live engines so a saved profile takes
   * effect immediately. */
  APP_PD_SetAuto(s_cfg.auto_mv, s_cfg.auto_ma);
  APP_PD_SetRemember(s_cfg.remember);
  APP_EPR_Ctx.enable = s_cfg.auto_epr;
  APP_EPR_Ctx.ceiling_mv = s_cfg.epr_ceiling_mv;
  return 1;
}

int APP_STORE_Save(void)
{
  volatile APP_STORE_Cfg_t *b = bkp();

  if (s_bkup_ok == 0u)
  {
    APP_LOG_Write("store: backup SRAM not accessible\r\n");
    return 0;
  }
  if (s_cfg.write_count >= APP_STORE_WRITE_BUDGET)
  {
    APP_LOG_Printf("store: write budget exhausted (%lu writes)\r\n",
                   (unsigned long)s_cfg.write_count);
    return 0;
  }

  /* Capture the live settings so a save means "save what is running now". */
  APP_PD_GetAuto(&s_cfg.auto_mv, &s_cfg.auto_ma);
  s_cfg.remember = APP_PD_GetRemember();
  s_cfg.auto_epr = APP_EPR_Ctx.enable;
  s_cfg.epr_ceiling_mv = APP_EPR_Ctx.ceiling_mv;

  s_cfg.version = APP_STORE_VERSION;
  s_cfg.write_count++;
  s_cfg.crc = cfg_crc(&s_cfg);

  memcpy((void *)b, &s_cfg, sizeof(s_cfg));

  if (b->crc != s_cfg.crc)
  {
    APP_LOG_Write("store: verify after write FAILED\r\n");
    s_valid = 0u;
    return 0;
  }

  s_valid = 1u;
  s_dirty = 0u;
  APP_LOG_Printf("store: saved (%lu bytes, write #%lu, crc 0x%08lX)\r\n",
                 (unsigned long)sizeof(s_cfg), (unsigned long)s_cfg.write_count,
                 (unsigned long)s_cfg.crc);
  return 1;
}

int APP_STORE_Erase(void)
{
  memset((void *)bkp(), 0, APP_STORE_SIZE);
  s_valid = 0u;
  s_dirty = 1u;
  APP_LOG_Write("store: erased\r\n");
  return 1;
}

APP_STORE_Cfg_t *APP_STORE_Get(void)
{
  return &s_cfg;
}

void APP_STORE_GetStatus(APP_STORE_Status_t *out)
{
  if (out == NULL)
  {
    return;
  }
  out->valid = s_valid;
  out->dirty = s_dirty;
  out->write_count = s_cfg.write_count;
  out->crc = s_cfg.crc;
  out->crc_ok = (cfg_crc(&s_cfg) == s_cfg.crc) ? 1u : 0u;
}

uint8_t APP_STORE_ProfileSave(const char *name)
{
  uint8_t slot = 0xFFu;
  uint8_t i;

  for (i = 0u; i < APP_STORE_MAX_PROFILES; i++)
  {
    if ((s_cfg.profiles[i].used != 0u) && (name != NULL) &&
        (strcmp(s_cfg.profiles[i].name, name) == 0))
    {
      slot = i;                    /* overwrite an existing profile of this name */
      break;
    }
  }
  if (slot == 0xFFu)
  {
    for (i = 0u; i < APP_STORE_MAX_PROFILES; i++)
    {
      if (s_cfg.profiles[i].used == 0u)
      {
        slot = i;
        break;
      }
    }
  }
  if (slot == 0xFFu)
  {
    return 0xFFu;
  }

  memset(&s_cfg.profiles[slot], 0, sizeof(APP_STORE_Profile_t));
  if (name != NULL)
  {
    (void)strncpy(s_cfg.profiles[slot].name, name, APP_STORE_NAME_LEN - 1u);
  }
  else
  {
    (void)snprintf(s_cfg.profiles[slot].name, APP_STORE_NAME_LEN, "p%u",
                   (unsigned)slot);
  }
  APP_PD_GetAuto(&s_cfg.profiles[slot].mv, &s_cfg.profiles[slot].ma);
  s_cfg.profiles[slot].epr = APP_EPR_Ctx.enable;
  s_cfg.profiles[slot].epr_ceiling_mv = APP_EPR_Ctx.ceiling_mv;
  s_cfg.profiles[slot].used = 1u;
  s_dirty = 1u;
  return slot;
}

uint8_t APP_STORE_ProfileLoad(uint8_t slot)
{
  const APP_STORE_Profile_t *p;

  if (slot >= APP_STORE_MAX_PROFILES)
  {
    return 0u;
  }
  p = &s_cfg.profiles[slot];
  if (p->used == 0u)
  {
    return 0u;
  }

  APP_PD_SetAuto(p->mv, p->ma);
  APP_EPR_Ctx.enable = p->epr;
  APP_EPR_Ctx.ceiling_mv = p->epr_ceiling_mv;
  s_dirty = 1u;
  return 1u;
}

uint8_t APP_STORE_ProfileList(void)
{
  uint8_t i;
  uint8_t n = 0u;

  for (i = 0u; i < APP_STORE_MAX_PROFILES; i++)
  {
    if (s_cfg.profiles[i].used != 0u)
    {
      APP_LOG_Printf("  [%u] %-16s %lu mV / %lu mA  %s  ceiling %lu mV\r\n",
                     (unsigned)i, s_cfg.profiles[i].name,
                     (unsigned long)s_cfg.profiles[i].mv,
                     (unsigned long)s_cfg.profiles[i].ma,
                     s_cfg.profiles[i].epr ? "EPR" : "SPR",
                     (unsigned long)s_cfg.profiles[i].epr_ceiling_mv);
      n++;
    }
  }
  if (n == 0u)
  {
    APP_LOG_Write("  (no profiles saved)\r\n");
  }
  return n;
}

int APP_STORE_Cmd(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "status";
  APP_STORE_Status_t st;

  if (strcmp(sub, "save") == 0)
  {
    return (APP_STORE_Save() != 0) ? 1 : 1;
  }
  if (strcmp(sub, "load") == 0)
  {
    (void)APP_STORE_Load();
    APP_LOG_Printf("store: %s\r\n", s_valid ? "loaded" : "no valid record");
    return 1;
  }
  if (strcmp(sub, "erase") == 0)
  {
    (void)APP_STORE_Erase();
    return 1;
  }
  if (strcmp(sub, "profiles") == 0)
  {
    APP_LOG_Write("profiles\r\n");
    (void)APP_STORE_ProfileList();
    return 1;
  }
  if ((strcmp(sub, "savep") == 0) && (argc >= 3))
  {
    uint8_t slot = APP_STORE_ProfileSave(argv[2]);

    if (slot == 0xFFu)
    {
      APP_LOG_Write("profile slots full\r\n");
    }
    else
    {
      APP_LOG_Printf("profile '%s' saved to slot %u (run 'store save' to persist)\r\n",
                     argv[2], (unsigned)slot);
    }
    return 1;
  }
  if ((strcmp(sub, "loadp") == 0) && (argc >= 3))
  {
    unsigned slot = 0u;

    if (sscanf(argv[2], "%u", &slot) != 1)
    {
      APP_LOG_Write("usage: store loadp <slot>\r\n");
      return 1;
    }
    if (APP_STORE_ProfileLoad((uint8_t)slot) == 0u)
    {
      APP_LOG_Write("no such profile\r\n");
    }
    else
    {
      APP_LOG_Printf("profile slot %u applied\r\n", slot);
    }
    return 1;
  }
  if (strcmp(sub, "status") != 0)
  {
    APP_LOG_Write("usage: store [status|save|load|erase|profiles|savep <name>|loadp <slot>]\r\n");
    return 1;
  }

  APP_STORE_GetStatus(&st);
  APP_LOG_Write("store (backup SRAM 0x38800000 - NOT NOR flash)\r\n");
  APP_LOG_Write("  NOR persistence: DISABLED (application XIPs from that\r\n");
  APP_LOG_Write("  same NOR; a safe RAM-resident programming stub is not\r\n");
  APP_LOG_Write("  implemented, so no NOR writes are performed at all)\r\n");
  APP_LOG_Printf("  record      : %s\r\n", st.valid ? "valid" : "empty/invalid");
  APP_LOG_Printf("  version     : %lu\r\n", (unsigned long)s_cfg.version);
  APP_LOG_Printf("  crc         : 0x%08lX (%s)\r\n", (unsigned long)st.crc,
                 st.crc_ok ? "ok" : "MISMATCH");
  APP_LOG_Printf("  dirty       : %s\r\n", st.dirty ? "yes - not saved" : "no");
  APP_LOG_Printf("  writes      : %lu / %lu budget\r\n",
                 (unsigned long)st.write_count,
                 (unsigned long)APP_STORE_WRITE_BUDGET);
  APP_LOG_Printf("  auto        : %lu mV / %lu mA, EPR %s, remember %s\r\n",
                 (unsigned long)s_cfg.auto_mv, (unsigned long)s_cfg.auto_ma,
                 s_cfg.auto_epr ? "on" : "off",
                 s_cfg.remember ? "on" : "off");
  APP_LOG_Printf("  epr ceiling : %lu mV\r\n",
                 (unsigned long)s_cfg.epr_ceiling_mv);
  APP_LOG_Write("profiles\r\n");
  (void)APP_STORE_ProfileList();
  return 1;
}
