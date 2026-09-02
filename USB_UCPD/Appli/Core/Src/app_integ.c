/**
 * @file    app_integ.c
 * @brief   Hardware CRC / HASH / RNG (see app_integ.h).
 */
#include "app_integ.h"
#include "main.h"          /* HAL and the CubeMX handles */
#include "crc.h"
#include "hash.h"
#include "rng.h"
#include "app_log.h"
#include "app_cap.h"
#include "app_diag.h"

#include <string.h>
#include <stdio.h>

/** Alignment staging buffer for the CRC peripheral, which wants words. */
#define APP_CRC_STAGE_WORDS 16u

uint8_t APP_INTEG_Ready(void)
{
  return ((hcrc.Instance != NULL) && (hhash.Instance != NULL) &&
          (hrng.Instance != NULL)) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* CRC                                                                 */
/* ------------------------------------------------------------------ */

uint32_t APP_CRC_Calc(const uint8_t *data, uint32_t len)
{
  uint32_t stage[APP_CRC_STAGE_WORDS];
  uint32_t crc = 0u;
  uint32_t done = 0u;

  if ((data == NULL) || (len == 0u))
  {
    return 0u;
  }
  if (hcrc.Instance == NULL)
  {
    return 0u;
  }

  while (done < len)
  {
    uint32_t chunk = len - done;
    uint32_t words;

    if (chunk > (APP_CRC_STAGE_WORDS * 4u))
    {
      chunk = APP_CRC_STAGE_WORDS * 4u;
    }

    memset(stage, 0, sizeof(stage));
    memcpy(stage, &data[done], chunk);

    /* Round up to a whole number of words: the trailing zero bytes are part of
     * the CRC input, which is fine because the buffer content is deterministic
     * for a given (data, len) pair. */
    words = (chunk + 3u) / 4u;

    if (done == 0u)
    {
      crc = HAL_CRC_Calculate(&hcrc, stage, words);
    }
    else
    {
      crc = HAL_CRC_Accumulate(&hcrc, stage, words);
    }
    done += chunk;
  }

  return crc;
}

/* ------------------------------------------------------------------ */
/* HASH                                                                */
/* ------------------------------------------------------------------ */

int APP_HASH_Sha256(const uint8_t *data, uint32_t len, uint8_t *out)
{
  if ((data == NULL) || (out == NULL))
  {
    return 0;
  }
  if (hhash.Instance == NULL)
  {
    return 0;
  }
  if (HAL_HASH_Start(&hhash, data, len, out, HAL_MAX_DELAY) != HAL_OK)
  {
    return 0;
  }
  return 1;
}

int APP_HASH_SessionId(uint8_t out[APP_HASH_SHA256_LEN])
{
  /* Deterministic description of what this session saw.  Deliberately excludes
   * timestamps and wall-clock so that a genuine replay produces the same id. */
  uint8_t blob[64];
  APP_CAP_Stats_t st;
  uint32_t crc;
  uint32_t i;

  if (out == NULL)
  {
    return 0;
  }
  memset(blob, 0, sizeof(blob));

  APP_CAP_GetStats(&st);

  blob[0] = 'P';
  blob[1] = 'D';
  blob[2] = 'S';
  blob[3] = 1u;                    /* format version */

  blob[4] = (uint8_t)(st.total & 0xFFu);
  blob[5] = (uint8_t)((st.total >> 8) & 0xFFu);
  blob[6] = (uint8_t)((st.total >> 16) & 0xFFu);
  blob[7] = (uint8_t)((st.total >> 24) & 0xFFu);
  blob[8] = (uint8_t)(st.dropped & 0xFFu);
  blob[9] = (uint8_t)(st.clipped & 0xFFu);
  blob[10] = (uint8_t)(st.capacity & 0xFFu);
  blob[11] = (uint8_t)((st.capacity >> 8) & 0xFFu);

  /* Fold the whole diagnostic counter block in with a CRC: that keeps the blob
   * a fixed size while still changing when any counter changes. */
  {
    APP_DIAG_Snapshot_t snap;
    uint8_t *raw;

    APP_DIAG_GetAll(&snap);
    raw = (uint8_t *)&snap;
    for (i = 0u; i < (APP_DIAG_COUNT * 4u); i++)
    {
      blob[16 + (i % 44u)] ^= raw[i];
    }
  }

  crc = APP_CRC_Calc(blob, sizeof(blob));
  blob[12] = (uint8_t)(crc & 0xFFu);
  blob[13] = (uint8_t)((crc >> 8) & 0xFFu);
  blob[14] = (uint8_t)((crc >> 16) & 0xFFu);
  blob[15] = (uint8_t)((crc >> 24) & 0xFFu);

  return APP_HASH_Sha256(blob, sizeof(blob), out);
}

/* ------------------------------------------------------------------ */
/* RNG                                                                 */
/* ------------------------------------------------------------------ */

uint32_t APP_RNG_U32(void)
{
  uint32_t r = 0u;

  if (hrng.Instance == NULL)
  {
    return 0u;
  }
  if (HAL_RNG_GenerateRandomNumber(&hrng, &r) != HAL_OK)
  {
    return 0u;
  }
  return r;
}

uint32_t APP_RNG_Below(uint32_t bound)
{
  uint32_t r;

  if (bound == 0u)
  {
    return 0u;
  }
  if (bound == 1u)
  {
    return 0u;
  }

  /* Rejection sampling: dropping the top of the range removes the bias that a
   * plain modulo would introduce.  At most a couple of iterations in practice. */
  {
    uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % bound);
    uint8_t guard = 0u;

    do
    {
      r = APP_RNG_U32();
      guard++;
    } while ((r > limit) && (guard < 32u));
  }
  return r % bound;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

int APP_INTEG_Cmd(int argc, char *argv[])
{
  const char *sub = (argc >= 2) ? argv[1] : "status";

  if (strcmp(sub, "crc") == 0)
  {
    const char *msg = (argc >= 3) ? argv[2] : "USB-C PD analyzer";
    uint32_t crc = APP_CRC_Calc((const uint8_t *)msg, (uint32_t)strlen(msg));

    APP_LOG_Printf("crc32(\"%s\") = 0x%08lX\r\n", msg, (unsigned long)crc);
    return 1;
  }

  if ((strcmp(sub, "sha256") == 0) || (strcmp(sub, "session") == 0))
  {
    uint8_t id[APP_HASH_SHA256_LEN];
    uint32_t i;
    const char *msg = (argc >= 3) ? argv[2] : NULL;
    int ok;

    if (msg != NULL)
    {
      ok = APP_HASH_Sha256((const uint8_t *)msg, (uint32_t)strlen(msg), id);
    }
    else
    {
      ok = APP_HASH_SessionId(id);
    }

    if (ok == 0)
    {
      APP_LOG_Write("HASH peripheral unavailable\r\n");
      return 1;
    }
    APP_LOG_Printf("%s sha256: ", (msg != NULL) ? "input" : "session");
    for (i = 0u; i < APP_HASH_SHA256_LEN; i++)
    {
      APP_LOG_Printf("%02X", (unsigned)id[i]);
    }
    APP_LOG_Write("\r\n");
    return 1;
  }

  if (strcmp(sub, "rng") == 0)
  {
    uint32_t i;

    APP_LOG_Write("rng: ");
    for (i = 0u; i < 4u; i++)
    {
      APP_LOG_Printf("%08lX ", (unsigned long)APP_RNG_U32());
    }
    APP_LOG_Write("\r\n");
    return 1;
  }

  if (strcmp(sub, "status") != 0)
  {
    APP_LOG_Write("usage: integ [status|crc [text]|sha256 [text]|session|rng]\r\n");
    return 1;
  }

  APP_LOG_Write("integrity engines\r\n");
  APP_LOG_Printf("  CRC  : %s\r\n", (hcrc.Instance != NULL) ? "ready" : "unavailable");
  APP_LOG_Printf("  HASH : %s (SHA-256)\r\n",
                 (hhash.Instance != NULL) ? "ready" : "unavailable");
  APP_LOG_Printf("  RNG  : %s\r\n", (hrng.Instance != NULL) ? "ready" : "unavailable");
  APP_LOG_Printf("  all  : %s\r\n", APP_INTEG_Ready() ? "READY" : "DEGRADED");
  return 1;
}
