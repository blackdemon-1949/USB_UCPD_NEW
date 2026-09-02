/**
 * @file    app_txn.h
 * @brief   PD transaction / negotiation state reconstruction.
 *
 * Turns the capture stream into transactions: which Request was answered by
 * which Accept / Reject / Wait, whether PS_RDY followed, and how long each
 * step took.  Correlation is per (port, SOP) and honours the 3-bit MessageID
 * rules, so retries and duplicates are recognised rather than counted twice.
 *
 * Pure: no HAL, no ST types, no I/O - unit tested on the host.
 */
#ifndef APP_TXN_H
#define APP_TXN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  APP_TXN_DETACHED = 0,   /* nothing on CC                            */
  APP_TXN_ATTACHED,       /* CC present, no PD traffic yet            */
  APP_TXN_NEGOTIATING,    /* capabilities seen, awaiting a response   */
  APP_TXN_TRANSITION,     /* Accept received, awaiting PS_RDY         */
  APP_TXN_CONTRACT,       /* explicit contract in force               */
  APP_TXN_EPR,            /* EPR contract in force                    */
  APP_TXN_HARD_RESET,     /* recovering from a hard reset             */
  APP_TXN_FAILED          /* last negotiation was rejected / timed out*/
} APP_TXN_State_t;

/* Reasons a transaction may be flagged.  Bitmask. */
#define APP_TXN_F_UNMATCHED   (1u << 0)  /* response with no request      */
#define APP_TXN_F_DUP_MSGID   (1u << 1)  /* same MessageID seen twice     */
#define APP_TXN_F_TIMEOUT     (1u << 2)  /* Accept but no PS_RDY in time  */
#define APP_TXN_F_REJECTED    (1u << 3)  /* Reject / Wait answer          */
#define APP_TXN_F_MALFORMED   (1u << 4)  /* frame failed to decode        */

/* USB PD 3.1 timer limits, in microseconds */
#define APP_TXN_PS_RDY_LIMIT_US   1000000u   /* tPSTransition + tSrcTransition */
#define APP_TXN_SENDER_RSP_US      30000u    /* tSenderResponse               */

typedef struct
{
  uint8_t  state;            /* APP_TXN_State_t                        */
  uint8_t  sop;
  uint8_t  flags;            /* APP_TXN_F_* for the last transaction   */
  uint8_t  last_rsp;         /* 0 none, 2 Accept, 3 Reject, 11 Wait    */
  uint8_t  req_pos;          /* object position of the pending Request */
  uint8_t  last_msg_id;      /* most recent partner MessageID          */
  uint8_t  msg_id_valid;
  /* Explicit "a Request is awaiting an answer" flag.  A timestamp of 0 is a
   * legal cycle-count value, so the timestamps themselves cannot double as
   * a pending indicator. */
  uint8_t  req_pending;
  uint8_t  rsp_pending;

  uint32_t t_cap;            /* timestamp of last Source_Capabilities  */
  uint32_t t_req;            /* timestamp of last Request              */
  uint32_t t_rsp;            /* timestamp of last Accept/Reject/Wait   */
  uint32_t t_psr;            /* timestamp of last PS_RDY               */
  uint32_t t_last;           /* timestamp of any traffic               */

  uint32_t accept_us;        /* Request -> Accept                      */
  uint32_t psrdy_us;         /* Accept -> PS_RDY                       */
  uint32_t contract_us;      /* Request -> PS_RDY                      */

  uint32_t contract_mv;      /* negotiated voltage                     */
  uint32_t contract_ma;      /* negotiated current                     */
  uint8_t  contract_epr;

  uint32_t n_caps;
  uint32_t n_req;
  uint32_t n_accept;
  uint32_t n_reject;
  uint32_t n_wait;
  uint32_t n_psr;
  uint32_t n_goodcrc;
  uint32_t n_soft_reset;
  uint32_t n_hard_reset;
  uint32_t n_retries;
  uint32_t n_dups;
  uint32_t n_unmatched;
  uint32_t n_timeouts;
  uint32_t n_contracts;
  uint32_t n_unsolicited_psr;  /* PS_RDY with no outstanding Request */
} APP_TXN_Port_t;

void APP_TXN_Init(APP_TXN_Port_t *p);
void APP_TXN_Reset(APP_TXN_Port_t *p);

/** Tell the tracker the core clock so cycle deltas become microseconds. */
void APP_TXN_SetClock(uint32_t core_hz);

/** Count a hard reset (signalled at the PHY layer, not a PD message). */
void APP_TXN_NoteHardReset(APP_TXN_Port_t *p);

/** Feed one captured PD frame into the tracker.  @p dir 0 = received, 1 = sent. */
void APP_TXN_Feed(APP_TXN_Port_t *p, uint8_t dir, uint8_t sop, uint32_t ts,
                  const uint8_t *msg, uint16_t len);

/** Call when the CAD layer reports a detach. */
void APP_TXN_Detach(APP_TXN_Port_t *p);

/**
 * Check pending transactions for timeouts.  Safe to call every loop turn.
 * @p core_hz converts the cycle timestamps to microseconds.
 */
void APP_TXN_Poll(APP_TXN_Port_t *p, uint32_t now_ts, uint32_t core_hz);

const char *APP_TXN_StateName(uint8_t state);

/** Record the operating point that the local policy engine asked for. */
void APP_TXN_NoteRequest(APP_TXN_Port_t *p, uint32_t mv, uint32_t ma,
                         uint8_t epr);

#ifdef __cplusplus
}
#endif

#endif /* APP_TXN_H */
