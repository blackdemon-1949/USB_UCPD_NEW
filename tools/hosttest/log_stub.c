/*
 * Host stand-in for the target logging backend.
 *
 * The engines under test call APP_LOG_* from their CLI entry points.  On the
 * host there is no USB CDC, so this stub formats to a buffer that a test can
 * inspect, which lets the CLI formatting paths be covered too rather than
 * stubbing them away.
 */
#include "app_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_STUB_BUF 4096

static char   s_buf[LOG_STUB_BUF];
static size_t s_len;

void APP_LOG_Init(void)            { s_len = 0u; s_buf[0] = '\0'; }
void APP_LOG_Flush(void)           { }
uint8_t APP_LOG_UsbReady(void)     { return 1u; }
void APP_LOG_SetUsbReady(uint8_t r) { (void)r; }
void APP_LOG_OnTxComplete(void)    { }
void APP_LOG_OnUsbConnect(void)    { }
void APP_LOG_OnUsbSuspend(void)    { }

int APP_LOG_Printf(const char *fmt, ...)
{
  va_list ap;
  int n;

  if (s_len >= (LOG_STUB_BUF - 1u))
  {
    return 0;
  }
  va_start(ap, fmt);
  n = vsnprintf(&s_buf[s_len], (size_t)(LOG_STUB_BUF - s_len), fmt, ap);
  va_end(ap);
  if (n > 0)
  {
    s_len += (size_t)n;
    if (s_len > (LOG_STUB_BUF - 1u))
    {
      s_len = LOG_STUB_BUF - 1u;
    }
  }
  return n;
}

void APP_LOG_Write(const char *s)
{
  if (s != NULL)
  {
    (void)APP_LOG_Printf("%s", s);
  }
}

void APP_LOG_WriteRaw(const uint8_t *data, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++)
  {
    (void)APP_LOG_Printf("%02X", (unsigned)data[i]);
  }
}

/* Test helper: expose what was logged. */
const char *log_stub_text(void) { return s_buf; }
void log_stub_reset(void)       { s_len = 0u; s_buf[0] = '\0'; }
