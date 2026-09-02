/**
 * @file    app_txn.c
 * @brief   PD transaction / negotiation state reconstruction (see app_txn.h).
 */
#include "app_txn.h"
#include "app_log.h"
#include "app_dec.h"
#include "app_cap.h"      /* APP_CAP_ElapsedUs - one cycle->us implementation */
#include <string.h>

/* PD message types on the wire (see app_dec.c tables) */
/* Wire values, per USB PD 3.1 Table 6-5 and USBPD_ControlMsg_TypeDef in
 * usbpd_def.h (GOODCRC 0x01, ACCEPT 0x03, REJECT 0x04, PS_RDY 0x06,
 * WAIT 0x0C, SOFT_RESET 0x0D).  These were previously one lower, which meant
 * a real Accept or PS_RDY was never recognised and no transaction ever
 * reached the contract state on live traffic. */
#define MT_GOODCRC   1u
#define MT_ACCEPT    3u
#define MT_REJECT    4u
#define MT_PS_RDY    6u
#define MT_WAIT      12u
#define MT_SOFT_RST  13u

#define MT_SRC_CAP   1u
#define MT_REQUEST   2u
#define MT_EPR_REQ   9u
#define MT_EPR_MODE  10u

static uint32_t s_core_hz = 400000000u;

void APP_TXN_SetClock(uint32_t core_hz)
{
  s_core_hz = (core_hz != 0u) ? core_hz : 1u;
}

static uint32_t us_between(uint32_t a, uint32_t b)
{
  return APP_CAP_ElapsedUs(a, b, s_core_hz);
}

void APP_TXN_Init(APP_TXN_Port_t *p)
{
  if (p != NULL)
  {
    memset(p, 0, sizeof(*p));
    p->state = APP_TXN_DETACHED;
    p->sop = 0xFFu;
    p->last_msg_id = 0xFFu;
  }
}

void APP_TXN_Reset(APP_TXN_Port_t *p)
{
  if (p != NULL)
  {
    APP_TXN_Init(p);
  }
}

void APP_TXN_Detach(APP_TXN_Port_t *p)
{
  if (p == NULL)
  {
    return;
  }
  /* keep the lifetime counters, drop the in-flight negotiation */
  p->state = APP_TXN_DETACHED;
  p->flags = 0u;
  p->last_rsp = 0u;
  p->req_pos = 0u;
  p->msg_id_valid = 0u;
  p->last_msg_id = 0xFFu;
  p->req_pending = 0u;
  p->rsp_pending = 0u;
  p->t_cap = 0u;
  p->t_req = 0u;
  p->t_rsp = 0u;
  p->t_psr = 0u;
  p->contract_mv = 0u;
  p->contract_ma = 0u;
  p->contract_epr = 0u;
}

void APP_TXN_NoteHardReset(APP_TXN_Port_t *p)
{
  if (p == NULL)
  {
    return;
  }
  p->n_hard_reset++;
  p->state = APP_TXN_HARD_RESET;
  p->msg_id_valid = 0u;      /* MessageID restarts at 0 after a hard reset */
  p->req_pending = 0u;
  p->rsp_pending = 0u;
  p->last_msg_id = 0xFFu;
  p->contract_mv = 0u;
  p->contract_ma = 0u;
  p->contract_epr = 0u;
}

void APP_TXN_NoteRequest(APP_TXN_Port_t *p, uint32_t mv, uint32_t ma,
                         uint8_t epr)
{
  if (p == NULL)
  {
    return;
  }
  p->contract_mv = mv;
  p->contract_ma = ma;
  p->contract_epr = epr ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* Feeding                                                             */
/* ------------------------------------------------------------------ */

static void handle_control(APP_TXN_Port_t *p, uint8_t dir, uint8_t type,
                           uint32_t ts)
{
  switch (type)
  {
    case MT_GOODCRC:
      p->n_goodcrc++;
      break;

    case MT_ACCEPT:
      p->n_accept++;
      p->last_rsp = MT_ACCEPT;
      p->t_rsp = ts;
      if (dir == 0u)      /* received from the source */
      {
        if (p->req_pending != 0u)
        {
          p->accept_us = us_between(p->t_req, ts);
          p->flags &= (uint8_t)~APP_TXN_F_UNMATCHED;
        }
        else
        {
          p->flags |= APP_TXN_F_UNMATCHED;
          p->n_unmatched++;
        }
        /* The Request stays outstanding until PS_RDY completes it; only the
         * "an answer arrived" latch is set here. */
        p->rsp_pending = 1u;
        p->state = APP_TXN_TRANSITION;
      }
      break;

    case MT_REJECT:
      p->n_reject++;
      p->last_rsp = MT_REJECT;
      p->t_rsp = ts;
      p->flags |= APP_TXN_F_REJECTED;
      if (dir == 0u)
      {
        if (p->req_pending == 0u)
        {
          p->flags |= APP_TXN_F_UNMATCHED;
          p->n_unmatched++;
        }
        p->req_pending = 0u;
        p->rsp_pending = 0u;
        p->state = APP_TXN_FAILED;
      }
      break;

    case MT_WAIT:
      p->n_wait++;
      p->last_rsp = MT_WAIT;
      p->t_rsp = ts;
      p->flags |= APP_TXN_F_REJECTED;
      if (dir == 0u)
      {
        if (p->req_pending == 0u)
        {
          p->flags |= APP_TXN_F_UNMATCHED;
          p->n_unmatched++;
        }
        /* WAIT leaves the Request outstanding: the sink may retry it. */
        p->state = APP_TXN_NEGOTIATING;
      }
      break;

    case MT_PS_RDY:
      p->n_psr++;
      if (dir == 0u)
      {
        p->t_psr = ts;
        if (p->rsp_pending != 0u)
        {
          p->psrdy_us = us_between(p->t_rsp, ts);
        }
        p->rsp_pending = 0u;
        if (p->req_pending != 0u)
        {
          /* PS_RDY completes the pending Request: this is the contract. */
          p->contract_us = us_between(p->t_req, ts);
          p->req_pending = 0u;
        }
        else
        {
          /* Legal - the source re-establishes the default contract after a
           * Hard Reset without a Request - but distinct from a negotiated
           * contract, so count it separately rather than passing it off as
           * one.  The fuzz engine treats a silent accept here as a defect. */
          p->n_unsolicited_psr++;
        }
        p->n_contracts++;
        p->state = p->contract_epr ? APP_TXN_EPR : APP_TXN_CONTRACT;
        p->flags &= (uint8_t)~APP_TXN_F_TIMEOUT;
      }
      break;

    case MT_SOFT_RST:
      p->n_soft_reset++;
      p->msg_id_valid = 0u;
      p->last_msg_id = 0xFFu;
      break;

    default:
      break;
  }
}

void APP_TXN_Feed(APP_TXN_Port_t *p, uint8_t dir, uint8_t sop, uint32_t ts,
                  const uint8_t *msg, uint16_t len)
{
  APP_DEC_Msg_t m;

  if ((p == NULL) || (msg == NULL))
  {
    return;
  }

  p->t_last = ts;
  p->sop = sop;
  if (p->state == APP_TXN_DETACHED)
  {
    p->state = APP_TXN_ATTACHED;
  }

  if (APP_DEC_Decode(msg, len, &m) != 0)
  {
    p->flags |= APP_TXN_F_MALFORMED;
    return;
  }
  if ((m.flags & (APP_DEC_F_SHORT | APP_DEC_F_TYPE_RSV)) != 0u)
  {
    p->flags |= APP_TXN_F_MALFORMED;
  }

  /* MessageID duplicate detection.  GoodCRC and protocol error messages keep
   * the previous MessageID, so they must not be counted as duplicates. */
  if ((m.msg_class != APP_DEC_CLASS_INVALID) && (m.msg_type != MT_GOODCRC))
  {
    if ((p->msg_id_valid != 0u) && (m.msg_id == p->last_msg_id))
    {
      p->n_dups++;
      p->flags |= APP_TXN_F_DUP_MSGID;
    }
    p->last_msg_id = m.msg_id;
    p->msg_id_valid = 1u;
  }

  if (m.extended != 0u)
  {
    /* Extended traffic does not drive the SPR negotiation state machine, but
     * EPR capability exchanges are worth counting. */
    return;
  }

  if (m.msg_class == APP_DEC_CLASS_CONTROL)
  {
    handle_control(p, dir, m.msg_type, ts);
    return;
  }

  if (m.msg_class != APP_DEC_CLASS_DATA)
  {
    return;
  }

  switch (m.msg_type)
  {
    case MT_SRC_CAP:
      if (dir == 0u)
      {
        p->n_caps++;
        p->t_cap = ts;
        if ((p->state != APP_TXN_CONTRACT) && (p->state != APP_TXN_EPR))
        {
          p->state = APP_TXN_NEGOTIATING;
        }
      }
      break;

    case MT_REQUEST:
      if (dir == 1u)          /* the Request we sent */
      {
        p->n_req++;
        p->t_req = ts;
        p->t_rsp = 0u;
        p->t_psr = 0u;
        p->req_pending = 1u;
        p->rsp_pending = 0u;
        p->accept_us = 0u;
        p->psrdy_us = 0u;
        p->contract_us = 0u;
        p->flags &= (uint8_t)~(APP_TXN_F_UNMATCHED | APP_TXN_F_REJECTED |
                               APP_TXN_F_TIMEOUT);
        p->state = APP_TXN_NEGOTIATING;
        if (m.data_len >= 4u)
        {
          uint32_t rdo = (uint32_t)m.data[0] | ((uint32_t)m.data[1] << 8) |
                         ((uint32_t)m.data[2] << 16) | ((uint32_t)m.data[3] << 24);
          p->req_pos = APP_DEC_RDO_POS(rdo);
        }
      }
      break;

    case MT_EPR_REQ:
      if (dir == 1u)
      {
        p->n_req++;
        p->t_req = ts;
        p->req_pending = 1u;
        p->rsp_pending = 0u;
        p->state = APP_TXN_NEGOTIATING;
      }
      break;

    case MT_EPR_MODE:
      /* EPR mode entry/exit is tracked through its own engine; here we only
       * note that the transaction is EPR related. */
      break;

    default:
      break;
  }
}

void APP_TXN_Poll(APP_TXN_Port_t *p, uint32_t now_ts, uint32_t core_hz)
{
  uint32_t elapsed;

  if ((p == NULL) || (core_hz == 0u))
  {
    return;
  }

  /* A Request that got an Accept but never saw PS_RDY has stalled. */
  if ((p->state == APP_TXN_TRANSITION) && (p->rsp_pending != 0u))
  {
    elapsed = APP_CAP_ElapsedUs(p->t_rsp, now_ts, core_hz);
    if (elapsed > APP_TXN_PS_RDY_LIMIT_US)
    {
      p->flags |= APP_TXN_F_TIMEOUT;
      p->n_timeouts++;
      p->state = APP_TXN_FAILED;
    }
  }
  /* A Request with no answer at all. */
  else if ((p->state == APP_TXN_NEGOTIATING) && (p->req_pending != 0u))
  {
    elapsed = APP_CAP_ElapsedUs(p->t_req, now_ts, core_hz);
    if (elapsed > APP_TXN_SENDER_RSP_US)
    {
      p->flags |= APP_TXN_F_TIMEOUT;
      p->n_timeouts++;
      p->state = APP_TXN_FAILED;
    }
  }
}

const char *APP_TXN_StateName(uint8_t state)
{
  switch (state)
  {
    case APP_TXN_DETACHED:    return "detached";
    case APP_TXN_ATTACHED:    return "attached";
    case APP_TXN_NEGOTIATING: return "negotiating";
    case APP_TXN_TRANSITION:  return "transition";
    case APP_TXN_CONTRACT:    return "contract";
    case APP_TXN_EPR:         return "contract(EPR)";
    case APP_TXN_HARD_RESET:  return "hard-reset";
    case APP_TXN_FAILED:      return "failed";
    default:                  return "UNKNOWN";
  }
}

