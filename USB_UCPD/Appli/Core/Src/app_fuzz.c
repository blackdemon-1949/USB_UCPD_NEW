/**
 * @file    app_fuzz.c
 * @brief   Deterministic malformed-message engine (see app_fuzz.h).
 */
#include "app_fuzz.h"
#include "app_dec.h"
#include "app_ext.h"
#include "app_txn.h"
#include "app_log.h"
#include "app_integ.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Generator                                                           */
/* ------------------------------------------------------------------ */

uint32_t APP_FUZZ_Next(uint32_t *state)
{
  /* Numerical Recipes LCG.  Chosen for being tiny and exactly reproducible on
   * both the target and the host, so a seed identifies a run. */
  if (state == NULL)
  {
    return 0u;
  }
  *state = (*state * 1664525u) + 1013904223u;
  return *state;
}

static uint32_t below(uint32_t *st, uint32_t bound)
{
  if (bound == 0u)
  {
    return 0u;
  }
  return APP_FUZZ_Next(st) % bound;
}

/* ------------------------------------------------------------------ */
/* Frame construction                                                  */
/* ------------------------------------------------------------------ */

/** b0 = type | dr<<5 | rev<<6 ; b1 = pr | msgid<<1 | ndo<<4 | ext<<7 */
static void hdr(uint8_t *f, uint8_t type, uint8_t ndo, uint8_t msgid,
                uint8_t pr, uint8_t ext)
{
  f[0] = (uint8_t)(type | (2u << 6));          /* rev = 2 -> PD 3.x */
  f[1] = (uint8_t)((pr & 1u) | ((msgid & 7u) << 1) |
                   ((ndo & 7u) << 4) | ((ext & 1u) << 7));
}

/** Build a well-formed base frame for @p mut and return its length. */
static uint16_t make_base(uint32_t *st, APP_FUZZ_Mut_t mut, uint8_t *f)
{
  uint16_t i;

  memset(f, 0, APP_FUZZ_MAX_FRAME);

  switch (mut)
  {
    case APP_FUZZ_MUT_BAD_EXT:
    case APP_FUZZ_MUT_CHUNK_GAP:
    case APP_FUZZ_MUT_CHUNK_DUP:
    case APP_FUZZ_MUT_CHUNK_ORDER:
      /* An extended message: 2 header bytes + 2 extended header bytes + data. */
      hdr(f, 0x01u, 1u, 1u, 1u, 1u);
      f[2] = 0x20u;                     /* DataSize = 32, chunked = 0 */
      f[3] = 0x00u;
      for (i = 4u; i < 20u; i++)
      {
        f[i] = (uint8_t)below(st, 256u);
      }
      return 20u;

    case APP_FUZZ_MUT_BAD_SEQ:
      /* PS_RDY with no preceding Request: a sequencing violation. */
      hdr(f, 0x06u, 0u, 3u, 1u, 0u);
      return 2u;

    case APP_FUZZ_MUT_ZERO_LEN:
      return 0u;

    default:
      /* A Source_Capabilities frame carrying two PDOs. */
      hdr(f, 0x01u, 2u, 1u, 1u, 0u);
      f[2] = 0x2Cu; f[3] = 0xD1u; f[4] = 0x02u; f[5] = 0x00u;   /* 9 V / 3 A */
      f[6] = 0x2Cu; f[7] = 0x91u; f[8] = 0x01u; f[9] = 0x00u;   /* 5 V / 3 A */
      return 10u;
  }
}

/**
 * Apply one mutation.
 * @return the new length, and sets *expect_bad when the result is definitely
 *         malformed and therefore must be flagged.
 */
static uint16_t mutate(uint32_t *st, APP_FUZZ_Mut_t mut, uint8_t *f,
                       uint16_t len, uint8_t *expect_bad)
{
  uint16_t off;

  *expect_bad = 1u;

  switch (mut)
  {
    case APP_FUZZ_MUT_TRUNCATE:
      /* Cut below the two header bytes, or below what NDO claims. */
      return (uint16_t)below(st, (len > 1u) ? len : 2u);

    case APP_FUZZ_MUT_BITFLIP:
      off = (uint16_t)below(st, len);
      f[off] ^= (uint8_t)(1u << below(st, 8u));
      /* A flipped bit may land somewhere harmless, so this case is not
       * required to be flagged - only required not to crash. */
      *expect_bad = 0u;
      return len;

    case APP_FUZZ_MUT_BAD_TYPE:
      /* 0x0E..0x1F are not valid control message types. */
      f[0] = (uint8_t)((f[0] & 0xE0u) | (0x1Eu + 0u));
      f[1] = (uint8_t)(f[1] & 0x8Fu);      /* NDO 0 -> treated as control */
      return 2u;

    case APP_FUZZ_MUT_NDO_MISMATCH:
      /* Claim five data objects but carry only two. */
      hdr(f, 0x01u, 5u, 1u, 1u, 0u);
      return 10u;

    case APP_FUZZ_MUT_BAD_EXT:
      /* Extended header declares 300 bytes; only 16 are present. */
      f[2] = 0x2Cu;
      f[3] = 0x01u;                        /* DataSize = 0x12C = 300 */
      return 20u;

    case APP_FUZZ_MUT_ZERO_LEN:
      return 0u;

    default:
      return len;
  }
}

/* ------------------------------------------------------------------ */
/* Chunk-sequence cases                                                */
/* ------------------------------------------------------------------ */

/** Build chunk @p n of a 2-chunk transfer into @p f and return its length. */
static uint16_t make_chunk(uint8_t *f, uint8_t n, uint8_t first)
{
  memset(f, 0, APP_FUZZ_MAX_FRAME);
  hdr(f, 0x0Cu, 1u, 1u, 1u, 1u);          /* PPS_Status, extended, chunked */
  /* DataSize = 30, chunked = 1, chunk number in B14..11 */
  f[2] = (uint8_t)(30u & 0xFFu);
  f[3] = (uint8_t)(0x80u | ((n & 0xFu) << 3) | ((30u >> 8) & 0x1u));
  if (first != 0u)
  {
    f[3] |= 0x04u;                         /* RequestChunk */
  }
  return 20u;
}

static void run_chunk_case(uint32_t *st, APP_FUZZ_Mut_t mut,
                           APP_FUZZ_Result_t *out)
{
  APP_EXT_Reasm_t r;
  APP_DEC_Msg_t m;
  uint8_t f[APP_FUZZ_MAX_FRAME];
  uint8_t seq[3];
  uint8_t i;
  uint8_t n = 2u;

  (void)st;
  APP_EXT_Reset(&r);

  switch (mut)
  {
    case APP_FUZZ_MUT_CHUNK_GAP:   seq[0] = 0u; seq[1] = 2u; n = 2u; break;
    case APP_FUZZ_MUT_CHUNK_DUP:   seq[0] = 0u; seq[1] = 0u; n = 2u; break;
    case APP_FUZZ_MUT_CHUNK_ORDER: seq[0] = 1u; seq[1] = 0u; n = 2u; break;
    default:                       return;
  }

  for (i = 0u; i < n; i++)
  {
    uint16_t len = make_chunk(f, seq[i], (uint8_t)(i == 0u));

    if (APP_DEC_Decode(f, len, &m) != 0)
    {
      continue;
    }
    (void)APP_EXT_Feed(&r, &m);
  }

  out->cases++;
  out->per_mut[mut]++;
  if (r.errors != APP_EXT_ERR_NONE)
  {
    out->flagged++;
  }
  else
  {
    /* A broken chunk sequence that the reassembler accepted is the finding. */
    out->unflagged_bad++;
    out->last_bad_case = out->cases;
    out->last_bad_mut = (uint8_t)mut;
  }
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

uint32_t APP_FUZZ_Run(uint32_t seed, uint32_t cases, APP_FUZZ_Result_t *out)
{
  uint32_t st = seed;
  uint32_t i;

  if (out == NULL)
  {
    return 0u;
  }
  memset(out, 0, sizeof(*out));
  out->seed = seed;

  if (cases == 0u)
  {
    out->ok = 1u;
    return 0u;
  }

  for (i = 0u; i < cases; i++)
  {
    APP_FUZZ_Mut_t mut = (APP_FUZZ_Mut_t)(1u + below(&st, APP_FUZZ_MUT_COUNT - 1u));
    uint8_t f[APP_FUZZ_MAX_FRAME];
    uint16_t len;
    uint8_t expect_bad = 0u;
    APP_DEC_Msg_t m;
    APP_TXN_Port_t p;

    if ((mut == APP_FUZZ_MUT_CHUNK_GAP) || (mut == APP_FUZZ_MUT_CHUNK_DUP) ||
        (mut == APP_FUZZ_MUT_CHUNK_ORDER))
    {
      run_chunk_case(&st, mut, out);
      continue;
    }

    len = make_base(&st, mut, f);
    len = mutate(&st, mut, f, len, &expect_bad);
    if (len > APP_FUZZ_MAX_FRAME)
    {
      len = APP_FUZZ_MAX_FRAME;
    }

    out->cases++;
    out->per_mut[mut]++;

    /* The property under test: this must not fault, whatever len and f hold. */
    if (APP_DEC_Decode(f, len, &m) != 0)
    {
      out->flagged++;
      continue;
    }

    /* Feed the same bytes to the other two consumers. */
    if (m.msg_class == APP_DEC_CLASS_EXTENDED)
    {
      APP_EXT_Reasm_t r;

      APP_EXT_Reset(&r);
      (void)APP_EXT_Feed(&r, &m);
      if (r.errors != APP_EXT_ERR_NONE)
      {
        out->flagged++;
        continue;
      }
    }

    APP_TXN_Init(&p);
    APP_TXN_Feed(&p, 0u, 0u, 0u, f, len);

    if (m.flags != 0u)
    {
      out->flagged++;
    }
    else if (mut == APP_FUZZ_MUT_BAD_SEQ)
    {
      /* A PS_RDY with no preceding Request must be recorded as unsolicited
       * rather than silently counted as a negotiated contract. */
      if (((p.flags & APP_TXN_F_UNMATCHED) != 0u) ||
          (p.n_unsolicited_psr != 0u))
      {
        out->flagged++;
      }
      else
      {
        out->unflagged_bad++;
        out->last_bad_case = out->cases;
        out->last_bad_mut = (uint8_t)mut;
      }
    }
    else if (expect_bad != 0u)
    {
      out->unflagged_bad++;
      out->last_bad_case = out->cases;
      out->last_bad_mut = (uint8_t)mut;
    }
    else
    {
      out->accepted++;
    }
  }

  out->ok = (out->unflagged_bad == 0u) ? 1u : 0u;
  return out->unflagged_bad;
}

uint32_t APP_FUZZ_RunRandom(uint32_t cases, APP_FUZZ_Result_t *out)
{
  uint32_t seed;

  /* Hardware RNG picks the seed only, so the run stays replayable. */
  seed = APP_RNG_U32();
  if (seed == 0u)
  {
    seed = 0xA5A5A5A5u;
  }
  return APP_FUZZ_Run(seed, cases, out);
}

const char *APP_FUZZ_MutName(APP_FUZZ_Mut_t m)
{
  switch (m)
  {
    case APP_FUZZ_MUT_NONE:        return "none";
    case APP_FUZZ_MUT_TRUNCATE:    return "truncate";
    case APP_FUZZ_MUT_BITFLIP:     return "bit-flip";
    case APP_FUZZ_MUT_BAD_TYPE:    return "bad-type";
    case APP_FUZZ_MUT_NDO_MISMATCH:return "ndo-mismatch";
    case APP_FUZZ_MUT_BAD_EXT:     return "bad-ext-header";
    case APP_FUZZ_MUT_CHUNK_GAP:   return "chunk-gap";
    case APP_FUZZ_MUT_CHUNK_DUP:   return "chunk-dup";
    case APP_FUZZ_MUT_CHUNK_ORDER: return "chunk-out-of-order";
    case APP_FUZZ_MUT_BAD_SEQ:     return "bad-sequence";
    case APP_FUZZ_MUT_ZERO_LEN:    return "zero-length";
    default:                       return "?";
  }
}

int APP_FUZZ_Cmd(int argc, char *argv[])
{
  APP_FUZZ_Result_t r;
  uint32_t seed = 1u;
  uint32_t n = 200u;
  uint32_t bad;
  uint32_t i;

  if (argc >= 3)
  {
    unsigned v = 0u;

    if (sscanf(argv[2], "%u", &v) == 1)
    {
      n = v;
    }
  }
  if (argc >= 4)
  {
    unsigned v = 0u;

    if (sscanf(argv[3], "%u", &v) == 1)
    {
      seed = v;
    }
  }

  if ((argc >= 2) && (strcmp(argv[1], "random") == 0))
  {
    bad = APP_FUZZ_RunRandom(n, &r);
  }
  else
  {
    bad = APP_FUZZ_Run(seed, n, &r);
  }

  APP_LOG_Printf("fuzz: %lu cases, seed 0x%08lX\r\n",
                 (unsigned long)r.cases, (unsigned long)r.seed);
  APP_LOG_Printf("  flagged        : %lu\r\n", (unsigned long)r.flagged);
  APP_LOG_Printf("  clean/accepted : %lu\r\n", (unsigned long)r.accepted);
  APP_LOG_Printf("  UNFLAGGED BAD  : %lu\r\n", (unsigned long)bad);
  for (i = 1u; i < (uint32_t)APP_FUZZ_MUT_COUNT; i++)
  {
    if (r.per_mut[i] != 0u)
    {
      APP_LOG_Printf("  %-18s %lu\r\n", APP_FUZZ_MutName((APP_FUZZ_Mut_t)i),
                     (unsigned long)r.per_mut[i]);
    }
  }
  if (bad != 0u)
  {
    APP_LOG_Printf("  first at case %lu (%s)\r\n",
                   (unsigned long)r.last_bad_case,
                   APP_FUZZ_MutName((APP_FUZZ_Mut_t)r.last_bad_mut));
  }
  APP_LOG_Printf("  verdict        : %s\r\n", r.ok ? "PASS" : "FAIL");
  return 1;
}
