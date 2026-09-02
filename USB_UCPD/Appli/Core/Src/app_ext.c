/**
 * @file    app_ext.c
 * @brief   Chunked extended-message reassembly (see app_ext.h).
 */
#include "app_ext.h"
#include "app_dec.h"
#include "app_log.h"

#include <string.h>
#include <stdio.h>

/* A live reassembler, fed from the capture path. */
static APP_EXT_Reasm_t s_live;

void APP_EXT_Reset(APP_EXT_Reasm_t *r)
{
  if (r != NULL)
  {
    memset(r, 0, sizeof(*r));
  }
}

uint16_t APP_EXT_ChunkSize(uint16_t total)
{
  /* PD 3.0 6.11.2: the first chunk of a chunked transfer carries at most
   * (Max Packet Size - 6) bytes of data.  With a 26-byte payload limit that is
   * 26 bytes; a 30-byte limit gives 26 as well after the 4-byte header pair.
   * Everything after the first chunk may carry up to 26 bytes.  We use 26,
   * which is the value ST's stack advertises. */
  (void)total;
  return 26u;
}

static void put(APP_EXT_Reasm_t *r, uint16_t offset, const uint8_t *src,
                uint16_t len)
{
  if ((offset + len) > APP_EXT_MAX_DATA)
  {
    len = (uint16_t)(APP_EXT_MAX_DATA - offset);
    r->errors |= APP_EXT_ERR_OVERFLOW;
  }
  memcpy(&r->data[offset], src, len);
}

int APP_EXT_Feed(APP_EXT_Reasm_t *r, const APP_DEC_Msg_t *m)
{
  uint16_t chunk_sz;

  if ((r == NULL) || (m == NULL))
  {
    return 0;
  }
  if ((m->flags & (APP_DEC_F_SHORT | APP_DEC_F_EXT_SHORT)) != 0u)
  {
    r->errors |= APP_EXT_ERR_BADDEC;
    return 0;
  }
  if (m->msg_class != APP_DEC_CLASS_EXTENDED)
  {
    return 0;                    /* not an extended message: nothing to do */
  }

  /* Unchunked extended message: it arrives whole, so copy it straight through. */
  if (m->ext_chunked == 0u)
  {
    if (r->active != 0u)
    {
      /* A new unchunked message cannot resume a chunked transfer. */
      APP_EXT_Reset(r);
    }
    r->active = 1u;
    r->msg_type = m->msg_type;
    r->msg_id = m->msg_id;
    r->total = m->ext_data_size;
    r->n_chunks = 1u;
    r->seen = 1u;
    r->complete = 0u;

    if (m->ext_data_size > APP_EXT_MAX_DATA)
    {
      r->errors |= APP_EXT_ERR_OVERFLOW;
      r->total = APP_EXT_MAX_DATA;
    }
    if (m->data_len < r->total)
    {
      r->have = m->data_len;
      r->errors |= APP_EXT_ERR_GAP;      /* declared more than it carried */
      r->complete = 0u;
      return 0;
    }
    put(r, 0u, m->data, r->total);
    r->have = r->total;
    r->complete = 1u;
    return 1;
  }

  /* Chunked frame. */
  chunk_sz = APP_EXT_ChunkSize(m->ext_data_size);

  if (m->data_len > chunk_sz)
  {
    r->errors |= APP_EXT_ERR_CHUNKSIZE;
  }

  if (r->active == 0u)
  {
    /* First chunk of a new transfer.  A well-formed transfer starts at 0. */
    r->active = 1u;
    r->msg_type = m->msg_type;
    r->msg_id = m->msg_id;
    r->total = m->ext_data_size;
    r->req_chunk = m->ext_req_chunk;
    r->expect = 0u;
    if (m->ext_chunk_num != 0u)
    {
      r->errors |= APP_EXT_ERR_ORDER;    /* did not start at chunk 0 */
      r->expect = m->ext_chunk_num;
    }
  }
  else
  {
    if (m->msg_type != r->msg_type)
    {
      r->errors |= APP_EXT_ERR_TYPE;
      APP_EXT_Reset(r);
      r->active = 1u;
      r->msg_type = m->msg_type;
      r->total = m->ext_data_size;
      r->expect = m->ext_chunk_num;
    }
    else if (m->ext_chunk_num == r->expect)
    {
      /* In order - nothing to flag. */
    }
    else if (m->ext_chunk_num < r->expect)
    {
      r->errors |= APP_EXT_ERR_DUP;      /* a chunk we already passed */
    }
    else
    {
      r->errors |= APP_EXT_ERR_GAP;      /* skipped one or more chunks */
      r->expect = m->ext_chunk_num;
    }
  }

  if (m->ext_chunk_num >= APP_EXT_MAX_CHUNKS)
  {
    /* The 4-bit field cannot exceed 15, so this is defensive only. */
    r->errors |= APP_EXT_ERR_OVERFLOW;
  }
  else
  {
    if ((r->seen & ((uint32_t)1u << m->ext_chunk_num)) != 0u)
    {
      r->errors |= APP_EXT_ERR_DUP;
    }
    r->seen |= ((uint32_t)1u << m->ext_chunk_num);
  }

  {
    uint16_t offset = (uint16_t)((uint16_t)m->ext_chunk_num * chunk_sz);

    if ((offset + m->data_len) > r->total)
    {
      /* The chunk claims to carry data past the declared size. */
      r->errors |= APP_EXT_ERR_OVERFLOW;
    }
    put(r, offset, m->data, m->data_len);

    if ((offset + m->data_len) > r->have)
    {
      r->have = (uint16_t)(offset + m->data_len);
    }
  }

  r->n_chunks++;
  r->expect = (uint8_t)(m->ext_chunk_num + 1u);

  if (r->have >= r->total)
  {
    r->complete = 1u;
    return 1;
  }
  return 0;
}

void APP_EXT_FormatErrors(uint8_t errors, char *out, size_t outsz)
{
  size_t n = 0u;

  if ((out == NULL) || (outsz == 0u))
  {
    return;
  }
  out[0] = '\0';

  if (errors == APP_EXT_ERR_NONE)
  {
    (void)snprintf(out, outsz, "none");
    return;
  }

#define E(bit, txt)                                                     \
  do {                                                                  \
    if ((errors & (bit)) != 0u)                                         \
    {                                                                   \
      n += (size_t)snprintf(&out[n], outsz - n, "%s%s",                 \
                            (n != 0u) ? "," : "", (txt));               \
      if (n >= outsz) { return; }                                       \
    }                                                                   \
  } while (0)

  E(APP_EXT_ERR_GAP, "gap");
  E(APP_EXT_ERR_DUP, "duplicate");
  E(APP_EXT_ERR_ORDER, "out-of-order");
  E(APP_EXT_ERR_TYPE, "type-change");
  E(APP_EXT_ERR_OVERFLOW, "overflow");
  E(APP_EXT_ERR_CHUNKSIZE, "oversized-chunk");
  E(APP_EXT_ERR_NOTCHUNK, "not-chunked");
  E(APP_EXT_ERR_BADDEC, "undecodable");
#undef E
}

const APP_EXT_Reasm_t *APP_EXT_GetLive(void)
{
  return &s_live;
}

/* Fed from the capture path; exposed so the wiring is testable. */
int APP_EXT_LiveFeed(const APP_DEC_Msg_t *m)
{
  return APP_EXT_Feed(&s_live, m);
}

int APP_EXT_Cmd(int argc, char *argv[])
{
  const APP_EXT_Reasm_t *r = APP_EXT_GetLive();
  char line[64];

  (void)argc;
  (void)argv;

  APP_LOG_Write("extended message reassembly\r\n");
  if (r->active == 0u)
  {
    APP_LOG_Write("  no extended message seen yet\r\n");
    return 1;
  }
  APP_LOG_Printf("  type      : %s (0x%02X)\r\n",
                 APP_DEC_ExtendedName(r->msg_type), r->msg_type);
  APP_LOG_Printf("  msg id    : %u\r\n", (unsigned)r->msg_id);
  APP_LOG_Printf("  declared  : %u bytes\r\n", (unsigned)r->total);
  APP_LOG_Printf("  collected : %u bytes in %u chunks\r\n",
                 (unsigned)r->have, (unsigned)r->n_chunks);
  APP_LOG_Printf("  chunks    : 0x%04lX\r\n", (unsigned long)r->seen);
  APP_LOG_Printf("  state     : %s\r\n",
                 r->complete ? "COMPLETE" : "IN PROGRESS");
  APP_EXT_FormatErrors(r->errors, line, sizeof(line));
  APP_LOG_Printf("  errors    : %s\r\n", line);
  return 1;
}
