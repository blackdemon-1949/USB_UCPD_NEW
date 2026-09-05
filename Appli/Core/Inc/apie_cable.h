/**
  ******************************************************************************
  * @file    apie_cable.h
  * @brief   Cable (SOP'/SOP'') intelligence + EPR/AVS protocol awareness.
  *
  * The source (SOP) and the cable plugs (SOP'/SOP'') are tracked as SEPARATE
  * entities. Cable facts come only from Discover Identity replies received on
  * SOP' (passive / e-marker) or SOP'' (active cable's far-end plug), and are
  * never attributed to the source. EPR/AVS structures are decoded so future
  * hardware can support extended power; on THIS board EPR power is gated by
  * APIE_HW_EPR_POWER_ENABLED (0) and is never energised until Phase 5.
  *
  * See PD_COMPATIBILITY.md for the protocol-support vs. hardware-capability
  * distinction.
  ******************************************************************************
  */
#ifndef APIE_CABLE_H
#define APIE_CABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"
#include <stdint.h>

/* Voltage tier for CableVDO / ActiveCableVDO b.CableMaxVoltage.
   Values match the USB PD spec encoding (0=20V, 1=30V, 2=40V, 3=50V). */
#define APIE_CABLE_MAX_V_20V   0U
#define APIE_CABLE_MAX_V_30V   1U
#define APIE_CABLE_MAX_V_40V   2U
#define APIE_CABLE_MAX_V_50V   3U

/* Convert the cable's 2-bit CableMaxVoltage field into a millivolt ceiling. */
uint16_t APIE_Cable_MaxMvFromField(uint8_t tier);

typedef struct
{
  uint8_t present;          /* 1 = a cable identity has been observed       */
  uint8_t sop;              /* USBPD_SOPTYPE_SOP1 (SOP') or _SOP2 (SOP'')   */
  uint8_t current_cap;      /* VBUS_CurrentHandCap: 0=def/1.5A 1=3A 2=5A    */
  uint8_t ss_cap;           /* Superspeed support (from ID header)          */
  uint8_t active;           /* 1 = ActiveCableVDO1 was returned             */
  uint8_t vconn;            /* VCONN-powered (from ID header)               */
  uint8_t epr_capable;      /* CableVDO / ActiveCableVDO1 EPR_Mode_Capable  */
  uint8_t max_voltage_tier; /* CableVDO/ActiveCableVDO1 CableMaxVoltage     */
  uint16_t vid;
  uint16_t pid;
  uint32_t hw_rev;
  uint32_t fw_rev;
} APIE_CableProfile_t;

typedef enum
{
  APIE_EPR_STATE_NOT_IN_EPR = 0,
  APIE_EPR_STATE_SRC_CAP,        /* source advertised EPR caps                */
  APIE_EPR_STATE_MODE_ENTRY,     /* EPR mode requested/entered                */
  APIE_EPR_STATE_MODE_ACTIVE
} APIE_EPR_State_t;

typedef struct
{
  uint8_t  epr_capable;          /* source supports EPR (saw AVS PDO)         */
  uint8_t  avs_present;
  uint16_t avs_min_mv;
  uint16_t avs_max_mv;
  uint8_t  state;                /* APIE_EPR_State_t                          */
  uint16_t epr_snk_pdp_w;        /* EPR sink operational PDP (watts)          */
  uint16_t current_voltage_mv;
} APIE_EPR_Info_t;

void APIE_Cable_Init(void);
void APIE_Cable_ResetSession(uint8_t conn_id);

/**
 * @brief  Feed a parsed Discover Identity reply into the cable tracker.
 *
 * This is called from USBPD_VDM_InformIdentity() for SOP' / SOP'' replies
 * (SOP replies describe the port-partner source and go to APIE_Profile_OnIdentity
 * instead). The struct is the ST USBPD library's parsed identity; we read the
 * VDO fields directly out of it rather than shifting raw bytes.
 *
 * @param sop          USBPD_SOPTYPE_SOP1 (SOP') or USBPD_SOPTYPE_SOP2 (SOP'').
 * @param id_header_v  ID Header VDO d32 (for product type / VCONN / current).
 * @param cert_v       Cert Stat VDO d32.
 * @param product_v    Product VDO d32.
 * @param passive_presence  1 if CableVDO (passive cable VDO) is valid.
 * @param cable_vdo    Passive Cable VDO d32 (only valid when passive_presence=1).
 * @param active_presence   1 if ActiveCableVDO1 is valid.
 * @param active_vdo1  Active Cable VDO1 d32 (only valid when active_presence=1).
 */
void APIE_Cable_OnIdentity(uint8_t sop,
                           uint32_t id_header_v,
                           uint32_t cert_v,
                           uint32_t product_v,
                           uint8_t  passive_presence,
                           uint32_t cable_vdo,
                           uint8_t  active_presence,
                           uint32_t active_vdo1);

void APIE_Cable_OnSopData(uint8_t sop, const uint8_t *payload, uint16_t len);
void APIE_EPR_OnSourceCaps(uint8_t port, const uint32_t *pdo, uint8_t n);
void APIE_EPR_OnModeChange(uint8_t new_state);
void APIE_EPR_OnAvs(const uint8_t *payload, uint16_t len);

const APIE_CableProfile_t *APIE_Cable_Get(void);
const APIE_EPR_Info_t *APIE_EPR_Get(void);

/**
 * @brief  Hard gate: is EPR power allowed right now?
 *
 * Returns 1 only if the compile-time hardware flag is set AND the cable
 * reports EPR-capable AND its CableMaxVoltage covers the requested ceiling.
 * Does NOT look at source caps — those are evaluated at request time.
 */
uint8_t APIE_EPR_PowerAllowed(void);

void APIE_Cable_Dump(void);
void APIE_EPR_Dump(void);
const char *APIE_SopName(uint8_t sop);

#ifdef __cplusplus
}
#endif

#endif /* APIE_CABLE_H */
