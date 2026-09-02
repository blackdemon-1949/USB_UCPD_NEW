/**
 * @file    app_ext.h
 * @brief   Chunked extended-message reassembly.
 *
 * PD 3.0 extended messages (Source_Capabilities_Extended, Status, PPS_Status,
 * Manufacturer_Info, EPR_Source_Capabilities, ...) carry up to 256 bytes of
 * data but a chunk is limited by the packet payload, so a large message
 * arrives as several frames.  Each frame repeats the message header and
 * carries an extended header whose ChunkNumber says where its payload belongs.
 *
 * This engine stitches them back together and, importantly, records the ways
 * in which a chunked transfer can be *wrong* - a gap, a duplicate, an
 * out-of-order chunk, a type change mid-transfer, or a declared size that
 * cannot fit.  An analyzer has to report those rather than silently emit a
 * short buffer.
 *
 * Pure: no I/O, no globals, no allocation.  The caller owns the state.
 */
#ifndef APP_EXT_H
#define APP_EXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#include "app_dec.h"   /* APP_DEC_Msg_t */

/** Largest extended data size the PD 3.0 extended header can express. */
#define APP_EXT_MAX_DATA 256u

/** Highest chunk number the 4-bit field can express. */
#define APP_EXT_MAX_CHUNKS 16u

/* Failure reasons, bitmask. */
#define APP_EXT_ERR_NONE      0u
#define APP_EXT_ERR_GAP       (1u << 0)  /* chunk number skipped            */
#define APP_EXT_ERR_DUP       (1u << 1)  /* chunk number repeated           */
#define APP_EXT_ERR_ORDER     (1u << 2)  /* chunk arrived out of sequence   */
#define APP_EXT_ERR_TYPE      (1u << 3)  /* message type changed mid-stream */
#define APP_EXT_ERR_OVERFLOW  (1u << 4)  /* more data than APP_EXT_MAX_DATA */
#define APP_EXT_ERR_CHUNKSIZE (1u << 5)  /* chunk payload larger than the
                                          * largest legal chunk             */
#define APP_EXT_ERR_NOTCHUNK  (1u << 6)  /* fed a non-chunked frame         */
#define APP_EXT_ERR_BADDEC    (1u << 7)  /* decoder rejected the frame      */

typedef struct
{
  uint8_t  active;                    /* a chunked transfer is in progress   */
  uint8_t  complete;                  /* all declared bytes collected        */
  uint8_t  msg_type;                  /* extended message type               */
  uint8_t  msg_id;
  uint16_t total;                     /* declared DataSize                   */
  uint16_t have;                      /* bytes collected                     */
  uint8_t  req_chunk;
  uint8_t  n_chunks;                  /* frames accepted                     */
  uint32_t seen;                      /* bit i = chunk i received            */
  uint8_t  expect;                    /* next chunk number expected          */
  uint8_t  errors;                    /* APP_EXT_ERR_*                       */
  uint8_t  data[APP_EXT_MAX_DATA];
} APP_EXT_Reasm_t;

/** Clear any in-progress transfer. */
void APP_EXT_Reset(APP_EXT_Reasm_t *r);

/**
 * Feed one decoded frame.  Non-chunked frames are ignored unless they are a
 * complete unchunked extended message, in which case they are copied straight
 * through and @ref APP_EXT_Reasm_t.complete is set.
 *
 * @return 1 when the message is now complete, 0 when more chunks are needed.
 */
int APP_EXT_Feed(APP_EXT_Reasm_t *r, const APP_DEC_Msg_t *m);

/** Largest legal chunk payload for a given total, per PD 3.0. */
uint16_t APP_EXT_ChunkSize(uint16_t total);

/** Name for an extended-message error bitmask, for display. */
void APP_EXT_FormatErrors(uint8_t errors, char *out, size_t outsz);

/** `ext` CLI command: current reassembly state. */
int APP_EXT_Cmd(int argc, char *argv[]);

/** Feed the target-side reassembler.  @return 1 when complete. */
int APP_EXT_LiveFeed(const APP_DEC_Msg_t *m);

/** The target-side reassembler, fed from the capture path. */
const APP_EXT_Reasm_t *APP_EXT_GetLive(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_EXT_H */
