/**
 * @file    app_pdcap.c
 * @brief   ST trace funnel -> capture ring bridge, and the `cap` CLI command.
 */
#include "app_pdcap.h"
#include "main.h"          /* CMSIS core (DWT, CoreDebug) and SystemCoreClock */
#include "app_cap.h"
#include "app_diag.h"
#include "app_txn.h"
#include "app_ext.h"

/* Transaction reconstruction state.  app_txn is stateless on purpose so that
 * a host test can drive its own instance; on target there is exactly one
 * port, owned here because the capture path is what feeds it. */
APP_TXN_Port_t APP_TXN_Port0;
#include "app_dec.h"
#include "app_log.h"
#include "usbpd_trace.h"

#include <string.h>
#include <stdio.h>

#ifndef APP_PDCAP_LIST_MAX
#define APP_PDCAP_LIST_MAX  64u     /* default records shown by `cap list` */
#endif

/* ------------------------------------------------------------------ */
/* Timestamps                                                          */
/* ------------------------------------------------------------------ */

uint32_t APP_PDCAP_CoreHz(void)
{
  return SystemCoreClock;
}

uint32_t APP_PDCAP_Cycles(void)
{
  return DWT->CYCCNT;
}

uint32_t APP_PDCAP_CyclesToUs(uint32_t cycles)
{
  return APP_CAP_ElapsedUs(0u, cycles, SystemCoreClock);
}

/* ------------------------------------------------------------------ */
/* Trace entry point                                                   */
/* ------------------------------------------------------------------ */

void APP_PDCAP_Trace(TRACE_EVENT type, uint8_t port, uint8_t sop,
                     uint8_t *ptr, uint32_t size)
{
  /* Stage only: a timestamp, a memcpy into the ring and a few counters.
   * No formatting, no CDC, no I2C and no blocking here. */
  APP_CAP_Record((uint8_t)type, port, sop, APP_PDCAP_Cycles(), ptr, size);

  /* Counter bump only - no formatting, no blocking, no allocation. */
  APP_DIAG_Inc(APP_DIAG_CAP_RECORDS);

  /* PD messages feed the transaction reconstruction.  APP_TXN_Feed is pure
   * integer arithmetic over the bytes already in hand: no formatting, no
   * blocking, no allocation. */
  if (type == USBPD_TRACE_MESSAGE_IN)
  {
    APP_DIAG_Inc(APP_DIAG_PD_RX);
    APP_TXN_Feed(&APP_TXN_Port0, 0u, sop, APP_PDCAP_Cycles(), ptr, size);
    {
      APP_DEC_Msg_t dec;

      /* Reassemble chunked extended messages.  APP_DEC_Decode points into the
       * caller's buffer, so this adds no copy and no allocation. */
      if (APP_DEC_Decode(ptr, (uint16_t)size, &dec) == 0)
      {
        if (dec.msg_class == APP_DEC_CLASS_EXTENDED)
        {
          (void)APP_EXT_LiveFeed(&dec);
        }
      }
    }
  }
  else if (type == USBPD_TRACE_MESSAGE_OUT)
  {
    APP_DIAG_Inc(APP_DIAG_PD_TX);
    APP_TXN_Feed(&APP_TXN_Port0, 1u, sop, APP_PDCAP_Cycles(), ptr, size);
  }

  /* hand the event straight back to the stock TRACER_EMB path */
  USBPD_TRACE_Add(type, port, sop, ptr, size);
}

void APP_PDCAP_Init(void)
{
  /* Free-running cycle counter: ~7 s per 32-bit wrap at 600 MHz, which is
   * far longer than any interval a PD transaction contains.  ElapsedUs()
   * uses unsigned subtraction, so deltas across the wrap stay correct. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  APP_CAP_Init();

  /* Take over the trace funnel registered by USBPD_TRACE_Init(). */
  USBPD_PE_SetTrace(APP_PDCAP_Trace, 3u);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void cap_stats(void)
{
  APP_CAP_Stats_t st;
  uint32_t span_us;

  APP_CAP_GetStats(&st);
  span_us = APP_PDCAP_CyclesToUs(st.newest_ts - st.oldest_ts);

  APP_LOG_Write("capture ring\r\n");
  APP_LOG_Printf("  state      : %s\r\n", st.enabled ? "on" : "off");
  APP_LOG_Printf("  records    : %lu / %lu (capacity %lu)%s\r\n",
                 (unsigned long)st.count, (unsigned long)st.total,
                 (unsigned long)st.capacity, st.wrapped ? " wrapped" : "");
  APP_LOG_Printf("  dropped    : %lu\r\n", (unsigned long)st.dropped);
  APP_LOG_Printf("  clipped    : %lu\r\n", (unsigned long)st.clipped);
  APP_LOG_Printf("  rx / tx    : %lu / %lu\r\n",
                 (unsigned long)st.msg_in, (unsigned long)st.msg_out);
  APP_LOG_Printf("  other      : %lu\r\n", (unsigned long)st.other);
  APP_LOG_Printf("  span       : %lu.%03lu ms @ %lu Hz\r\n",
                 (unsigned long)(span_us / 1000u),
                 (unsigned long)(span_us % 1000u),
                 (unsigned long)SystemCoreClock);
  APP_LOG_Printf("  payload    : %u B/record, %u B ring\r\n",
                 (unsigned)APP_CAP_PAYLOAD,
                 (unsigned)(APP_CAP_RING_RECORDS * sizeof(APP_CAP_Rec_t)));
}

static const char *type_name(uint8_t t)
{
  switch (t)
  {
    case APP_CAP_T_MSG_IN:   return "RX";
    case APP_CAP_T_MSG_OUT:  return "TX";
    case APP_CAP_T_CAD_EVENT: return "CAD";
    case APP_CAP_T_PE_STATE: return "PE";
    case APP_CAP_T_NOTIF:    return "NTF";
    default:                 return "--";
  }
}

static void cap_list(uint32_t max)
{
  uint32_t count = APP_CAP_Count();
  uint32_t first = 0u;
  uint32_t i;
  char line[192];

  if (count == 0u)
  {
    APP_LOG_Write("capture ring is empty (cap on, then attach a source)\r\n");
    return;
  }
  if (max > count)
  {
    first = count - max;
  }

  APP_LOG_Printf("%-4s %-6s %-4s %-4s %-4s %s\r\n",
                 "idx", "t(us)", "dir", "sop", "id", "message");
  for (i = first; i < count; i++)
  {
    APP_CAP_Rec_t r;
    uint32_t rel_us;
    if (APP_CAP_Get(i, &r) != 0)
    {
      break;
    }
    rel_us = APP_PDCAP_CyclesToUs(r.ts);

    if ((r.type == APP_CAP_T_MSG_IN) || (r.type == APP_CAP_T_MSG_OUT))
    {
      APP_DEC_FormatFrame(r.data, r.len, line, sizeof(line));
      APP_LOG_Printf("%-4lu %-6lu %-4s %-4s     %s%s\r\n",
                     (unsigned long)i, (unsigned long)rel_us,
                     type_name(r.type), APP_DEC_SopName(r.sop), line,
                     (r.flags & APP_CAP_F_CLIPPED) ? " [clipped]" : "");
    }
    else
    {
      APP_LOG_Printf("%-4lu %-6lu %-4s %-4s     type %u len %u\r\n",
                     (unsigned long)i, (unsigned long)rel_us,
                     type_name(r.type), APP_DEC_SopName(r.sop),
                     (unsigned)r.type, (unsigned)r.len);
    }
  }
}

static void cap_raw(uint32_t max)
{
  uint32_t count = APP_CAP_Count();
  uint32_t first = 0u;
  uint32_t i;
  char line[128];
  size_t n;

  if (count == 0u)
  {
    APP_LOG_Write("capture ring is empty\r\n");
    return;
  }
  if (max > count)
  {
    first = count - max;
  }

  /* One line per record: <index>,<cycles>,<dir>,<sop>,<len>,<hex...>
   * Deliberately fixed-format so tools/ can parse it without a library. */
  for (i = first; i < count; i++)
  {
    APP_CAP_Rec_t r;
    uint32_t b;
    if (APP_CAP_Get(i, &r) != 0)
    {
      break;
    }
    n = (size_t)snprintf(line, sizeof(line), "%lu,%lu,%u,%u,%u,",
                         (unsigned long)i, (unsigned long)r.ts,
                         (unsigned)r.type, (unsigned)r.sop, (unsigned)r.len);
    for (b = 0u; (b < r.len) && ((n + 3u) < sizeof(line)); b++)
    {
      n += (size_t)snprintf(&line[n], sizeof(line) - n, "%02X", r.data[b]);
    }
    APP_LOG_Write(line);
    APP_LOG_Write("\r\n");
  }
}

int APP_PDCAP_Cmd(int argc, char *argv[])
{
  uint32_t max = APP_PDCAP_LIST_MAX;

  if (argc >= 3)
  {
    unsigned v;
    if (sscanf(argv[2], "%u", &v) != 1)
    {
      APP_LOG_Write("usage: cap [stats|on|off|clear|list [n]|raw [n]]\r\n");
      return 1;
    }
    max = (v == 0u) ? APP_PDCAP_LIST_MAX : v;
  }

  if (argc < 2)
  {
    cap_stats();
  }
  else if (strcmp(argv[1], "stats") == 0)
  {
    cap_stats();
  }
  else if (strcmp(argv[1], "on") == 0)
  {
    APP_CAP_SetEnabled(1u);
    APP_LOG_Write("capture on\r\n");
  }
  else if (strcmp(argv[1], "off") == 0)
  {
    APP_CAP_SetEnabled(0u);
    APP_LOG_Write("capture off\r\n");
  }
  else if (strcmp(argv[1], "clear") == 0)
  {
    APP_CAP_Clear();
    APP_LOG_Write("capture cleared\r\n");
  }
  else if (strcmp(argv[1], "list") == 0)
  {
    cap_list(max);
  }
  else if (strcmp(argv[1], "raw") == 0)
  {
    cap_raw(max);
  }
  else
  {
    APP_LOG_Write("usage: cap [stats|on|off|clear|list [n]|raw [n]]\r\n");
  }
  return 1;
}
