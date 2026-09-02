/**
 * @file    app_cap.c
 * @brief   Lossless RAM ring capture of PD protocol traffic (see app_cap.h).
 */
#include "app_cap.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Critical section                                                    */
/*                                                                     */
/* On the target the producer may be the USB-PD task while the consumer */
/* is the CLI in the super loop, so the index update is made atomic.    */
/* On the host this compiles away, which keeps the module unit-testable.*/
/* ------------------------------------------------------------------ */
#if defined(__ARM_ARCH) || defined(STM32H7R3xx)
static inline uint32_t cap_enter(void)
{
  uint32_t primask;
  __asm volatile ("mrs %0, primask" : "=r" (primask));
  __asm volatile ("cpsid i" ::: "memory");
  return primask;
}

static inline void cap_exit(uint32_t primask)
{
  __asm volatile ("msr primask, %0" :: "r" (primask) : "memory");
}
#define APP_CAP_LOCK()    uint32_t _pm = cap_enter()
#define APP_CAP_UNLOCK()  cap_exit(_pm)
#else
#define APP_CAP_LOCK()    do { } while (0)
#define APP_CAP_UNLOCK()  do { } while (0)
#endif

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static APP_CAP_Rec_t s_ring[APP_CAP_RING_RECORDS];
static volatile uint32_t s_head;      /* next slot to write               */
static volatile uint32_t s_total;     /* records written since Init/Clear */
static volatile uint8_t  s_enabled = 1u;

static struct
{
  uint32_t dropped;
  uint32_t clipped;
  uint32_t malformed;
  uint32_t msg_in;
  uint32_t msg_out;
  uint32_t other;
  uint32_t oldest_ts;
  uint32_t newest_ts;
  uint8_t  wrapped;
} s_stat;

void APP_CAP_Init(void)
{
  APP_CAP_LOCK();
  memset(s_ring, 0, sizeof(s_ring));
  s_head = 0u;
  s_total = 0u;
  memset(&s_stat, 0, sizeof(s_stat));
  s_enabled = 1u;
  APP_CAP_UNLOCK();
}

void APP_CAP_Record(uint8_t type, uint8_t port, uint8_t sop, uint32_t ts,
                    const uint8_t *data, uint32_t size)
{
  APP_CAP_Rec_t *r;
  uint32_t n;

  if (s_enabled == 0u)
  {
    return;
  }

  APP_CAP_LOCK();

  r = &s_ring[s_head];
  r->ts = ts;
  r->type = type;
  r->sop = sop;
  r->port = port;
  r->flags = 0u;

  n = size;
  if (n > APP_CAP_PAYLOAD)
  {
    n = APP_CAP_PAYLOAD;
    r->flags |= APP_CAP_F_CLIPPED;
    s_stat.clipped++;
  }
  r->len = (uint8_t)n;
  if (n != 0u)
  {
    memcpy(r->data, data, n);
  }

  /* A full ring means the record we are about to overwrite has never been
   * consumed - count it as a drop rather than losing the information. */
  if (s_total >= APP_CAP_RING_RECORDS)
  {
    s_stat.dropped++;
    s_stat.wrapped = 1u;
  }

  s_head = (s_head + 1u) % APP_CAP_RING_RECORDS;
  s_total++;

  if (s_total == 1u)
  {
    s_stat.oldest_ts = ts;
  }
  s_stat.newest_ts = ts;

  switch (type)
  {
    case APP_CAP_T_MSG_IN:  s_stat.msg_in++;  break;
    case APP_CAP_T_MSG_OUT: s_stat.msg_out++; break;
    default:                s_stat.other++;   break;
  }

  APP_CAP_UNLOCK();
}

int APP_CAP_Get(uint32_t index, APP_CAP_Rec_t *out)
{
  uint32_t total;
  uint32_t available;
  uint32_t oldest;
  uint32_t slot;

  if (out == NULL)
  {
    return -1;
  }

  {
    APP_CAP_LOCK();
    total = s_total;
    APP_CAP_UNLOCK();
  }

  available = (total > APP_CAP_RING_RECORDS) ? APP_CAP_RING_RECORDS : total;
  if (index >= available)
  {
    return -2;                     /* caller asked beyond the snapshot */
  }

  oldest = total - available;
  slot = (oldest + index) % APP_CAP_RING_RECORDS;

  {
    APP_CAP_LOCK();
    *out = s_ring[slot];
    APP_CAP_UNLOCK();
  }
  return 0;
}

uint32_t APP_CAP_Count(void)
{
  uint32_t total;

  {
    APP_CAP_LOCK();
    total = s_total;
    APP_CAP_UNLOCK();
  }

  return (total > APP_CAP_RING_RECORDS) ? APP_CAP_RING_RECORDS : total;
}

void APP_CAP_GetStats(APP_CAP_Stats_t *out)
{
  uint32_t total;

  if (out == NULL)
  {
    return;
  }

  APP_CAP_LOCK();
  total = s_total;
  out->capacity  = APP_CAP_RING_RECORDS;
  out->count     = (total > APP_CAP_RING_RECORDS) ? APP_CAP_RING_RECORDS : total;
  out->total     = total;
  out->dropped   = s_stat.dropped;
  out->clipped   = s_stat.clipped;
  out->malformed = s_stat.malformed;
  out->msg_in    = s_stat.msg_in;
  out->msg_out   = s_stat.msg_out;
  out->other     = s_stat.other;
  out->oldest_ts = s_stat.oldest_ts;
  out->newest_ts = s_stat.newest_ts;
  out->enabled   = s_enabled;
  out->wrapped   = s_stat.wrapped;
  APP_CAP_UNLOCK();
}

void APP_CAP_Clear(void)
{
  APP_CAP_LOCK();
  memset(s_ring, 0, sizeof(s_ring));
  s_head = 0u;
  s_total = 0u;
  s_stat.dropped = 0u;
  s_stat.clipped = 0u;
  s_stat.malformed = 0u;
  s_stat.msg_in = 0u;
  s_stat.msg_out = 0u;
  s_stat.other = 0u;
  s_stat.oldest_ts = 0u;
  s_stat.newest_ts = 0u;
  s_stat.wrapped = 0u;
  APP_CAP_UNLOCK();
}

void APP_CAP_SetEnabled(uint8_t on)
{
  APP_CAP_LOCK();
  s_enabled = (on != 0u) ? 1u : 0u;
  APP_CAP_UNLOCK();
}

uint8_t APP_CAP_IsEnabled(void)
{
  return s_enabled;
}

uint32_t APP_CAP_ElapsedUs(uint32_t from, uint32_t to, uint32_t core_hz)
{
  uint32_t cycles;

  if (core_hz == 0u)
  {
    return 0u;
  }
  /* unsigned subtraction is wrap-safe for intervals shorter than 2^32 cycles */
  cycles = to - from;
  return (uint32_t)(((uint64_t)cycles * 1000000ull) / (uint64_t)core_hz);
}
