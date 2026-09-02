/**
 * @file    app_pps.h
 * @brief   PPS (Programmable Power Supply) analysis and request validation.
 *
 * The Request itself is produced by the existing, hardware-proven path in
 * app_pd.c.  This engine adds the analysis layer on top of it:
 *
 *   - enumeration of every PPS window a source advertises,
 *   - coverage analysis: which of the advertised windows overlap,
 *   - validation of an operating point against a window before a Request is
 *     built, so an out-of-range request is rejected here rather than by the
 *     source,
 *   - construction of a Programmable Power Supply RDO for the test engine,
 *     which needs to synthesize requests rather than only accept them.
 *
 * Field positions are the ones already encoded in app_dec.h, which were taken
 * from USBPD_ProgrammablePowerSupplyAPDO_TypeDef in usbpd_def.h.
 */
#ifndef APP_PPS_H
#define APP_PPS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Programmable Power Supply RDO layout (PD 3.0, Table 6-26) */
#define APP_PPS_RDO_POS(r)        ((uint8_t)(((r) >> 28) & 0xFu))
#define APP_PPS_RDO_UNCHUNKED(r)  ((uint8_t)(((r) >> 27) & 0x1u))
#define APP_PPS_RDO_USB_COMM(r)   ((uint8_t)(((r) >> 26) & 0x1u))
#define APP_PPS_RDO_VOLT(r)       ((uint32_t)(((r) >> 9) & 0xFFFu) * 20u)  /* mV */
#define APP_PPS_RDO_CURR(r)       ((uint32_t)((r) & 0x7Fu) * 50u)          /* mA */

/* Validation verdicts */
#define APP_PPS_OK          0u
#define APP_PPS_NO_WINDOW   1u   /* no PPS APDO advertised                 */
#define APP_PPS_BELOW_MIN   2u   /* voltage under the window floor         */
#define APP_PPS_ABOVE_MAX   3u   /* voltage over the window ceiling        */
#define APP_PPS_OVER_CURR   4u   /* current over the window limit          */
#define APP_PPS_POWER_LIMIT 5u   /* product of V and I exceeds the APDO PDP */

#define APP_PPS_MAX_WINDOWS 7u   /* USBPD_MAX_NB_PDO */

typedef struct
{
  uint8_t  pos;          /* 1-based object position in the source caps    */
  uint32_t min_mv;
  uint32_t max_mv;
  uint32_t max_ma;
  uint8_t  power_limited;
} APP_PPS_Window_t;

typedef struct
{
  uint8_t          n;
  APP_PPS_Window_t w[APP_PPS_MAX_WINDOWS];
  uint32_t         span_min_mv;   /* widest reachable window across all   */
  uint32_t         span_max_mv;
  uint32_t         span_max_ma;
  uint32_t         max_pdp_mw;    /* highest V*I any window can deliver   */
} APP_PPS_Set_t;

/* --- pure API (host testable) ------------------------------------- */

/** Is @p pdo a PPS APDO (object type 11b, subtype 00b)? */
int APP_PPS_IsApdo(uint32_t pdo);

/** Parse one PPS APDO.  @return 1 on success, 0 when not a PPS APDO. */
int APP_PPS_Parse(uint32_t pdo, uint8_t pos, APP_PPS_Window_t *out);

/** Enumerate every PPS window in a source capability list. */
void APP_PPS_Analyse(const uint32_t *pdo, uint8_t count, APP_PPS_Set_t *out);

/**
 * Validate an operating point against a window.
 * @return one of APP_PPS_OK / APP_PPS_*.
 */
uint8_t APP_PPS_Validate(const APP_PPS_Window_t *w, uint32_t mv, uint32_t ma);

/**
 * Build a Programmable Power Supply RDO.
 * @return the RDO, or 0 on invalid input.
 */
uint32_t APP_PPS_BuildRdo(uint8_t pos, uint32_t mv, uint32_t ma,
                          uint8_t unchunked, uint8_t usb_comm);

const char *APP_PPS_VerdictName(uint8_t verdict);
void APP_PPS_FormatWindow(const APP_PPS_Window_t *w, char *out, size_t outsz);

/* --- target glue --------------------------------------------------- */

/** Fed from APP_PD_StoreSrcPDO, the real source-capability path. */
void APP_PPS_OnSrcPdo(const uint8_t *ptr, uint32_t size);

/** The set parsed from the most recent source capabilities. */
const APP_PPS_Set_t *APP_PPS_Get(void);

int APP_PPS_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_PPS_H */
