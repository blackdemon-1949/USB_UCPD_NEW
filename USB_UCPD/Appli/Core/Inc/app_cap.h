/**
 * @file    app_cap.h
 * @brief   Lossless RAM ring capture of PD protocol traffic.
 *
 * This module is a pure ring buffer: it knows nothing about the ST stack,
 * nothing about DMA and nothing about the console.  Producers hand it a
 * timestamp plus raw bytes; consumers walk the ring.  Keeping it free of
 * hardware access means it can be unit tested on the host.
 *
 * Placement: the ring lives in ordinary .bss (cacheable AXI SRAM).  It is
 * written from the USB-PD task context and read from the super loop, never
 * from a DMA master, so it must NOT go in the non-cacheable window - that
 * window is reserved for buffers a DMA engine touches.
 *
 * ISR discipline: APP_CAP_Record() only memcpy()s into the ring and bumps a
 * few counters.  It never prints, never blocks, never allocates and never
 * touches I2C, CDC or flash.
 */
#ifndef APP_CAP_H
#define APP_CAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Bytes of PD payload kept per record.  A standard PD message is at most
 *  2 header + 7 x 4 = 30 bytes, and a chunked extended message carries at
 *  most 26 payload bytes, so 32 covers every unchunked and chunked frame in
 *  full.  Longer unchunked extended payloads are clipped and flagged. */
#define APP_CAP_PAYLOAD       32u

/** Ring depth.  512 x 40 bytes = 20 KiB of AXI SRAM, which holds roughly the
 *  last 500 messages of a negotiation - far more than a full PD 3.1
 *  transaction needs. */
#define APP_CAP_RING_RECORDS  512u

/* Producer record types.  Values 1..16 deliberately match ST's TRACE_EVENT
 * numbering so a record can be correlated with a TRACER_EMB frame. */
#define APP_CAP_T_MSG_IN        1u
#define APP_CAP_T_MSG_OUT       2u
#define APP_CAP_T_CAD_EVENT     3u
#define APP_CAP_T_PE_STATE      4u
#define APP_CAP_T_NOTIF         9u
#define APP_CAP_T_OTHER         0x40u

/* Record flags */
#define APP_CAP_F_CLIPPED     (1u << 0)  /* payload longer than APP_CAP_PAYLOAD */
#define APP_CAP_F_MALFORMED   (1u << 1)  /* decoder rejected the frame         */

typedef struct
{
  uint32_t ts;                          /* producer timestamp (cycles)      */
  uint8_t  type;                        /* APP_CAP_T_*                      */
  uint8_t  sop;                         /* 0/1/2 or 0xFF when not applicable */
  uint8_t  port;
  uint8_t  flags;                       /* APP_CAP_F_*                      */
  uint8_t  len;                         /* bytes stored in data[]           */
  uint8_t  data[APP_CAP_PAYLOAD];
} APP_CAP_Rec_t;

typedef struct
{
  uint32_t capacity;                    /* ring depth in records            */
  uint32_t count;                       /* records currently readable       */
  uint32_t total;                       /* records ever written             */
  uint32_t dropped;                     /* overwritten before being read    */
  uint32_t clipped;                     /* payload longer than the record   */
  uint32_t malformed;                   /* frames the decoder rejected      */
  uint32_t msg_in;                      /* received PD messages             */
  uint32_t msg_out;                     /* transmitted PD messages          */
  uint32_t other;                       /* state / notification records     */
  uint32_t oldest_ts;
  uint32_t newest_ts;
  uint8_t  enabled;
  uint8_t  wrapped;                     /* 1 once the ring has lapped       */
} APP_CAP_Stats_t;

void APP_CAP_Init(void);

/**
 * Producer entry point - safe from the USB-PD task and from ISRs.
 *
 * @param type  APP_CAP_T_* record type
 * @param port  port index
 * @param sop   SOP type, or 0xFF when the record has no SOP
 * @param ts    producer timestamp
 * @param data  raw bytes, may be NULL when @p size is 0
 * @param size  byte count; anything beyond APP_CAP_PAYLOAD is clipped
 */
void APP_CAP_Record(uint8_t type, uint8_t port, uint8_t sop, uint32_t ts,
                    const uint8_t *data, uint32_t size);

/* Consumer API.  APP_CAP_Get() indexes from the OLDEST readable record, so
 * a caller can walk a snapshot without racing the producer. */
int      APP_CAP_Get(uint32_t index, APP_CAP_Rec_t *out);
uint32_t APP_CAP_Count(void);
void     APP_CAP_GetStats(APP_CAP_Stats_t *out);
void     APP_CAP_Clear(void);
void     APP_CAP_SetEnabled(uint8_t on);
uint8_t  APP_CAP_IsEnabled(void);

/** Microseconds between two producer timestamps (wrap-safe). */
uint32_t APP_CAP_ElapsedUs(uint32_t from, uint32_t to, uint32_t core_hz);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAP_H */
