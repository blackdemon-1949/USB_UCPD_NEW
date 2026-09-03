/*
 * pdport_app.h - pdsink application glue for the board (PDEngine port).
 *
 * The pdsink object graph is owned by pdport_app.cpp (one C++ file, the
 * only new source the CubeIDE build adds for the switch-over).  This
 * header is the C seam the existing C application modules (app_pd.c,
 * app_epr.c, app_pps.c, app_cli.c, main.c) use on the pdsink path:
 *
 *   - pdport_init() / pdport_service(): init once before the main loop,
 *     then pump every loop pass (self-throttled to 1 ms), replacing the
 *     old USBPD_DPM_InitCore()/USBPD_DPM_Run() pair.
 *   - pdport_get_status(): truthful state snapshot (contract, EPR mode,
 *     raw source capabilities, counters) for the CLI/status tables.
 *   - pdport_request_*(): the request engine entry points (fixed PDO /
 *     any-voltage / PPS / EPR AVS) - the re-pointed "req"/"pps"/"epr"
 *     command bodies.
 *   - pdport_epr_*(): EPR mode control with explicit return codes, so a
 *     CLI never reports "accepted" for something that was refused.
 *   - pdport_set_event_cb(): optional event callback (cable attach,
 *     contract, EPR failures...) for notification-driven app modules.
 *
 * The pdsink DPM default policy (dpm.cpp) applies when nothing has been
 * triggered: first supported source PDO (vSafe5V) is requested.  Sink
 * capabilities and the EPR watt figure default to the pdsink profile
 * (5/9/12/15/20 V + PPS, EPR fixed 28 V at slot 8, AVS 15-28 V at slot
 * 9, 140 W) - see pdport_app.cpp for the exact table and the macros
 * that adjust it for the board's front-end rating.
 *
 * Threading: all functions run in the main-loop context (pdport_service
 * drives the stack); none of them are safe from IRQ context.
 */
#ifndef PDPORT_APP_H
#define PDPORT_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Init / pump                                                        */
/* ------------------------------------------------------------------ */

/*
 * Initialise the pdsink object graph (transport + driver + TC/PRL/PE/
 * DPM/Task).  Safe to call more than once (idempotent).
 * Returns 0 on success, -1 if the transport init failed.
 */
int pdport_init(void);

/*
 * Main-loop pump.  Call on every loop pass; it self-throttles to a 1 ms
 * cadence and drains IRQ events, exactly like the old USBPD_DPM_Run().
 */
void pdport_service(void);

/* ------------------------------------------------------------------ */
/* Events (optional notification hook)                                */
/* ------------------------------------------------------------------ */

/* Event ids delivered through the pdport event callback.  The values are
 * the pdsink MsgToDpm_* message ids themselves (messages.h msg_dpm_id),
 * so an event number can be cross-checked against the core enum. */
#define PDPORT_EV_STARTUP            50u
#define PDPORT_EV_TRANSIT_TO_DEFAULT 51u
#define PDPORT_EV_SRC_CAPS_RECEIVED  52u
#define PDPORT_EV_SELECT_CAP_DONE    53u
#define PDPORT_EV_SRC_DISABLED       54u
#define PDPORT_EV_ALERT              55u
#define PDPORT_EV_EPR_ENTRY_FAILED   56u
#define PDPORT_EV_SNK_READY          57u
#define PDPORT_EV_CABLE_ATTACHED     58u
#define PDPORT_EV_CABLE_DETACHED     59u
#define PDPORT_EV_HANDSHAKE_DONE     60u
#define PDPORT_EV_POWER_REJECTED     61u
#define PDPORT_EV_POWER_ACCEPTED     62u

typedef void (*pdport_event_cb_t)(uint32_t ev, void* arg);

/* Install (or clear with cb == NULL) the event notification hook. */
void pdport_set_event_cb(pdport_event_cb_t cb, void* arg);

/* ------------------------------------------------------------------ */
/* Status snapshot                                                    */
/* ------------------------------------------------------------------ */

#define PDPORT_MAX_SRC_PDOS 11u /* PD 3.1 EPR source cap objects */

typedef struct pdport_status {
    /* -- link / stack -- */
    uint32_t initialised;      /* pdport_init() done */
    uint32_t attached;         /* CC attached, stack link up */
    uint32_t active_cc;        /* 1 or 2; 0 = none */
    uint32_t active_cc_level;  /* PD_CC_* code (see pd_tr.h) */
    uint32_t vbus_ok;
    uint32_t pe_state;         /* numeric pdsink PE state id */
    uint32_t revision;         /* PD_REVISION::Type on the link: wire Spec
                                  Revision value.  2 = 10b = PD 3.x family
                                  (PD 3.0 AND 3.1 AND 3.2 share the 10b
                                  header value - see data_objects.h); 1 =
                                  PD 2.0; 0 = legacy.  EPR capability is
                                  not this field but epr_source_capable. */

    /* -- contract -- */
    uint32_t explicit_contract;   /* PE_FLAG::HAS_EXPLICIT_CONTRACT */
    uint32_t contract_position;   /* RDO object position, 1-based; 0 = none */
    uint32_t contract_mv;         /* decoded operating point */
    uint32_t contract_ma;
    uint32_t in_pps_contract;

    /* -- EPR -- */
    uint32_t in_epr_mode;         /* PE_FLAG::IN_EPR_MODE */
    uint32_t epr_auto_enter;      /* 1 = auto entry enabled (EPR allowed) */
    uint32_t epr_source_capable;  /* 5 V PDO1 "EPR Mode Capable" bit
                                  (PD 3.1+ source) + link on the PD 3.x
                                  family value */

    /* -- source capabilities (SPR list, replaced by the EPR list while
     *    in EPR mode; zero-padded slots stay zero) -- */
    uint32_t src_caps_count;
    uint32_t src_caps[PDPORT_MAX_SRC_PDOS];

    /* -- raw negotiated RDO (0 when no explicit contract) -- */
    uint32_t rdo_contracted;

    /* -- driver counters (cumulative since init) -- */
    uint32_t rx_frames;      /* non-GoodCRC frames handed to the stack */
    uint32_t rx_goodcrc;     /* GoodCRC frames received */
    uint32_t tx_frames;      /* data frames armed on the wire */
    uint32_t tx_succeeded;   /* GoodCRC-confirmed transmissions */
    uint32_t tx_failed;      /* watch timeouts (PRL retries) */
    uint32_t hr_sent;        /* hard reset bursts transmitted */
    uint32_t hr_rx;          /* hard resets received from the partner */
} pdport_status_t;

/* Fill *out with the current stack state.  out must not be NULL. */
void pdport_get_status(pdport_status_t* out);

/* ------------------------------------------------------------------ */
/* Request engine (re-pointed "req"/"pps"/"epr ..." command bodies)   */
/*                                                                     */
/* All requests return 0 when queued (the PE performs them on its own  */
/* schedule) and -1 when refused.  "Queued" is never "done": poll      */
/* pdport_get_status() / the event hook for the outcome.               */
/* ------------------------------------------------------------------ */

/*
 * Request the source PDO at `position` (1-based, as printed by the PDO
 * tables).  mv/ma may be 0 = take the profile maximum; non-zero values
 * are clamped to the PDO limits (fixed PDOs ignore mv).
 */
int pdport_request_position(uint32_t position, uint32_t mv, uint32_t ma);

/* Request the first PDO matching voltage mv (and optionally current ma). */
int pdport_request_any(uint32_t mv, uint32_t ma);

/* Request the PPS PDO at mv (mv must sit inside the offered PPS window). */
int pdport_request_pps(uint32_t mv, uint32_t ma);

/* Request the EPR AVS PDO at mv (inside the offered AVS window).  For
 * EPR AVS the PDO carries watts, not amps: ma is derived from the DPM's
 * watt figure and clamped to the PD 5 A limit. */
int pdport_request_epr_avs(uint32_t mv, uint32_t ma);

/* ------------------------------------------------------------------ */
/* EPR mode control                                                   */
/* ------------------------------------------------------------------ */

#define PDPORT_EPR_ENTER_QUEUED    0 /* entry requested; PE will proceed  */
#define PDPORT_EPR_ENTER_ALREADY   1 /* already in EPR mode, nothing done */
#define PDPORT_EPR_ENTER_REFUSED  -1 /* not allowed (no EPR source / no   */
                                     /* PD3 / no explicit contract)       */

/*
 * Ask the policy engine to enter EPR mode now.  Also lifts the auto-
 * entry latch that a failed entry attempt or a sink-initiated exit left
 * behind, so "epr enter" after "epr exit" works without a reset.
 */
int pdport_epr_enter(void);

#define PDPORT_EPR_EXIT_QUEUED     0 /* EPR_Mode(Exit) request queued     */
#define PDPORT_EPR_EXIT_NOT_ACTIVE 1 /* not in EPR mode, nothing to exit  */
#define PDPORT_EPR_EXIT_SPR_FIRST  2 /* SPR PDO request queued first: per */
                                     /* PD 3.1 the Sink must hold an SPR  */
                                     /* contract before EPR_Mode(Exit);   */
                                     /* call again once the status shows  */
                                     /* contract_position <= 7            */
#define PDPORT_EPR_EXIT_FAILED    -1 /* refused / no SPR PDO available    */

/*
 * Sink-initiated EPR mode exit.  Two-step when needed: if the current
 * contract is EPR-level (position > 7) an SPR PDO (first fixed PDO of
 * the current list, else PDO 1) is requested first and PDPORT_EPR_EXIT_
 * SPR_FIRST is returned; the caller repeats the call after the SPR
 * contract is in place to actually send EPR_Mode(Exit).
 */
int pdport_epr_exit(void);

/* Enable (1) / disable (0) automatic EPR mode entry.  Only affects
 * future entries; an active EPR contract is left untouched. */
int pdport_epr_auto(int enable);

#ifdef __cplusplus
}
#endif

#endif /* PDPORT_APP_H */
