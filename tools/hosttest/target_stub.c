/*
 * Stubs for the small number of target-only entry points that the otherwise
 * pure engines reference.  They let app_test.c run its vector suite on the
 * host, which is what caught the truncated test frames.
 */
#include <stdint.h>

/* Software CRC-32 (IEEE 802.3 polynomial), used in place of the CRC peripheral
 * so that the replay digest is still a real, reproducible function of the
 * bytes.  It does not have to match the hardware polynomial for the test's
 * purpose, which is that identical input gives identical output. */
uint32_t APP_CRC_Calc(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t i;
  int      b;

  if (data == 0)
  {
    return 0u;
  }
  for (i = 0u; i < len; i++)
  {
    crc ^= data[i];
    for (b = 0; b < 8; b++)
    {
      crc = ((crc & 1u) != 0u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return ~crc;
}

uint32_t APP_PDCAP_Cycles(void) { return 0u; }

uint32_t SystemCoreClock = 400000000u;

#include "app_txn.h"

/* The port instance the capture path owns on target. */
APP_TXN_Port_t APP_TXN_Port0;

/* Stand-in for the hardware RNG: a small LCG.  Deterministic, which is what
 * the replay digest comparison needs. */
uint32_t APP_RNG_Below(uint32_t bound)
{
  static uint32_t s = 12345u;

  if (bound <= 1u)
  {
    return 0u;
  }
  s = s * 1103515245u + 12345u;
  return s % bound;
}

uint32_t APP_RNG_U32(void)
{
  static uint32_t s = 0x1234ABCDu;

  s = s * 1103515245u + 12345u;
  return s;
}
