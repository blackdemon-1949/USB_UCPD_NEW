/**
 * @file    app_pdcap.h
 * @brief   Bridge between the ST USB-PD trace funnel and the capture ring.
 *
 * The ST library calls a single registered function pointer for every trace
 * event:
 *
 *     extern TRACE_ENTRY_POINT USBPD_Trace;      // usbpd_trace.h
 *     void USBPD_PE_SetTrace(TRACE_ENTRY_POINT, uint8_t);
 *
 * usbpd_trace.c registers USBPD_TRACE_Add() with it.  APP_PDCAP_Init()
 * re-registers APP_PDCAP_Trace() *after* USBPD_TRACE_Init() has run, so every
 * event is recorded in the RAM ring and then forwarded unchanged to the
 * existing TRACER_EMB path.  No ST source or library is modified.
 */
#ifndef APP_PDCAP_H
#define APP_PDCAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "usbpd_core.h"

/** Enable the DWT cycle counter and take over the trace entry point.
 *  Must be called AFTER USBPD_DPM_InitCore() (which runs USBPD_TRACE_Init). */
void APP_PDCAP_Init(void);

/** TRACE_ENTRY_POINT implementation: record, then forward. */
void APP_PDCAP_Trace(TRACE_EVENT type, uint8_t port, uint8_t sop,
                     uint8_t *ptr, uint32_t size);

/** Raw cycle counter, used as the capture timestamp. */
uint32_t APP_PDCAP_Cycles(void);

/** Convert a cycle delta to microseconds using the running core clock. */
uint32_t APP_PDCAP_CyclesToUs(uint32_t cycles);

/** Core clock in Hz, as reported by CMSIS SystemCoreClock. */
uint32_t APP_PDCAP_CoreHz(void);

/** `cap` CLI command: statistics, decoded listing, raw hex and JSONL. */
int APP_PDCAP_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_PDCAP_H */
