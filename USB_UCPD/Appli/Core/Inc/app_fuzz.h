/**
 * @file    app_fuzz.h
 * @brief   Deterministic malformed-message engine.
 *
 * Feeds deliberately broken frames through the same decode, reassembly and
 * transaction paths the firmware uses on live traffic, and checks the property
 * that actually matters: **nothing crashes, nothing reads out of bounds, and
 * structural problems are flagged rather than silently accepted.**
 *
 * Deterministic by design.  The mutation stream comes from a seeded LCG, so a
 * failure can be reproduced from its seed alone.  The hardware RNG may be used
 * to pick a seed (see APP_FUZZ_RunRandom) but never to drive the mutations
 * directly, otherwise a failing run could not be replayed.
 *
 * Pure: no I/O, no globals outside the result struct, no allocation.
 */
#ifndef APP_FUZZ_H
#define APP_FUZZ_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Largest frame the fuzzer will generate. */
#define APP_FUZZ_MAX_FRAME 64u

/* Mutation kinds, recorded per case so a failure is explainable. */
typedef enum
{
  APP_FUZZ_MUT_NONE = 0,
  APP_FUZZ_MUT_TRUNCATE,      /* shorter than the header claims            */
  APP_FUZZ_MUT_BITFLIP,       /* single bit inverted at a random offset    */
  APP_FUZZ_MUT_BAD_TYPE,      /* reserved or unknown message type          */
  APP_FUZZ_MUT_NDO_MISMATCH,  /* NDO field disagrees with the payload      */
  APP_FUZZ_MUT_BAD_EXT,       /* extended header size larger than payload  */
  APP_FUZZ_MUT_CHUNK_GAP,     /* chunk numbers skip                        */
  APP_FUZZ_MUT_CHUNK_DUP,     /* same chunk number twice                   */
  APP_FUZZ_MUT_CHUNK_ORDER,   /* chunks arrive out of sequence             */
  APP_FUZZ_MUT_BAD_SEQ,       /* Accept/PS_RDY with no preceding Request   */
  APP_FUZZ_MUT_ZERO_LEN,      /* empty frame                               */
  APP_FUZZ_MUT_COUNT
} APP_FUZZ_Mut_t;

typedef struct
{
  uint32_t seed;
  uint32_t cases;
  uint32_t flagged;        /* decoder/reassembler reported a problem        */
  uint32_t accepted;       /* accepted cleanly - expected for benign cases  */
  uint32_t unflagged_bad;  /* malformed but NOT flagged: the real finding   */
  uint32_t per_mut[APP_FUZZ_MUT_COUNT];
  uint32_t last_bad_case;
  uint8_t  last_bad_mut;
  uint8_t  ok;             /* 1 when unflagged_bad == 0                     */
} APP_FUZZ_Result_t;

/** Advance the generator.  Deterministic for a given seed. */
uint32_t APP_FUZZ_Next(uint32_t *state);

/**
 * Run @p cases mutations starting from @p seed.
 * @return the number of malformed frames that were not flagged.
 */
uint32_t APP_FUZZ_Run(uint32_t seed, uint32_t cases, APP_FUZZ_Result_t *out);

/** Seed from the hardware RNG, then run.  The chosen seed is reported so the
 *  run can be replayed with APP_FUZZ_Run(). */
uint32_t APP_FUZZ_RunRandom(uint32_t cases, APP_FUZZ_Result_t *out);

const char *APP_FUZZ_MutName(APP_FUZZ_Mut_t m);

int APP_FUZZ_Cmd(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_FUZZ_H */
