/**
 * @file    app_test.h
 * @brief   Deterministic protocol test and replay engine.
 *
 * Two distinct mechanisms, both real:
 *
 * 1. On-target vector suite.  A fixed table of protocol vectors is run against
 *    the same pure engines the firmware uses in production (app_dec, app_cap,
 *    app_txn, app_pps, app_cable).  A regression that only shows up on the
 *    board is therefore visible without a host connection.  Every vector is
 *    deterministic: no timestamps, no random input.
 *
 * 2. Replay.  Records already in the capture ring are fed back through the
 *    decoder and the transaction engine into a scratch port, and the result is
 *    compared with the live port.  If re-deriving the transaction from the same
 *    bytes does not reproduce the same state, the reconstruction is not
 *    actually a function of the wire data.  The replay digest is a hardware
 *    CRC, so a one-word difference shows up.
 *
 * Nothing here touches the wire: this instrument is a sink, so "injecting a
 * malformed message" means decoding a malformed byte sequence, not
 * transmitting one.
 */
#ifndef APP_TEST_H
#define APP_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct
{
  uint32_t vectors;
  uint32_t passed;
  uint32_t failed;
  uint32_t replay_records;
  uint32_t replay_crc;      /* 0 when no replay has been run */
  uint8_t  replay_match;    /* 1 when the replay reproduced live state */
  uint8_t  last_ok;
} APP_TEST_Result_t;

/** Run the deterministic vector suite.  @return the number of failures. */
uint32_t APP_TEST_RunSuite(void);

/**
 * Replay @p count records from the capture ring through the decoder and the
 * transaction engine.  @p count == 0 replays everything readable.
 */
void APP_TEST_Replay(uint32_t count);

void APP_TEST_GetResult(APP_TEST_Result_t *out);

int APP_TEST_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_TEST_H */
