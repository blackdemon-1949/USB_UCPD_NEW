#include "app_cli.h"
#include "app_log.h"
#include "app_pd.h"
#include "app_board.h"
#include "app_cmd.h"
#include "usbpd_dpm_user.h"
#include "usbpd_core.h"
#include "ina226.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define CLI_LINE_MAX  96U
#define CLI_RX_MAX    256U

static char     s_line[CLI_LINE_MAX];
static uint16_t s_len;
static uint8_t  s_rx[CLI_RX_MAX];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static uint8_t  s_greeted;

void APP_CLI_Init(void)
{
  s_len = 0;
  s_rx_head = 0;
  s_rx_tail = 0;
  s_greeted = 0;
}

void APP_CLI_OnRx(const uint8_t *data, uint32_t len)
{
  uint32_t i;
  for (i = 0; i < len; i++)
  {
    uint16_t next = (uint16_t)((s_rx_head + 1U) % CLI_RX_MAX);
    if (next == s_rx_tail)
    {
      break;
    }
    s_rx[s_rx_head] = data[i];
    s_rx_head = next;
  }
}

void APP_CLI_PrintHelp(void)
{
  APP_LOG_Write(
    "Commands (type one, then Enter):\r\n"
    "\r\n"
    "  -- ask the source for power --\r\n"
    "  caps                   list the power levels the source offers\r\n"
    "  req <n> [ma]           ask for fixed level n (1 = first) at this current\r\n"
    "  volt <mv> [ma]         ask for the closest level (fixed or PPS) to this voltage\r\n"
    "  pps <mv> [ma]          ask for an exact PPS voltage (20 mV steps) and current\r\n"
    "\r\n"
    "  -- automations --\r\n"
    "  auto <mv> [ma]         automatically ask for this voltage after attach\r\n"
    "  auto off               go back to the default 5 V on attach\r\n"
    "  remember on|off        re-apply your last request after the next attach\r\n"
    "  sweep <from> <to> <step> [ma]   step the PPS voltage from..to (mV)\r\n"
    "  sweep stop             stop a running sweep\r\n"
    "\r\n"
    "  -- output monitor (INA226 on I2C2) --\r\n"
    "  ina                    one-shot output V / I / P reading\r\n"
    "  ina auto on|off        periodic reading every second (default on)\r\n"
    "  ina period <ms>        change the periodic reading interval\r\n"
    "  ina addr <hex>         use a different INA226 address (default 40)\r\n"
    "  ina scan               scan the I2C2 bus (0x08-0x77)\r\n"
    "  ina vbus real|synth    feed the PD stack real INA226 vbus\r\n"
    "\r\n"
    "  -- read details from the source --\r\n"
    "  status                 what is connected and what we asked for\r\n"
    "  getstatus              read the source's status (temperature, faults)\r\n"
    "  getpps                 read the source's real output voltage and current\r\n"
    "  srcext                 read extended info (VID/PID, firmware, max power)\r\n"
    "  manuinfo               read the manufacturer name and IDs\r\n"
    "  battery                read battery capability and status (if it has one)\r\n"
    "  countrycodes           list the country codes the source supports\r\n"
    "  countryinfo <XX>       read country info (XX = two letters, e.g. US)\r\n"
    "  identify               ask the source to identify itself (VDM)\r\n"
    "  svids                  list alternate-mode SVIDs the source supports\r\n"
    "  modes <svid>           list the modes of an SVID (e.g. modes ff01)\r\n"
    "\r\n"
    "  -- control / tools --\r\n"
    "  getcaps                re-read the source's power levels\r\n"
    "  softreset / hardreset  PD soft / hard reset\r\n"
    "  pd                     UCPD registers + PHY counters\r\n"
    "  info                   board / memory / clocks\r\n"
    "  led on|off|hb          LED override\r\n"
    "  help                   this list\r\n"
    "\r\n"
    "wire a PD source to PM0 (CC1) or PM1 (CC2) plus GND. The USB Type-C port is this\r\n"
    "console; the USB-PD trace for STM32CubeMonitor-UCPD is on USART1 PA9/PA10 @921600.\r\n"
  );
  /* Commands contributed by the analyzer feature layers (see app_cmd.c). */
  APP_CMD_PrintHelp();
}

void APP_CLI_PrintBanner(void)
{
  APP_LOG_Write(
    "\r\n"
    "========================================\r\n"
    "  PD Bench  —  STM32H7R3 UCPD sink\r\n"
    "  WeAct H7R3Z8  |  XiP PY25Q64HA\r\n"
    "========================================\r\n"
    "USB CDC up. Type help\r\n"
    "\r\n"
  );
  APP_CLI_PrintHelp();
  if (APP_PD_IsAttached())
  {
    APP_PD_PrintCaps();
  }
  else
  {
    APP_LOG_Write("waiting for PD source on CC1/CC2...\r\n");
  }
  INA226_PrintStatus();
  APP_LOG_Write("> ");
}

void APP_CLI_OnHostOpen(void)
{
  s_greeted = 0;
}

static void prompt(void)
{
  APP_LOG_Write("> ");
}

static int parse_u(const char *s, unsigned *out)
{
  char *end = NULL;
  unsigned long v;
  if ((s == NULL) || (*s == '\0'))
  {
    return -1;
  }
  v = strtoul(s, &end, 0);
  if ((end == s) || (end == NULL))
  {
    return -1;
  }
  *out = (unsigned)v;
  return 0;
}

static int parse_h(const char *s, unsigned *out)
{
  char *end = NULL;
  unsigned long v;
  if ((s == NULL) || (*s == '\0'))
  {
    return -1;
  }
  v = strtoul(s, &end, 16);
  if ((end == s) || (end == NULL))
  {
    return -1;
  }
  *out = (unsigned)v;
  return 0;
}


static void handle_line(char *line)
{
  char *argv[6];
  int argc = 0;
  char *p = line;

  while (*p && (argc < 6))
  {
    while (*p && isspace((unsigned char)*p)) { p++; }
    if (*p == '\0') { break; }
    argv[argc++] = p;
    while (*p && !isspace((unsigned char)*p)) { p++; }
    if (*p) { *p++ = '\0'; }
  }
  if (argc == 0)
  {
    prompt();
    return;
  }

  if ((strcmp(argv[0], "help") == 0) || (strcmp(argv[0], "?") == 0))
  {
    APP_CLI_PrintHelp();
  }
  else if (strcmp(argv[0], "status") == 0)
  {
    APP_PD_PrintStatus();
    INA226_PrintStatus();
  }
  else if ((strcmp(argv[0], "ina") == 0) || (strcmp(argv[0], "ina226") == 0))
  {
    INA226_Cli(argc, argv);
  }
  else if (strcmp(argv[0], "info") == 0)
  {
    APP_BOARD_PrintInfo();
  }
  else if (strcmp(argv[0], "pd") == 0)
  {
    APP_BOARD_PrintUcpd();
  }
  else if (strcmp(argv[0], "caps") == 0)
  {
    APP_PD_PrintCaps();
  }
  else if (strcmp(argv[0], "getcaps") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetSourceCapability(0);
    APP_LOG_Printf("Asked the source for its power levels (%s).\r\n",
                   (st == USBPD_OK) ? "sent" : "the stack refused");
    if (st == USBPD_OK)
    {
      APP_PD_RequestCapsPrint();   /* print the reply even if it did not change */
    }
  }
  else if (strcmp(argv[0], "req") == 0)
  {
    unsigned n = 0;
    unsigned ma = 0;
    if ((argc < 2) || (parse_u(argv[1], &n) != 0) || (n == 0U) || (n > 7U))
    {
      APP_LOG_Write("usage: req <1-7> [current in mA]\r\n");
    }
    else
    {
      if (argc >= 3) { (void)parse_u(argv[2], &ma); }
      (void)APP_PD_SendRequest(0, (uint8_t)n, 0, (uint16_t)ma);
    }
  }
  else if (strcmp(argv[0], "volt") == 0)
  {
    unsigned mv = 0;
    unsigned ma = 0;
    uint8_t idx = 0;
    uint8_t is_pps = 0;
    if ((argc < 2) || (parse_u(argv[1], &mv) != 0))
    {
      APP_LOG_Write("usage: volt <mv> [current in mA]\r\n");
    }
    else
    {
      if (argc >= 3) { (void)parse_u(argv[2], &ma); }
      if (APP_PD_FindBestPdo(mv, &idx, &is_pps) == 0U)
      {
        APP_LOG_Write("No power level covers that voltage (type 'caps' to see the list).\r\n");
      }
      else
      {
        APP_LOG_Printf("Asking for %u mV%s...\r\n", (unsigned)mv,
                       (is_pps != 0U) ? " via PPS" : "");
        (void)APP_PD_SendRequest(0, idx, (uint16_t)mv, (uint16_t)ma);
      }
    }
  }
  else if (strcmp(argv[0], "pps") == 0)
  {
    unsigned mv = 0;
    unsigned ma = 0;
    uint8_t idx = 0;
    uint8_t is_pps = 0;
    if ((argc < 2) || (parse_u(argv[1], &mv) != 0))
    {
      APP_LOG_Write("usage: pps <mv> [current in mA]\r\n");
    }
    else
    {
      if (argc >= 3) { (void)parse_u(argv[2], &ma); }
      if ((APP_PD_FindBestPdo(mv, &idx, &is_pps) == 0U) || (is_pps == 0U))
      {
        APP_LOG_Write("The source has no PPS range covering that voltage.\r\n");
      }
      else
      {
        (void)APP_PD_SendRequest(0, idx, (uint16_t)mv, (uint16_t)ma);
      }
    }
  }
  else if (strcmp(argv[0], "auto") == 0)
  {
    if ((argc >= 2) && (strcmp(argv[1], "off") == 0))
    {
      APP_PD_SetAuto(0U, 0U);
      APP_LOG_Write("Auto request is now off. The default 5 V will be used after attach.\r\n");
    }
    else
    {
      unsigned mv = 0;
      unsigned ma = 0;
      if ((argc < 2) || (parse_u(argv[1], &mv) != 0) || (mv == 0U))
      {
        APP_LOG_Write("usage: auto <mv> [ma]   or   auto off\r\n");
      }
      else
      {
        if (argc >= 3) { (void)parse_u(argv[2], &ma); }
        APP_PD_SetAuto(mv, ma);
        APP_LOG_Printf("Auto request set to %u mV%s. It will be applied after the next attach.\r\n",
                       (unsigned)mv, (ma != 0U) ? " (current limited)" : "");
      }
    }
  }
  else if (strcmp(argv[0], "remember") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Printf("Remember mode is %s.\r\n", APP_PD_GetRemember() ? "on" : "off");
    }
    else if (strcmp(argv[1], "on") == 0)
    {
      APP_PD_SetRemember(1U);
      APP_LOG_Write("Remember mode is on: your last request will be re-applied after re-connect.\r\n");
    }
    else
    {
      APP_PD_SetRemember(0U);
      APP_LOG_Write("Remember mode is off.\r\n");
    }
  }
  else if (strcmp(argv[0], "sweep") == 0)
  {
    if ((argc >= 2) && (strcmp(argv[1], "stop") == 0))
    {
      APP_PD_StopSweep();
      APP_LOG_Write("Sweep stopped.\r\n");
    }
    else
    {
      unsigned from = 0, to = 0, step = 0, ma = 0;
      if ((argc < 4) || (parse_u(argv[1], &from) != 0) ||
          (parse_u(argv[2], &to) != 0) || (parse_u(argv[3], &step) != 0) ||
          (from == 0U) || (to < from) || (step == 0U))
      {
        APP_LOG_Write("usage: sweep <from_mV> <to_mV> <step_mV> [current mA]   or   sweep stop\r\n");
        APP_LOG_Write("example: sweep 5000 20000 500 3000\r\n");
      }
      else
      {
        if (argc >= 5) { (void)parse_u(argv[4], &ma); }
        if (APP_PD_IsAttached() == 0U)
        {
          APP_LOG_Write("Connect a PD source first (PM0/PM1 + GND), then start the sweep.\r\n");
        }
        else
        {
          APP_PD_StartSweep(from, to, step, ma);
          APP_LOG_Printf("Starting a PPS sweep from %u mV to %u mV in %u mV steps.\r\n",
                         (unsigned)from, (unsigned)to, (unsigned)step);
        }
      }
    }
  }
  else if (strcmp(argv[0], "getstatus") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetStatus(0);
    APP_LOG_Printf("Asked the source for its status (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused (is there a contract?)");
  }
  else if (strcmp(argv[0], "getpps") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetPPS_Status(0);
    APP_LOG_Printf("Asked the source for its real output voltage/current (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused (need a PPS contract?)");
  }
  else if (strcmp(argv[0], "srcext") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetSourceCapabilityExt(0);
    APP_LOG_Printf("Asked for extended source info (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "manuinfo") == 0)
  {
    USBPD_GMIDB_TypeDef req;
    req.ManufacturerInfoTarget = USBPD_MANUFINFO_TARGET_PORT_CABLE_PLUG;
    req.ManufacturerInfoRef = 0U;
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetManufacturerInfo(0, USBPD_SOPTYPE_SOP, (uint8_t *)&req);
    APP_LOG_Printf("Asked for the manufacturer information (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "battery") == 0)
  {
    uint8_t ref = 0U;
    USBPD_StatusTypeDef st1 = USBPD_DPM_RequestGetBatteryCapability(0, &ref);
    USBPD_StatusTypeDef st2 = USBPD_DPM_RequestGetBatteryStatus(0, &ref);
    APP_LOG_Printf("Asked for battery capability and status (%s / %s).\r\n",
                   (st1 == USBPD_OK) ? "capability sent" : "capability refused",
                   (st2 == USBPD_OK) ? "status sent" : "status refused");
  }
  else if (strcmp(argv[0], "countrycodes") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetCountryCodes(0);
    APP_LOG_Printf("Asked for the country codes (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "countryinfo") == 0)
  {
    if ((argc < 2) || (argv[1][0] == '\0') || (argv[1][1] == '\0'))
    {
      APP_LOG_Write("usage: countryinfo <XX>   (two letters, e.g. US)\r\n");
    }
    else
    {
      uint16_t cc = (uint16_t)(((uint16_t)(uint8_t)argv[1][0] << 8U) | (uint16_t)(uint8_t)argv[1][1]);
      USBPD_StatusTypeDef st = USBPD_DPM_RequestGetCountryInfo(0, cc);
      APP_LOG_Printf("Asked for country info for %c%c (%s).\r\n",
                     argv[1][0], argv[1][1],
                     (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
    }
  }
  else if (strcmp(argv[0], "identify") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestVDM_DiscoveryIdentify(0, USBPD_SOPTYPE_SOP);
    APP_LOG_Printf("Asked the source to identify itself (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
    APP_LOG_Write("(Some sources NAK this from a sink; use 'srcext'/'manuinfo' for a more reliable answer.)\r\n");
  }
  else if (strcmp(argv[0], "svids") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestVDM_DiscoverySVID(0, USBPD_SOPTYPE_SOP);
    APP_LOG_Printf("Asked for the alternate-mode SVID list (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "modes") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Write("usage: modes <svid>   (e.g. modes ff01)\r\n");
    }
    else
    {
      unsigned svid = 0;
      if (parse_h(argv[1], &svid) != 0)
      {
        APP_LOG_Write("usage: modes <svid>   (e.g. modes ff01)\r\n");
      }
      else
      {
        USBPD_StatusTypeDef st = USBPD_DPM_RequestVDM_DiscoveryMode(0, USBPD_SOPTYPE_SOP, (uint16_t)svid);
        APP_LOG_Printf("Asked for the modes of SVID 0x%04X (%s).\r\n",
                       (unsigned)svid, (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
      }
    }
  }
  else if (strcmp(argv[0], "hardreset") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestHardReset(0);
    APP_LOG_Printf("HARD_RESET %s\r\n", (st == USBPD_OK) ? "sent" : "failed");
  }
  else if (strcmp(argv[0], "softreset") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestSoftReset(0, USBPD_SOPTYPE_SOP);
    APP_LOG_Printf("SOFT_RESET %s\r\n", (st == USBPD_OK) ? "sent" : "failed");
  }
  else if (strcmp(argv[0], "led") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Write("usage: led on|off|hb\r\n");
    }
    else if (strcmp(argv[1], "on") == 0) { APP_LED_Set(APP_LED_ON); }
    else if (strcmp(argv[1], "off") == 0) { APP_LED_Set(APP_LED_OFF); }
    else { APP_LED_Set(APP_LED_HEARTBEAT); }
  }
  else if (APP_CMD_Dispatch(argc, argv) == 0)
  {
    APP_LOG_Printf("unknown: %s  (help)\r\n", argv[0]);
  }
  prompt();
}

void APP_CLI_Poll(void)
{
  if ((s_greeted == 0U) && APP_LOG_UsbReady())
  {
    s_greeted = 1U;
    APP_CLI_PrintBanner();
  }

  while (s_rx_tail != s_rx_head)
  {
    uint8_t c = s_rx[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % CLI_RX_MAX);

    if ((c == '\r') || (c == '\n'))
    {
      if (s_len > 0U)
      {
        s_line[s_len] = '\0';
        APP_LOG_Write("\r\n");
        handle_line(s_line);
        s_len = 0;
      }
      else if (c == '\r')
      {
        APP_LOG_Write("\r\n");
        prompt();
      }
      continue;
    }
    if ((c == 0x08U) || (c == 0x7FU))
    {
      if (s_len > 0U)
      {
        s_len--;
        APP_LOG_Write("\b \b");
      }
      continue;
    }
    if ((c >= 32U) && (c < 127U) && (s_len < (CLI_LINE_MAX - 1U)))
    {
      s_line[s_len++] = (char)c;
      APP_LOG_WriteRaw(&c, 1);
    }
  }
}
