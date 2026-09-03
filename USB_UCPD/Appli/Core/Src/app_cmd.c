/**
 * @file    app_cmd.c
 * @brief   CLI command registry for the analyzer feature layers.
 *
 * Adding a new engine to the console is a one-line change here: append an
 * entry to s_cmds.  Nothing in app_cli.c has to be edited, which keeps the
 * hardware-verified baseline console intact.
 */
#include "app_cmd.h"
#include "main.h"          /* SystemCoreClock */
#include "app_log.h"
#include "app_pdcap.h"
#include "app_cap.h"    /* APP_CAP_ElapsedUs */
#include "app_diag.h"
#include "app_epr.h"
#include "app_txn.h"
#include "app_pwr.h"
#include "app_cable.h"
#include "app_pd.h"
#include "ina226.h"
#include "app_temp.h"
#include "app_pps.h"
#include "app_test.h"
#include "app_store.h"
#include "app_integ.h"
#include "app_ext.h"
#include "app_fuzz.h"
#include "app_vdm.h"

extern APP_TXN_Port_t APP_TXN_Port0;   /* owned by app_pdcap.c */

static APP_PWR_Stat_t s_pwr;

#include <string.h>

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Engine status commands                                              */
/*                                                                     */
/* These live here, not in the engine files, because app_txn / app_pwr */
/* are stateless: app_cmd owns the single APP_PWR_Stat_t and reads the */
/* transaction port instance owned by the capture path.                */
/* ------------------------------------------------------------------ */

static int cmd_power(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "status";
  char line[96];

  if (strcmp(sub, "clear") == 0)
  {
    APP_PWR_Init(&s_pwr);
    APP_LOG_Write("power statistics cleared\r\n");
    return 1;
  }
  if (strcmp(sub, "status") != 0)
  {
    APP_LOG_Write("usage: power [status|clear]\r\n");
    return 1;
  }

  APP_PWR_Format(&s_pwr, line, sizeof(line));
  APP_LOG_Printf("power  %s\r\n", line);
  APP_LOG_Printf("  samples   : %lu over %lu us\r\n",
                 (unsigned long)s_pwr.n, (unsigned long)s_pwr.span_us);
  APP_LOG_Printf("  session   : %ld uWh, %ld uAh\r\n",
                 (long)s_pwr.uwh, (long)s_pwr.uah);
  APP_LOG_Printf("  contract  : %lu mV / %lu mA %s\r\n",
                 (unsigned long)s_pwr.contract_mv,
                 (unsigned long)s_pwr.contract_ma,
                 s_pwr.contract_epr ? "EPR" : "SPR");
  APP_LOG_Printf("  deviation : worst %lu mV from contract\r\n",
                 (unsigned long)s_pwr.worst_dev_mv);
  {
    const APP_PPS_Set_t *ps = APP_PPS_Get();

    if (ps->n != 0u)
    {
      APP_LOG_Printf("  pps span  : %lu-%lu mV, %lu mA max\r\n",
                     (unsigned long)ps->span_min_mv,
                     (unsigned long)ps->span_max_mv,
                     (unsigned long)ps->span_max_ma);
    }
  }
  return 1;
}


static int cmd_txn(int argc, char *argv[])
{
  const APP_TXN_Port_t *p = &APP_TXN_Port0;

  (void)argc;
  (void)argv;

  APP_LOG_Printf("transaction  port0  %s\r\n", APP_TXN_StateName(p->state));
  APP_LOG_Printf("  contracts      : %lu\r\n", (unsigned long)p->n_contracts);
  APP_LOG_Printf("  active         : %lu mV / %lu mA %s\r\n",
                 (unsigned long)p->contract_mv, (unsigned long)p->contract_ma,
                 p->contract_epr ? "EPR" : "SPR");
  APP_LOG_Printf("  caps/req/acc   : %lu / %lu / %lu\r\n",
                 (unsigned long)p->n_caps, (unsigned long)p->n_req,
                 (unsigned long)p->n_accept);
  APP_LOG_Printf("  reject/wait    : %lu / %lu\r\n",
                 (unsigned long)p->n_reject, (unsigned long)p->n_wait);
  APP_LOG_Printf("  goodcrc        : %lu\r\n", (unsigned long)p->n_goodcrc);
  APP_LOG_Printf("  soft/hard rst  : %lu / %lu\r\n",
                 (unsigned long)p->n_soft_reset, (unsigned long)p->n_hard_reset);
  APP_LOG_Printf("  retries/dups   : %lu / %lu\r\n",
                 (unsigned long)p->n_retries, (unsigned long)p->n_dups);
  APP_LOG_Printf("  accept latency : %lu us\r\n", (unsigned long)p->accept_us);
  APP_LOG_Printf("  ps_rdy latency : %lu us\r\n", (unsigned long)p->psrdy_us);
  APP_LOG_Printf("  unmatched/tmo  : %lu / %lu\r\n",
                 (unsigned long)p->n_unmatched, (unsigned long)p->n_timeouts);
  return 1;
}

static const APP_CMD_t s_cmds[] =
{
  /* name  usage                                              help */
#if APP_ENG_CAPTURE
  { "cap", "cap [stats|on|off|clear|list [n]|raw [n]]",
    "PD capture ring: stats, decoded listing, raw frames",
    APP_PDCAP_Cmd },
#endif

  { "diag", "diag [all|health|coherency|clear]",
    "counters, health verdict, DMA/cache coherency check",
    APP_DIAG_Cmd },

  { "epr", "epr [on|off|enter|exit|caps|request|diag|ceiling <mv>|want <mv>|status]",
    "EPR: detect/report always; on=auto-enter, enter=one manual attempt",
    APP_EPR_Cmd },

#if APP_ENG_ANALYTICS
    { "power", "power [status|clear]",
    "V/I/P statistics and session energy from the INA226",
    cmd_power },
#endif

  { "cable", "cable [status|vdo <hex>|discover]",
    "live cable E-marker identity, VDO decode, compatibility verdict",
    APP_CBL_LiveCmd },

  { "pps", "pps [status|check <mv> <ma>|rdo <pos> <mv> [ma]]",
    "PPS windows, operating-point validation, RDO construction",
    APP_PPS_Cmd },

  /* DTS temperature is a requested bench feature and does not belong to the
   * optional analytics engine.  APP_TEMP_Poll() (the statistics accumulator)
   * is what ANALYTICS gates; reading the sensor on demand is always available. */
  { "temp", "temp [status|clear]",
    "DTS temperature: current, min, max, average",
    APP_TEMP_Cmd },

  { "integ", "integ [status|crc [text]|sha256 [text]|session|rng]",
    "hardware CRC, SHA-256 fingerprints and RNG",
    APP_INTEG_Cmd },

#if APP_ENG_TEST
  { "test", "test [suite|replay [n]|all]",
    "deterministic protocol vectors and capture replay",
    APP_TEST_Cmd },
#endif

#if APP_ENG_STORE
  { "store", "store [status|save|load|erase|profiles|savep <n>|loadp <i>]",
    "explicit, wear-aware config and profile persistence",
    APP_STORE_Cmd },
#endif

#if APP_ENG_EXT
    { "ext", "ext",
    "chunked extended-message reassembly state and errors",
    APP_EXT_Cmd },
#endif

#if APP_ENG_FUZZ
  { "fuzz", "fuzz [run [n] [seed]|random [n]]",
    "malformed-message engine: headers, PDOs, extended, chunks, sequencing",
    APP_FUZZ_Cmd },
#endif

  { "vdm", "vdm status|discover|svids|modes|enter|exit",
    "VDM / alternate-mode control and cable re-discovery over the ST PE",
    APP_VDM_Cmd },

  { "txn", "txn",
    "reconstructed PD transaction and negotiation timing",
    cmd_txn },
};

#define APP_CMD_N  (sizeof(s_cmds) / sizeof(s_cmds[0]))

unsigned APP_CMD_Count(void)
{
  return (unsigned)APP_CMD_N;
}

int APP_CMD_Dispatch(int argc, char *argv[])
{
  unsigned i;

  if ((argc < 1) || (argv == NULL) || (argv[0] == NULL))
  {
    return 0;
  }

  for (i = 0u; i < APP_CMD_N; i++)
  {
    if (strcmp(argv[0], s_cmds[i].name) == 0)
    {
      return s_cmds[i].fn(argc, argv);
    }
  }
  return 0;
}

void APP_CMD_PrintHelp(void)
{
  unsigned i;

  APP_LOG_Write("\r\nanalyzer commands:\r\n");
  for (i = 0u; i < APP_CMD_N; i++)
  {
    APP_LOG_Printf("  %-34s %s\r\n", s_cmds[i].usage, s_cmds[i].help);
  }
}

/* ------------------------------------------------------------------ */
/* Periodic engine poll                                                */
/* ------------------------------------------------------------------ */

void APP_CMD_Poll(void)
{
  APP_TXN_Poll(&APP_TXN_Port0, APP_PDCAP_Cycles(), SystemCoreClock);
  APP_TEMP_Poll();

  if (INA226_IsPresent() != 0u)
  {
    static uint32_t last;
    uint32_t now = APP_PDCAP_Cycles();
    /* Three arguments: the cycle delta alone cannot become microseconds
     * without the clock rate.  This call previously passed two, which the
     * implicit-declaration warning hid until the header was included. */
    uint32_t dt_us = APP_CAP_ElapsedUs(last, now, SystemCoreClock);

    last = now;
    if (dt_us > 0u)
    {
      APP_PWR_Sample(&s_pwr, dt_us, (int32_t)INA226_GetBusMv(),
                     INA226_GetCurUa());
    }
  }
}
