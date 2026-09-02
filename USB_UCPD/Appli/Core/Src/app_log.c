#include "app_log.h"
#include "usbd_cdc_if.h"
#include "usb_device.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* 8 kB: the help text alone is ~2.5 kB and PD/EPR events can burst while the
 * host is not draining.  At 2 kB the queue overflowed and dropped the tail
 * silently, which on the bench looked exactly like a firmware reboot. */
#define LOG_Q_SIZE  8192U

static uint8_t  s_q[LOG_Q_SIZE];
static uint16_t s_head;
static uint16_t s_tail;
static uint8_t  s_usb_ready;
static volatile uint8_t s_tx_busy;
static uint32_t s_dropped;      /* bytes discarded because the queue was full */
static uint8_t  s_drop_flagged; /* 1 = a drop notice is still owed to the host */
/* CDC IN DMA reads this buffer; it must not be held in a stale D-cache line. */
static uint8_t  s_tx[256]
  __attribute__((section("noncacheable_buffer"), aligned(32)));

extern USBD_HandleTypeDef hUsbDeviceHS;

uint8_t APP_LOG_UsbReady(void)
{
  return s_usb_ready;
}

void APP_LOG_SetUsbReady(uint8_t ready)
{
  s_usb_ready = ready;
}

void APP_LOG_OnTxComplete(void)
{
  s_tx_busy = 0U;
}

void APP_LOG_OnUsbConnect(void)
{
  /* Called from CDC_Init_HS on every SET_CONFIGURATION (i.e. on every
   * enumeration).  A bus reset / unplug can interrupt an IN transfer, in
   * which case TransmitCplt never fires and s_tx_busy would stay 1 for
   * ever: the console would go silent until the next replug.  Re-arm it
   * here so a re-enumerated port always prints again. */
  s_tx_busy = 0U;
}

void APP_LOG_OnUsbSuspend(void)
{
  /* Bus suspended: drop the session and re-arm the TX state. */
  s_usb_ready = 0U;
  s_tx_busy = 0U;
}

void APP_LOG_Init(void)
{
  s_head = 0;
  s_tail = 0;
  s_usb_ready = 0;
  s_tx_busy = 0;
}

static uint16_t q_count(void)
{
  if (s_head >= s_tail)
  {
    return (uint16_t)(s_head - s_tail);
  }
  return (uint16_t)(LOG_Q_SIZE - s_tail + s_head);
}

void APP_LOG_WriteRaw(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++)
  {
    uint16_t next = (uint16_t)((s_head + 1U) % LOG_Q_SIZE);
    if (next == s_tail)
    {
      /* Queue full.  Account for the loss instead of dropping it silently -
       * silent truncation is what made a chopped console line look like a
       * spontaneous reboot. */
      s_dropped += (uint32_t)(len - i);
      s_drop_flagged = 1U;
      break;
    }
    s_q[s_head] = data[i];
    s_head = next;
  }
}

uint32_t APP_LOG_Dropped(void)
{
  return s_dropped;
}

void APP_LOG_Write(const char *s)
{
  if (s == NULL)
  {
    return;
  }
  APP_LOG_WriteRaw((const uint8_t *)s, (uint16_t)strlen(s));
}

int APP_LOG_Printf(const char *fmt, ...)
{
  char buf[256];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
  {
    return n;
  }
  if (n >= (int)sizeof(buf))
  {
    n = (int)sizeof(buf) - 1;
  }
  APP_LOG_WriteRaw((const uint8_t *)buf, (uint16_t)n);
  return n;
}

void APP_LOG_Flush(void)
{
  uint16_t n;
  uint16_t i;

  if ((s_usb_ready == 0U) || (hUsbDeviceHS.dev_state != USBD_STATE_CONFIGURED))
  {
    return;
  }
  if (s_tx_busy != 0U)
  {
    return;
  }
  n = q_count();
  if (n == 0U)
  {
    return;
  }
  if (n > sizeof(s_tx))
  {
    n = (uint16_t)sizeof(s_tx);
  }
  for (i = 0; i < n; i++)
  {
    s_tx[i] = s_q[s_tail];
    s_tail = (uint16_t)((s_tail + 1U) % LOG_Q_SIZE);
  }
  s_tx_busy = 1U;
  if (CDC_Transmit_HS(s_tx, n) != USBD_OK)
  {
    s_tx_busy = 0U;
  }
}
