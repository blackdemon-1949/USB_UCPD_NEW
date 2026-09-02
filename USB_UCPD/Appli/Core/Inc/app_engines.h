/**
 * @file    app_engines.h
 * @brief   Compile-time enable switches for the advanced application engines.
 *
 * WHY THIS EXISTS
 * ---------------
 * The post-baseline firmware showed a hard-reset storm, USB CDC instability and
 * missing E-marker/EPR behaviour on real hardware.  Several engines were added
 * at once, so the culprit cannot be identified by reading code alone - it has
 * to be bisected on the board.
 *
 * Each switch isolates exactly one engine.  Set a switch to 0, rebuild, flash,
 * and observe.  The engines are independent, so any subset can be disabled.
 * With every switch at 0 the firmware reduces to the golden-baseline behaviour
 * plus the CLI: the ST PD stack, fixed PDO/PPS request engine, INA226 and the
 * original CLI are all untouched by these switches.
 *
 * BISECTION ORDER
 * ---------------
 * Start from the most invasive, because those are the ones that can actually
 * disturb the Policy Engine:
 *
 *   1. APP_ENG_CAPTURE    - replaces the ST trace entry point and runs on
 *                           every PD message.  Disabling it restores the stock
 *                           USBPD_TRACE_Add registration untouched.
 *   2. APP_ENG_CABLE_VDM  - installs vdmCallbacks.  Disabling it restores the
 *                           baseline, where no VDM callbacks were registered.
 *   3. APP_ENG_EPR        - answers the ST stack's EPR data requests and
 *                           advertises a sink AVS PDO.
 *   4. APP_ENG_TXN        - pure bookkeeping off the capture path.
 *   5. APP_ENG_ANALYTICS  - main-loop polling (power, temperature, txn poll).
 *   6. APP_ENG_DIAG       - counters only.
 *   7. APP_ENG_STORE      - backup-SRAM persistence init.
 *
 * Nothing here changes CubeMX-generated code or the ST middleware.
 */
#ifndef APP_ENGINES_H
#define APP_ENGINES_H

/* BENCH PROFILE: the requested feature set, and nothing that destabilises it.
 *
 * ON  - voltage requests, caps, PPS/AVS, EPR, cable/e-marker, VDM, DTS temp,
 *       diagnostics, and CAPTURE.
 *
 * CAPTURE must stay ON even though it is "analyzer": it owns the ST trace
 * funnel, and with it compiled out the PD TX/RX/GoodCRC counters and the
 * whole transaction view are structurally dead (the bench saw pd_rx/pd_tx
 * frozen at 0 while PD was plainly running).  It is a bounded RAM ring with
 * no console or I2C work in the callback, so it does not load the CDC path.
 *
 * OFF - TXN, EXT, ANALYTICS: enabling all three together brought the USB CDC
 *       instability back on the bench.  ANALYTICS in particular polls the
 *       INA226 over I2C and formats statistics from the super-loop, which is
 *       exactly the kind of background work that starves CDC servicing.  The
 *       'power'/'temp' data they provided is already available on demand from
 *       'ina' and 'temp', so nothing is lost.
 *       FUZZ/TEST are test-only, STORE writes backup SRAM.
 *
 * Re-enable any of them explicitly with -DAPP_ENG_xxx=1. */

/** RAM capture ring + takeover of the ST PD trace entry point. */
#ifndef APP_ENG_CAPTURE
#define APP_ENG_CAPTURE        1
#endif

/** Transaction reconstruction.  Requires APP_ENG_CAPTURE for live data. */
#ifndef APP_ENG_TXN
#define APP_ENG_TXN            0
#endif

/** Cable / E-marker engine and VDM callback registration. */
#ifndef APP_ENG_CABLE_VDM
#define APP_ENG_CABLE_VDM      1
#endif

/** EPR engine: sink AVS PDO, PDP and EPR notification handling. */
#ifndef APP_ENG_EPR
#define APP_ENG_EPR            1
#endif

/** VDM alternate-mode control (vdm command). */
#ifndef APP_ENG_VDM
#define APP_ENG_VDM            1
#endif

/** Diagnostic counters. */
#ifndef APP_ENG_DIAG
#define APP_ENG_DIAG           1
#endif

/** Main-loop periodic analytics: power sampling, DTS temperature, txn poll. */
#ifndef APP_ENG_ANALYTICS
#define APP_ENG_ANALYTICS      0
#endif

/** Backup-SRAM persistence init.  Never writes NOR. */
#ifndef APP_ENG_STORE
#define APP_ENG_STORE          0
#endif

/** Chunked extended-message reassembly.  Requires APP_ENG_CAPTURE. */
#ifndef APP_ENG_EXT
#define APP_ENG_EXT            0
#endif

/** Malformed-message (fuzz) engine.  Test-only; adds no background load. */
#ifndef APP_ENG_FUZZ
#define APP_ENG_FUZZ           0
#endif

/** On-target deterministic protocol test vectors.  Test-only. */
#ifndef APP_ENG_TEST
#define APP_ENG_TEST           0
#endif

/** One-line summary of what is compiled out, printed once at startup so the
 *  log on the bench says exactly which build is running. */
#define APP_ENG_SUMMARY \
  "cap=" APP_ENG_STR(APP_ENG_CAPTURE) " txn=" APP_ENG_STR(APP_ENG_TXN) \
  " cable=" APP_ENG_STR(APP_ENG_CABLE_VDM) " epr=" APP_ENG_STR(APP_ENG_EPR) \
  " vdm=" APP_ENG_STR(APP_ENG_VDM) " diag=" APP_ENG_STR(APP_ENG_DIAG) \
  " anal=" APP_ENG_STR(APP_ENG_ANALYTICS) " store=" APP_ENG_STR(APP_ENG_STORE)

#define APP_ENG_STR2(x) #x
#define APP_ENG_STR(x)  APP_ENG_STR2(x)

#endif /* APP_ENGINES_H */
