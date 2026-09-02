/**
 * @file    app_vdm.c
 * @brief   Vendor Defined Message / alternate-mode state logic (see app_vdm.h).
 *
 * Deliberately free of ST headers so the validation, counters and status
 * classifier can be exercised on the host under sanitizers.  The actual ST PE
 * calls live in app_vdm_target.c.
 */
#include "app_vdm.h"
#include "app_log.h"

#include <string.h>
#include <stdio.h>

/** USBPD_PORT_COUNT is 1 in this project; only port 0 exists. */
#define APP_VDM_PORT_MAX   0u
/** Mode index is 1-based per the SVDM Discover Modes response. */
#define APP_VDM_MODE_MIN   1u
#define APP_VDM_MODE_MAX   6u

static APP_VDM_State_t s_vdm;

void APP_VDM_Init(void)
{
  memset(&s_vdm, 0, sizeof(s_vdm));
}

const APP_VDM_State_t *APP_VDM_Get(void)
{
  return &s_vdm;
}

void APP_VDM_Clear(void)
{
  uint8_t keep_in = s_vdm.in_alt_mode;

  memset(&s_vdm, 0, sizeof(s_vdm));
  s_vdm.in_alt_mode = keep_in;
}

/* ------------------------------------------------------------------ */
/* Validation and request bookkeeping                                  */
/* ------------------------------------------------------------------ */

/**
 * Validate a request and mark it outstanding.
 * @return 0 when the request is well-formed and may be handed to the PE.
 */
int APP_VDM_Prepare(uint8_t port, uint8_t sop, APP_VDM_Req_t req,
                    uint16_t svid, uint8_t index)
{
  if ((req <= APP_VDM_REQ_NONE) || (req >= APP_VDM_REQ_COUNT))
  {
    return APP_VDM_CALL_REJECTED;
  }
  if (port > APP_VDM_PORT_MAX)
  {
    return APP_VDM_CALL_REJECTED;
  }
  if (sop > APP_VDM_SOP2)
  {
    return APP_VDM_CALL_REJECTED;
  }

  switch (req)
  {
    case APP_VDM_REQ_IDENTITY:
    case APP_VDM_REQ_SVID:
      /* No SVID or mode index is meaningful here. */
      svid = 0u;
      index = 0u;
      break;

    case APP_VDM_REQ_MODE:
      if (svid == 0u)
      {
        return APP_VDM_CALL_REJECTED;
      }
      index = 0u;
      break;

    case APP_VDM_REQ_MODE_ENTER:
    case APP_VDM_REQ_MODE_EXIT:
      if (svid == 0u)
      {
        return APP_VDM_CALL_REJECTED;
      }
      /* Mode index 0 means "all modes" for exit, so 0 is legal there but not
       * for enter, where exactly one mode must be selected. */
      if ((req == APP_VDM_REQ_MODE_ENTER) &&
          ((index < APP_VDM_MODE_MIN) || (index > APP_VDM_MODE_MAX)))
      {
        return APP_VDM_CALL_REJECTED;
      }
      if (index > APP_VDM_MODE_MAX)
      {
        return APP_VDM_CALL_REJECTED;
      }
      break;

    default:
      return APP_VDM_CALL_REJECTED;
  }

  /* Only one SVDM transaction may be outstanding at a time; the PE serialises
   * anyway, but refusing early gives the user a clear answer. */
  if ((s_vdm.pending != 0u) && (s_vdm.pending_req != (uint8_t)APP_VDM_REQ_NONE))
  {
    return APP_VDM_CALL_REJECTED;
  }

  s_vdm.port = port;
  s_vdm.sop = sop;
  s_vdm.svid = svid;
  s_vdm.mode_index = index;
  s_vdm.pending = 1u;
  s_vdm.pending_req = (uint8_t)req;
  return APP_VDM_CALL_OK;
}

/**
 * Record whether the ST PE accepted the request.
 * @param st_ok non-zero when the PE returned USBPD_OK.
 */
int APP_VDM_CompleteCall(int st_ok)
{
  APP_VDM_Req_t req = (APP_VDM_Req_t)s_vdm.pending_req;

  switch (req)
  {
    case APP_VDM_REQ_IDENTITY:   s_vdm.n_identity_req++; break;
    case APP_VDM_REQ_SVID:       s_vdm.n_svid_req++;     break;
    case APP_VDM_REQ_MODE:       s_vdm.n_mode_req++;     break;
    case APP_VDM_REQ_MODE_ENTER: s_vdm.n_enter_req++;    break;
    case APP_VDM_REQ_MODE_EXIT:  s_vdm.n_exit_req++;     break;
    default: break;
  }

  if (st_ok == 0)
  {
    /* The PE refused: nothing is outstanding, so clear the latch and say so. */
    s_vdm.n_rejected++;
    s_vdm.last_rejected = 1u;
    s_vdm.last_req = (uint8_t)req;
    s_vdm.pending = 0u;
    s_vdm.pending_req = (uint8_t)APP_VDM_REQ_NONE;
    return APP_VDM_CALL_REJECTED;
  }

  s_vdm.last_rejected = 0u;
  return APP_VDM_CALL_OK;
}

/* ------------------------------------------------------------------ */
/* Response classification                                             */
/* ------------------------------------------------------------------ */

uint8_t APP_VDM_ApplyStatus(uint8_t in_alt_mode, uint8_t is_enter,
                            uint32_t status)
{
  if (status == (uint32_t)APP_VDM_STAT_ACK)
  {
    /* Only an ACK'd Enter puts us in the mode; only an ACK'd Exit leaves it.
     * NAK and BUSY leave the current state untouched. */
    return (is_enter != 0u) ? 1u : 0u;
  }
  return in_alt_mode;
}

static void record_mode(uint8_t is_enter, uint8_t sop, uint32_t status,
                        uint16_t svid, uint32_t mode_index)
{
  s_vdm.last_status = (uint8_t)status;
  s_vdm.last_req = is_enter ? (uint8_t)APP_VDM_REQ_MODE_ENTER
                            : (uint8_t)APP_VDM_REQ_MODE_EXIT;
  s_vdm.last_svid = svid;
  s_vdm.last_mode_index = (uint8_t)mode_index;
  s_vdm.pending = 0u;
  s_vdm.pending_req = (uint8_t)APP_VDM_REQ_NONE;

  switch (status)
  {
    case (uint32_t)APP_VDM_STAT_ACK:
      if (is_enter != 0u) { s_vdm.n_enter_ack++; } else { s_vdm.n_exit_ack++; }
      break;
    case (uint32_t)APP_VDM_STAT_NAK:
      if (is_enter != 0u) { s_vdm.n_enter_nak++; } else { s_vdm.n_exit_nak++; }
      break;
    case (uint32_t)APP_VDM_STAT_BUSY:
      if (is_enter != 0u) { s_vdm.n_enter_busy++; } else { s_vdm.n_exit_busy++; }
      break;
    default:
      break;
  }

  s_vdm.in_alt_mode = APP_VDM_ApplyStatus(s_vdm.in_alt_mode, is_enter, status);
  s_vdm.sop = sop;
}

void APP_VDM_OnModeEnter(uint8_t port, uint8_t sop, uint32_t status,
                         uint16_t svid, uint32_t mode_index)
{
  (void)port;
  record_mode(1u, sop, status, svid, mode_index);
}

void APP_VDM_OnModeExit(uint8_t port, uint8_t sop, uint32_t status,
                        uint16_t svid, uint32_t mode_index)
{
  (void)port;
  record_mode(0u, sop, status, svid, mode_index);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

const char *APP_VDM_ReqName(APP_VDM_Req_t r)
{
  switch (r)
  {
    case APP_VDM_REQ_NONE:       return "none";
    case APP_VDM_REQ_IDENTITY:   return "Discover_Identity";
    case APP_VDM_REQ_SVID:       return "Discover_SVIDs";
    case APP_VDM_REQ_MODE:       return "Discover_Modes";
    case APP_VDM_REQ_MODE_ENTER: return "Enter_Mode";
    case APP_VDM_REQ_MODE_EXIT:  return "Exit_Mode";
    default:                     return "?";
  }
}

const char *APP_VDM_StatName(uint8_t status)
{
  switch (status)
  {
    case APP_VDM_STAT_NONE: return "none";
    case APP_VDM_STAT_ACK:  return "ACK";
    case APP_VDM_STAT_NAK:  return "NAK";
    case APP_VDM_STAT_BUSY: return "BUSY";
    default:                return "?";
  }
}

const char *APP_VDM_SopName(uint8_t sop)
{
  switch (sop)
  {
    case APP_VDM_SOP:  return "SOP";
    case APP_VDM_SOP1: return "SOP'";
    case APP_VDM_SOP2: return "SOP''";
    default:           return "?";
  }
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void print_state(void)
{
  const APP_VDM_State_t *s = APP_VDM_Get();

  APP_LOG_Write("vdm state\r\n");
  APP_LOG_Printf("  port/sop      : %u / %s\r\n", s->port,
                 APP_VDM_SopName(s->sop));
  APP_LOG_Printf("  in alt mode   : %s\r\n", s->in_alt_mode ? "YES" : "no");
  APP_LOG_Printf("  pending       : %s\r\n",
                 APP_VDM_ReqName((APP_VDM_Req_t)s->pending_req));
  APP_LOG_Printf("  last request  : %s svid=0x%04X mode=%u -> %s%s\r\n",
                 APP_VDM_ReqName((APP_VDM_Req_t)s->last_req),
                 (unsigned)s->last_svid, (unsigned)s->last_mode_index,
                 APP_VDM_StatName(s->last_status),
                 s->last_rejected ? " (PE rejected request)" : "");
  APP_LOG_Printf("  enter req/ack/nak/busy : %lu/%lu/%lu/%lu\r\n",
                 (unsigned long)s->n_enter_req, (unsigned long)s->n_enter_ack,
                 (unsigned long)s->n_enter_nak,
                 (unsigned long)s->n_enter_busy);
  APP_LOG_Printf("  exit  req/ack/nak/busy : %lu/%lu/%lu/%lu\r\n",
                 (unsigned long)s->n_exit_req, (unsigned long)s->n_exit_ack,
                 (unsigned long)s->n_exit_nak,
                 (unsigned long)s->n_exit_busy);
  APP_LOG_Printf("  identity/svid/mode req : %lu/%lu/%lu\r\n",
                 (unsigned long)s->n_identity_req,
                 (unsigned long)s->n_svid_req,
                 (unsigned long)s->n_mode_req);
  APP_LOG_Printf("  PE rejected   : %lu\r\n", (unsigned long)s->n_rejected);
}

static unsigned argu(const char *s, unsigned dflt)
{
  unsigned v = dflt;

  if ((s != NULL) && (sscanf(s, "%u", &v) != 1))
  {
    return dflt;
  }
  return v;
}

static unsigned argh(const char *s, unsigned dflt)
{
  unsigned v = dflt;

  if ((s != NULL) && (sscanf(s, "%x", &v) != 1))
  {
    return dflt;
  }
  return v;
}

int APP_VDM_Cmd(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "status";
  uint8_t sop;
  uint16_t svid;
  uint8_t idx;
  int rc;

  if (strcmp(sub, "status") == 0)
  {
    print_state();
    return 1;
  }
  if (strcmp(sub, "clear") == 0)
  {
    APP_VDM_Clear();
    APP_LOG_Write("vdm counters cleared\r\n");
    return 1;
  }

  sop = (uint8_t)argu((argc >= 3) ? argv[2] : NULL, APP_VDM_SOP1);

  if (strcmp(sub, "discover") == 0)
  {
    /* Cable / E-marker re-discovery: Discover Identity on SOP' or SOP''. */
    rc = APP_VDM_RequestIdentity(0u, sop);
    APP_LOG_Printf("vdm: Discover_Identity on %s -> %s\r\n",
                   APP_VDM_SopName(sop),
                   (rc == APP_VDM_CALL_OK) ? "requested" : "REJECTED");
    return 1;
  }
  if (strcmp(sub, "svids") == 0)
  {
    rc = APP_VDM_RequestSVID(0u, sop);
    APP_LOG_Printf("vdm: Discover_SVIDs on %s -> %s\r\n",
                   APP_VDM_SopName(sop),
                   (rc == APP_VDM_CALL_OK) ? "requested" : "REJECTED");
    return 1;
  }
  if (strcmp(sub, "modes") == 0)
  {
    svid = (uint16_t)argh((argc >= 4) ? argv[3] : NULL, 0u);
    rc = APP_VDM_RequestMode(0u, sop, svid);
    APP_LOG_Printf("vdm: Discover_Modes svid=0x%04X -> %s\r\n",
                   (unsigned)svid,
                   (rc == APP_VDM_CALL_OK) ? "requested" : "REJECTED");
    return 1;
  }
  if (strcmp(sub, "enter") == 0)
  {
    svid = (uint16_t)argh((argc >= 4) ? argv[3] : NULL, 0u);
    idx = (uint8_t)argu((argc >= 5) ? argv[4] : NULL, 1u);
    rc = APP_VDM_ModeEnter(0u, sop, svid, idx);
    APP_LOG_Printf("vdm: Enter_Mode svid=0x%04X mode=%u -> %s\r\n",
                   (unsigned)svid, (unsigned)idx,
                   (rc == APP_VDM_CALL_OK) ? "requested" : "REJECTED");
    return 1;
  }
  if (strcmp(sub, "exit") == 0)
  {
    svid = (uint16_t)argh((argc >= 4) ? argv[3] : NULL, 0u);
    idx = (uint8_t)argu((argc >= 5) ? argv[4] : NULL, 0u);
    rc = APP_VDM_ModeExit(0u, sop, svid, idx);
    APP_LOG_Printf("vdm: Exit_Mode svid=0x%04X mode=%u -> %s\r\n",
                   (unsigned)svid, (unsigned)idx,
                   (rc == APP_VDM_CALL_OK) ? "requested" : "REJECTED");
    return 1;
  }

  APP_LOG_Write("usage: vdm status|clear|discover [sop]|svids [sop]\r\n");
  APP_LOG_Write("       vdm modes <svid_hex> [sop]\r\n");
  APP_LOG_Write("       vdm enter <svid_hex> <mode 1-6> [sop]\r\n");
  APP_LOG_Write("       vdm exit  <svid_hex> <mode 0-6> [sop]\r\n");
  APP_LOG_Write("  sop: 0=SOP 1=SOP' (default) 2=SOP''\r\n");
  return 1;
}
