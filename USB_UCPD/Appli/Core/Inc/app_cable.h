/**
 * @file    app_cable.h
 * @brief   Cable / E-marker engine: Discover Identity and cable VDO decoding.
 *
 * Pure and hardware-free so it can be unit tested on the host.  Every bit
 * position and every enumerated value is taken from ST's usbpd_def.h
 * (USBPD_IDHeaderVDO_TypeDef, USBPD_CableVdo_TypeDef,
 * USBPD_ActiveCableVdo1_TypeDef and the CABLE_* / VBUS_* / USB*_GEN* defines),
 * which matches USB PD Revision 3.1.
 *
 * SOP' / SOP'' traffic reaches this module as captured frames; the module only
 * interprets bytes, it never sends anything itself.
 */
#ifndef APP_CABLE_H
#define APP_CABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Cable VDO (passive) field positions, LSB first, per USBPD_CableVdo_TypeDef */
#define APP_CBL_SS_SUPPORT(v)      ((uint8_t)((v) & 0x7u))                /* B2..0   */
#define APP_CBL_CURRENT_CAP(v)     ((uint8_t)(((v) >> 5) & 0x3u))         /* B6..5   */
#define APP_CBL_MAX_VBUS(v)        ((uint8_t)(((v) >> 9) & 0x3u))         /* B10..9  */
#define APP_CBL_TERM_TYPE(v)       ((uint8_t)(((v) >> 11) & 0x3u))        /* B12..11 */
#define APP_CBL_LATENCY(v)         ((uint8_t)(((v) >> 13) & 0xFu))        /* B16..13 */
#define APP_CBL_EPR_CAPABLE(v)     ((uint8_t)(((v) >> 17) & 0x1u))        /* B17     */
#define APP_CBL_TO_TYPE(v)         ((uint8_t)(((v) >> 18) & 0x3u))        /* B19..18 */
#define APP_CBL_FW_VER(v)          ((uint8_t)(((v) >> 22) & 0xFu))        /* B25..22 */
#define APP_CBL_HW_VER(v)          ((uint8_t)(((v) >> 26) & 0xFu))        /* B29..26 */
#define APP_CBL_MODE(v)            ((uint8_t)(((v) >> 30) & 0x3u))        /* B31..30 */

/* Active cable VDO1 differences, per USBPD_ActiveCableVdo1_TypeDef */
#define APP_CBL_ACT_SOP2_PRESENT(v) ((uint8_t)(((v) >> 3) & 0x1u))        /* B3      */
#define APP_CBL_ACT_VBUS_THRU(v)    ((uint8_t)(((v) >> 4) & 0x1u))        /* B4      */
#define APP_CBL_ACT_SBU_TYPE(v)     ((uint8_t)(((v) >> 7) & 0x1u))        /* B7      */
#define APP_CBL_ACT_SBU_SUPPORT(v)  ((uint8_t)(((v) >> 8) & 0x1u))        /* B8      */

/* Enumerated values, verbatim from ST's usbpd_def.h */
#define APP_CBL_CUR_DEFAULT   0u   /* VBUS_DEFAULT   */
#define APP_CBL_CUR_3A        1u   /* VBUS_3A        */
#define APP_CBL_CUR_5A        2u   /* VBUS_5A        */

#define APP_CBL_SS_USB2       0u   /* USB2P0_ONLY      */
#define APP_CBL_SS_GEN1       1u   /* USB3P2_GEN1      */
#define APP_CBL_SS_GEN2       2u   /* USB3P2_USB4_GEN2 */
#define APP_CBL_SS_GEN3       3u   /* USB4_GEN3        */

#define APP_CBL_VBUS_20V      0u   /* VBUS_MAX_20V */
#define APP_CBL_VBUS_30V      1u   /* VBUS_MAX_30V */
#define APP_CBL_VBUS_40V      2u   /* VBUS_MAX_40V */
#define APP_CBL_VBUS_50V      3u   /* VBUS_MAX_50V */

#define APP_CBL_TERM_PASSIVE_NOVCONN 0u  /* CABLE_TERM_BOTH_PASSIVE_NO_VCONN */
#define APP_CBL_TERM_PASSIVE_VCONN   1u  /* CABLE_TERM_BOTH_PASSIVE_VCONN    */
#define APP_CBL_TERM_ONE_EACH        2u  /* CABLE_TERM_ONE_EACH_VCONN        */
#define APP_CBL_TERM_BOTH_ACTIVE     3u  /* CABLE_TERM_BOTH_ACTIVE_VCONN     */

#define APP_CBL_TO_A          0u   /* CABLE_TO_TYPE_A */
#define APP_CBL_TO_B          1u   /* CABLE_TO_TYPE_B */
#define APP_CBL_TO_C          2u   /* CABLE_TO_TYPE_C */
#define APP_CBL_TO_CAPTIVE    3u   /* CABLE_CAPTIVE   */

/* Discover Identity: product type in the ID header VDO */
#define APP_CBL_PT_UNDEFINED       0u
#define APP_CBL_PT_HUB             1u
#define APP_CBL_PT_PERIPHERAL      2u
#define APP_CBL_PT_PSD             3u   /* Portable Power Source Device  */
#define APP_CBL_PT_AMA             4u   /* Alternate Mode Adapter        */
#define APP_CBL_PT_ACTIVE_CABLE    5u
#define APP_CBL_PT_PASSIVE_CABLE   6u
#define APP_CBL_PT_VPD             7u   /* VCONN Powered Device          */

typedef struct
{
  uint32_t id_header;      /* VDO1 */
  uint32_t cert_stat;      /* VDO2 */
  uint32_t product;        /* VDO3 */
  uint32_t cable_vdo;      /* VDO4 for cables, product-type VDO otherwise */
  uint8_t  vdo_count;
  uint8_t  vid_valid;
  uint8_t  active;         /* 1 when the product type says active cable */
  uint8_t  passive;
} APP_CBL_Identity_t;

typedef struct
{
  uint8_t  valid;
  uint8_t  active;
  uint8_t  ss_support;     /* APP_CBL_SS_*                              */
  uint8_t  current_cap;    /* APP_CBL_CUR_*                             */
  uint8_t  max_vbus;       /* APP_CBL_VBUS_*                            */
  uint8_t  term_type;      /* APP_CBL_TERM_*                            */
  uint8_t  latency;        /* raw CABLE_LATENCY_* value                 */
  uint8_t  epr_capable;
  uint8_t  to_type;        /* APP_CBL_TO_*                              */
  uint8_t  fw_ver;
  uint8_t  hw_ver;
  uint8_t  sop2_present;   /* active cable only                         */
  uint8_t  vbus_through;   /* active cable only                         */
  uint8_t  sbu_support;    /* active cable only                         */
  uint16_t vid;
  uint32_t pid;
} APP_CBL_Info_t;

/* Compatibility verdicts for a requested operating point */
#define APP_CBL_OK            0u
#define APP_CBL_NO_CABLE      1u   /* no identity discovered yet        */
#define APP_CBL_VOLT_LIMIT    2u   /* cable cannot carry this voltage   */
#define APP_CBL_CURR_LIMIT    3u   /* cable cannot carry this current   */
#define APP_CBL_NOT_EPR       4u   /* EPR asked, cable is not EPR rated */

/* Pure API */
void        APP_CBL_DecodeIdentity(const uint32_t *vdo, uint8_t count,
                                   APP_CBL_Identity_t *out);
void        APP_CBL_DecodeVdo(uint32_t vdo, uint8_t active, APP_CBL_Info_t *out);
int         APP_CBL_DecodeDiscoverIdentityAck(const uint8_t *payload,
                                              uint16_t len,
                                              APP_CBL_Info_t *info);
uint32_t    APP_CBL_MaxVoltageMv(uint8_t max_vbus);
uint32_t    APP_CBL_MaxCurrentMa(uint8_t current_cap);

/**
 * Check whether a cable can carry an operating point.
 * @return APP_CBL_* verdict.
 */
uint8_t APP_CBL_Check(const APP_CBL_Info_t *info, uint32_t mv, uint32_t ma,
                      uint8_t want_epr);

/* Formatting - bounded, always NUL terminated */
void APP_CBL_FormatInfo(const APP_CBL_Info_t *info, char *out, size_t outsz);
const char *APP_CBL_SsName(uint8_t ss);
const char *APP_CBL_TermName(uint8_t term);
const char *APP_CBL_ToTypeName(uint8_t to);
const char *APP_CBL_ProductTypeName(uint8_t pt);
const char *APP_CBL_VerdictName(uint8_t verdict);

/* ------------------------------------------------------------------ */
/* Target glue                                                         */
/*                                                                     */
/* Fed from USBPD_VDM_InformIdentity(), the real ST DPM/VDM callback    */
/* that delivers a Discover Identity response.  The pointer is typed as */
/* void* here so that this header stays free of ST includes and the     */
/* pure decode API above remains host-testable; app_cable.c casts it to */
/* USBPD_DiscoveryIdentity_TypeDef*.                                   */
/* ------------------------------------------------------------------ */

/**
 * Register the ST VDM callbacks for @p port.
 *
 * USBPD_PE_Init() does not take the VDM callback table, and nothing else in
 * this project ever called USBPD_PE_InitVDM_Callback() - so vdmCallbacks in
 * usbpd_vdm_user.c was defined but never installed, and the whole VDM chain
 * (including USBPD_VDM_InformIdentity, the only source of live cable
 * identity) was dead code that --gc-sections removed.  Registering it here,
 * from the application layer, keeps the ST middleware and the generated
 * usbpd_dpm_core.c untouched.  Must run after MX_USBPD_Init().
 * @return 1 when the PE accepted the table.
 */
int APP_CBL_RegisterVdm(uint8_t port);

/** Record a live Discover Identity response.  @p identity may be NULL. */
void APP_CBL_OnIdentity(const void *identity, uint8_t ok);

/** True once a real cable/product identity has been decoded. */
uint8_t APP_CBL_IsLive(void);

/** Decoded identity of the attached cable.  valid=0 when none seen yet. */
const APP_CBL_Info_t *APP_CBL_GetLive(void);

/** Re-evaluate the cable verdict against a proposed operating point. */
uint8_t APP_CBL_Evaluate(uint32_t mv, uint32_t ma, uint8_t want_epr);

/** `cable` CLI command. */
int APP_CBL_LiveCmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_CABLE_H */
