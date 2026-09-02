/**
 * @file    app_integ.h
 * @brief   Hardware CRC / HASH / RNG used for real integrity work.
 *
 * These three peripherals are enabled by CubeMX and are exercised here, not
 * left idle:
 *
 *   CRC  - per-record integrity for the capture ring and the persistence
 *          store, so a corrupted record is detected instead of trusted.
 *   HASH - SHA-256 session fingerprint.  Two capture sessions that hash equal
 *          carried byte-identical traffic, which is what makes the replay and
 *          regression comparisons meaningful.
 *   RNG  - the jitter and packet-mutation source for the test engine, so that
 *          "repeat the test with a different seed" is actually possible.
 *
 * All three are polled synchronously from task context.  Nothing here may be
 * called from a PD or USB ISR.
 */
#ifndef APP_INTEG_H
#define APP_INTEG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define APP_HASH_SHA256_LEN 32u

/** True when all three peripherals are usable. */
uint8_t APP_INTEG_Ready(void);

/* --- CRC ---------------------------------------------------------------- */

/**
 * Hardware CRC-32 over a byte buffer.  The input is staged through an aligned
 * word buffer, so the caller does not need to pad or align anything.
 * @return the CRC, or 0 when the peripheral is unavailable.
 */
uint32_t APP_CRC_Calc(const uint8_t *data, uint32_t len);

/* --- HASH --------------------------------------------------------------- */

/**
 * SHA-256 of a byte buffer into @p out (32 bytes).
 * @return 1 on success, 0 on failure.
 */
int APP_HASH_Sha256(const uint8_t *data, uint32_t len, uint8_t *out);

/**
 * Fingerprint of the current session: build identity, capture statistics and
 * the negotiation counters, hashed.  Two identical fingerprints mean the
 * instrument observed the same traffic.
 */
int APP_HASH_SessionId(uint8_t out[APP_HASH_SHA256_LEN]);

/* --- RNG ---------------------------------------------------------------- */

/** One hardware random word, or 0 when the peripheral is unavailable. */
uint32_t APP_RNG_U32(void);

/** Bounded random value in [0, bound).  Rejects biased samples. */
uint32_t APP_RNG_Below(uint32_t bound);

int APP_INTEG_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTEG_H */
