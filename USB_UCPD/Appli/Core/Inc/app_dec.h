/**
 * @file    app_dec.h
 * @brief   USB Power Delivery message decoder - pure, side-effect free.
 *
 * Design rules
 * ------------
 *  - This module contains NO HAL, NO ST middleware and NO hardware access.
 *    It only transforms bytes into fields and text.  That keeps it usable
 *    from the PD trace hook, from the CLI and from the host-side unit tests
 *    (tools/hosttest), which compile the very same .c file on x86.
 *  - Bit layouts follow USB PD Revision 3.1 and are cross-checked at build
 *    time against the bitfield structures in ST's usbpd_def.h
 *    (see tools/hosttest and app_dec_xcheck.c).
 *  - Nothing here allocates, blocks or prints.
 */
#ifndef APP_DEC_H
#define APP_DEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------------ */
/* PD Message Header, 16 bits, transmitted least significant byte first.     */
/*                                                                           */
/*   B4..0     Message Type                                                  */
/*   B5        Port Data Role   0 = UFP        1 = DFP                       */
/*   B7..6     Specification Revision   00=1.0  01=2.0  10=3.0               */
/*   B8        Port Power Role  0 = Sink       1 = Source                    */
/*   B11..9    Message ID                                                    */
/*   B14..12   Number of Data Objects                                        */
/*   B15       Extended         0 = standard   1 = extended                  */
/*                                                                           */
/* Verified against the USB-IF "USB Power Delivery Compliance Test           */
/* Specification" (Bit 4..0 Message Type, Bit 5 Port Data Role, Bit 7..6     */
/* Specification Revision, Bit 8 Port Power Role, Bits 11..9 MessageID,      */
/* Bits 14..12 Number of Data Objects, Bit 15 Extended).  Note PD 2.0 uses   */
/* only Bit 3..0 for the Message Type; PD 3.0 widens it to Bit 4..0.         */
/* ------------------------------------------------------------------------ */
#define APP_DEC_HDR_TYPE(h)        ((uint8_t)((h) & 0x001Fu))
#define APP_DEC_HDR_DATA_ROLE(h)   ((uint8_t)(((h) >> 5) & 0x0001u))
#define APP_DEC_HDR_SPEC_REV(h)    ((uint8_t)(((h) >> 6) & 0x0003u))
#define APP_DEC_HDR_POWER_ROLE(h)  ((uint8_t)(((h) >> 8) & 0x0001u))
#define APP_DEC_HDR_MSG_ID(h)      ((uint8_t)(((h) >> 9) & 0x0007u))
#define APP_DEC_HDR_NUM_OBJ(h)     ((uint8_t)(((h) >> 12) & 0x0007u))
#define APP_DEC_HDR_EXTENDED(h)    ((uint8_t)(((h) >> 15) & 0x0001u))

/* Extended Message Header, 16 bits, immediately follows the PD header.
 *   B8..0    Data Size
 *   B9       Reserved
 *   B10      Request Chunk
 *   B14..11  Chunk Number
 *   B15      Chunked                                                      */
#define APP_DEC_EXTHDR_SIZE(h)     ((uint16_t)((h) & 0x01FFu))
#define APP_DEC_EXTHDR_REQCHUNK(h) ((uint8_t)(((h) >> 10) & 0x0001u))
#define APP_DEC_EXTHDR_CHUNK(h)    ((uint8_t)(((h) >> 11) & 0x000Fu))
#define APP_DEC_EXTHDR_CHUNKED(h)  ((uint8_t)(((h) >> 15) & 0x0001u))

/* ------------------------------------------------------------------------ */
/* Power Data Objects.  Layouts match USBPD_SRCFixedSupplyPDO_TypeDef,        */
/* USBPD_SRCVariableSupplyPDO_TypeDef and USBPD_SRCBatterySupplyPDO_TypeDef   */
/* in Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_def.h.                */
/* ------------------------------------------------------------------------ */
#define APP_DEC_PDO_KIND(p)          ((uint8_t)(((p) >> 30) & 0x3u))
#define APP_DEC_PDO_FIXED            0u   /* 00b Fixed Supply              */
#define APP_DEC_PDO_BATTERY          1u   /* 01b Battery                   */
#define APP_DEC_PDO_VARIABLE         2u   /* 10b Variable Supply           */
#define APP_DEC_PDO_APDO             3u   /* 11b Augmented (PPS / AVS)     */

#define APP_DEC_PDO_FIXED_CURR(p)    ((uint32_t)((p) & 0x3FFu))          /* 10 mA   */
#define APP_DEC_PDO_FIXED_VOLT(p)    ((uint32_t)(((p) >> 10) & 0x3FFu))  /* 50 mV   */
#define APP_DEC_PDO_FIXED_PEAK(p)    ((uint8_t)(((p) >> 20) & 0x3u))
#define APP_DEC_PDO_FIXED_EPR(p)     ((uint8_t)(((p) >> 23) & 0x1u))
#define APP_DEC_PDO_FIXED_UNCHUNK(p) ((uint8_t)(((p) >> 24) & 0x1u))
#define APP_DEC_PDO_FIXED_DRD(p)     ((uint8_t)(((p) >> 25) & 0x1u))
#define APP_DEC_PDO_FIXED_USBCOMM(p) ((uint8_t)(((p) >> 26) & 0x1u))
#define APP_DEC_PDO_FIXED_EXTPWR(p)  ((uint8_t)(((p) >> 27) & 0x1u))
#define APP_DEC_PDO_FIXED_SUSPEND(p) ((uint8_t)(((p) >> 28) & 0x1u))
#define APP_DEC_PDO_FIXED_DRP(p)     ((uint8_t)(((p) >> 29) & 0x1u))

#define APP_DEC_PDO_VAR_MINVOLT(p)   ((uint32_t)(((p) >> 10) & 0x3FFu))  /* 50 mV   */
#define APP_DEC_PDO_VAR_MAXVOLT(p)   ((uint32_t)(((p) >> 20) & 0x3FFu))  /* 50 mV   */
#define APP_DEC_PDO_BATT_MAXPWR(p)   ((uint32_t)((p) & 0x3FFu))          /* 250 mW  */

/* Augmented PDO (APDO) sub-type lives in B29..28 */
#define APP_DEC_APDO_SUBTYPE(p)      ((uint8_t)(((p) >> 28) & 0x3u))
#define APP_DEC_APDO_PPS             0u   /* 00b Programmable Power Supply */
#define APP_DEC_APDO_AVSPDO          1u   /* 01b EPR Adjustable Voltage    */
#define APP_DEC_APDO_SPR_PPS         0u
#define APP_DEC_APDO_EPR_AVS         1u   /* 11b AVSPDO per PD3.1          */

/* PPS APDO: B24..17 max voltage 100 mV, B15..8 min voltage 100 mV,
 *           B6..0 max current 50 mA                                      */
#define APP_DEC_APDO_PPS_MAXCURR(p)  ((uint32_t)((p) & 0x7Fu))           /* 50 mA   */
#define APP_DEC_APDO_PPS_MINVOLT(p)  ((uint32_t)(((p) >> 8) & 0xFFu))    /* 100 mV  */
#define APP_DEC_APDO_PPS_MAXVOLT(p)  ((uint32_t)(((p) >> 17) & 0xFFu))   /* 100 mV  */
#define APP_DEC_APDO_PPS_PPS_POWER_LIMITED(p) ((uint8_t)(((p) >> 27) & 0x1u))

/* ------------------------------------------------------------------------ */
/* Request Data Object.  Layout matches USBPD_CORE_RDO_ReqFixedVariablePDO_   */
/* TypeDef / ..._ReqBatteryPDO_TypeDef in usbpd_def.h.                        */
/*   B31..28 Object Position, B27 Battery, B26 Unchunked, B25 GiveBack,        */
/*   B24 No USB Suspend, B23 USB Communications, B22 Capability Mismatch,     */
/*   B21 EPR Mode Capable                                                     */
/*   B19..10 Operating Current/Power, B9..0 Max Operating Current/Power       */
/* ------------------------------------------------------------------------ */
#define APP_DEC_RDO_POS(r)           ((uint8_t)(((r) >> 28) & 0xFu))
#define APP_DEC_RDO_BATTERY(r)       ((uint8_t)(((r) >> 27) & 0x1u))
#define APP_DEC_RDO_UNCHUNKED(r)     ((uint8_t)(((r) >> 26) & 0x1u))
#define APP_DEC_RDO_NO_SUSPEND(r)    ((uint8_t)(((r) >> 24) & 0x1u))
#define APP_DEC_RDO_USB_COMM(r)      ((uint8_t)(((r) >> 23) & 0x1u))
#define APP_DEC_RDO_CAP_MISMATCH(r)  ((uint8_t)(((r) >> 22) & 0x1u))
#define APP_DEC_RDO_GIVEBACK(r)      ((uint8_t)(((r) >> 25) & 0x1u))
#define APP_DEC_RDO_EPR_MODE(r)      ((uint8_t)(((r) >> 21) & 0x1u))
#define APP_DEC_RDO_OP_CURR(r)       ((uint32_t)(((r) >> 10) & 0x3FFu))  /* 10 mA   */
#define APP_DEC_RDO_MAX_CURR(r)      ((uint32_t)((r) & 0x3FFu))          /* 10 mA   */
#define APP_DEC_RDO_OP_PWR(r)        ((uint32_t)(((r) >> 10) & 0x3FFu))  /* 250 mW  */
#define APP_DEC_RDO_MAX_PWR(r)       ((uint32_t)((r) & 0x3FFu))          /* 250 mW  */

/* ------------------------------------------------------------------------ */
/* Decoded message                                                          */
/* ------------------------------------------------------------------------ */
typedef enum
{
  APP_DEC_CLASS_CONTROL  = 0,   /* no data objects                          */
  APP_DEC_CLASS_DATA,           /* standard data message                    */
  APP_DEC_CLASS_EXTENDED,       /* extended (possibly chunked)              */
  APP_DEC_CLASS_INVALID         /* header/payload inconsistent              */
} APP_DEC_MsgClass_t;

/* Reasons a frame may be flagged as malformed.  Bitmask. */
#define APP_DEC_F_SHORT        (1u << 0)  /* fewer than 2 header bytes      */
#define APP_DEC_F_NUMOBJ       (1u << 1)  /* NDO * 4 bytes missing          */
#define APP_DEC_F_EXT_SHORT    (1u << 2)  /* extended header missing        */
#define APP_DEC_F_EXT_SIZE     (1u << 3)  /* extended data size > payload   */
#define APP_DEC_F_CTRL_DO      (1u << 4)  /* control message carrying objects*/
#define APP_DEC_F_DATA_NODO    (1u << 5)  /* data message without objects   */
#define APP_DEC_F_TYPE_RSV     (1u << 6)  /* reserved message type          */
#define APP_DEC_F_TRUNCATED    (1u << 7)  /* payload clipped by the capture */

typedef struct
{
  uint16_t            raw_header;
  uint8_t             msg_type;
  uint8_t             num_obj;
  uint8_t             msg_id;
  uint8_t             spec_rev;
  uint8_t             power_role;   /* 0 = Sink, 1 = Source               */
  uint8_t             data_role;    /* 0 = UFP,  1 = DFP                  */
  uint8_t             extended;
  uint8_t             msg_class;    /* APP_DEC_MsgClass_t                  */
  uint8_t             flags;        /* APP_DEC_F_*                         */

  /* valid when extended != 0 */
  uint16_t            ext_header;
  uint16_t            ext_data_size;
  uint8_t             ext_chunked;
  uint8_t             ext_chunk_num;
  uint8_t             ext_req_chunk;

  /* offset of the data objects inside the buffer (2, or 4 if extended) */
  uint16_t            data_offset;
  uint8_t             data_len;     /* bytes actually present              */
  const uint8_t      *data;         /* points into the caller's buffer     */
} APP_DEC_Msg_t;

/* ------------------------------------------------------------------------ */
/* API - every function is pure: no I/O, no globals, no allocation.          */
/* ------------------------------------------------------------------------ */

/**
 * Decode a raw PD frame (header + optional extended header + objects).
 *
 * @param msg  frame bytes, header first
 * @param len  number of bytes available
 * @param out  decoded result; out->data points into @p msg
 * @return 0 on success, -1 when @p out could not be filled at all.
 *         Structural problems are reported through out->flags instead of a
 *         failure, because an analyzer must still show a malformed frame.
 */
int APP_DEC_Decode(const uint8_t *msg, uint16_t len, APP_DEC_Msg_t *out);

/** Number of bytes this frame needs to be complete, 0 if unknown. */
uint16_t APP_DEC_FrameSize(const uint8_t *msg, uint16_t len);

/* Naming.  All return pointers to static strings - never free them. */
const char *APP_DEC_SopName(uint8_t sop);
const char *APP_DEC_ClassName(uint8_t msg_class);
const char *APP_DEC_MsgName(const APP_DEC_Msg_t *m);
const char *APP_DEC_ControlName(uint8_t type);
const char *APP_DEC_DataName(uint8_t type);
const char *APP_DEC_ExtendedName(uint8_t type);
const char *APP_DEC_SpecRevName(uint8_t rev);

/* Formatting.  All are bounded by @p outsz and always NUL-terminate. */
void APP_DEC_FormatPdo(uint32_t pdo, char *out, size_t outsz);
void APP_DEC_FormatRdo(uint32_t rdo, char *out, size_t outsz);
void APP_DEC_FormatFlags(uint8_t flags, char *out, size_t outsz);
/** One-line human/machine readable rendering of a whole frame. */
void APP_DEC_FormatFrame(const uint8_t *msg, uint16_t len, char *out, size_t outsz);

/**
 * Extract the voltage/current a Fixed Supply PDO offers.
 * @return 1 on success, 0 when @p pdo is not a Fixed Supply PDO.
 */
int APP_DEC_PdoFixedToMvMa(uint32_t pdo, uint32_t *mv, uint32_t *ma);
/**
 * Extract the window of a PPS APDO.
 * @return 1 on success, 0 when @p pdo is not a PPS APDO.
 */
int APP_DEC_PdoPpsToRange(uint32_t pdo, uint32_t *min_mv, uint32_t *max_mv,
                          uint32_t *max_ma);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEC_H */
