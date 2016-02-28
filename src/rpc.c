#include <string.h>

#include "rpc.h"

enum {
  MPACK_RPC_RECEIVE_ARRAY = 1,
  MPACK_RPC_RECEIVE_TYPE,
  MPACK_RPC_RECEIVE_ID
};

static int mpack_rpc_validate_hdr(mpack_rpc_header_t *hdr);
static mpack_rpc_header_t mpack_rpc_request_hdr(void);
static mpack_rpc_header_t mpack_rpc_reply_hdr(void);
static mpack_rpc_header_t mpack_rpc_notify_hdr(void);
static struct mpack_rpc_bucket_s *mpack_rpc_search(mpack_rpc_session_t *session,
    mpack_uint32_t msg_id);
static int mpack_rpc_put(mpack_rpc_session_t *s, mpack_rpc_message_t m);
static int mpack_rpc_pop(mpack_rpc_session_t *s, mpack_rpc_message_t *m);
static void mpack_rpc_reset_hdr(mpack_rpc_header_t *hdr);

MPACK_API void mpack_rpc_session_init(mpack_rpc_session_t *session,
    mpack_uint32_t capacity)
{
  session->capacity = capacity ? capacity : MPACK_RPC_POOL_CAPACITY;
  session->request_id = 0;
  mpack_tokbuf_init(&session->reader);
  mpack_tokbuf_init(&session->writer);
  mpack_rpc_reset_hdr(&session->receive);
  mpack_rpc_reset_hdr(&session->send);
  memset(session->pool, 0,
      sizeof(struct mpack_rpc_bucket_s) * session->capacity);
}

MPACK_API int mpack_rpc_receive_tok(mpack_rpc_session_t *session,
    mpack_token_t tok, mpack_rpc_message_t *msg)
{
  int type;

  if (session->receive.index == 0) {
    session->receive.toks[0] = tok;
    session->receive.index++;
    return MPACK_EOF;  /* get the type */
  }

  if (session->receive.index == 1) {
    session->receive.toks[1] = tok;
    session->receive.index++;
    if ((type = mpack_rpc_validate_hdr(&session->receive))) goto end;
    if (session->receive.toks[1].data.value.lo < 2) return MPACK_EOF;
    type = MPACK_RPC_NOTIFICATION;
    goto end;
  }

  assert(session->receive.index == 2);
  
  if (tok.type != MPACK_TOKEN_UINT || tok.length > 4) {
    /* invalid message id */
    type = MPACK_RPC_EMSGID;
    goto end;
  }
    
  msg->id = tok.data.value.lo;
  msg->data = NULL;
  type = (int)session->receive.toks[1].data.value.lo + MPACK_RPC_REQUEST;

  if (type == MPACK_RPC_RESPONSE && !mpack_rpc_pop(session, msg))
    type = MPACK_RPC_ERESPID;

end:
  mpack_rpc_reset_hdr(&session->receive);
  return type;
}

MPACK_API int mpack_rpc_request_tok(mpack_rpc_session_t *session, 
    mpack_token_t *tok, void *data)
{
  if (session->send.index == 0) {
    int status;
    mpack_rpc_message_t msg;
    msg.id = session->request_id++;
    msg.data = data;
    session->send = mpack_rpc_request_hdr();
    session->send.toks[2].type = MPACK_TOKEN_UINT;
    session->send.toks[2].data.value.lo = msg.id;
    session->send.toks[2].data.value.hi = 0;
    *tok = session->send.toks[0];
    session->send.index++;
    status = mpack_rpc_put(session, msg);
    if (status == -1) return MPACK_NOMEM;
    assert(status);
    return MPACK_EOF;
  }
  
  if (session->send.index == 1) {
    *tok = session->send.toks[1];
    session->send.index++;
    return MPACK_EOF;
  }

  assert(session->send.index == 2);
  *tok = session->send.toks[2];
  mpack_rpc_reset_hdr(&session->send);
  return MPACK_OK;
}

MPACK_API int mpack_rpc_reply_tok(mpack_rpc_session_t *session,
    mpack_token_t *tok, mpack_uint32_t id)
{
  if (session->send.index == 0) {
    session->send = mpack_rpc_reply_hdr();
    session->send.toks[2].type = MPACK_TOKEN_UINT;
    session->send.toks[2].data.value.lo = id;
    session->send.toks[2].data.value.hi = 0;
    *tok = session->send.toks[0];
    session->send.index++;
    return MPACK_EOF;
  }

  if (session->send.index == 1) {
    *tok = session->send.toks[1];
    session->send.index++;
    return MPACK_EOF;
  }

  assert(session->send.index == 2);
  *tok = session->send.toks[2];
  mpack_rpc_reset_hdr(&session->send);
  return MPACK_OK;
}

MPACK_API int mpack_rpc_notify_tok(mpack_rpc_session_t *session,
    mpack_token_t *tok)
{
  if (session->send.index == 0) {
    session->send = mpack_rpc_notify_hdr();
    *tok = session->send.toks[0];
    session->send.index++;
    return MPACK_EOF;
  }

  assert(session->send.index == 1);
  *tok = session->send.toks[1];
  mpack_rpc_reset_hdr(&session->send);
  return MPACK_OK;
}

MPACK_API int mpack_rpc_receive(mpack_rpc_session_t *session, const char **buf,
    size_t *buflen, mpack_rpc_message_t *msg)
{
  int status;

  do {
    mpack_token_t tok;
    status = mpack_read(&session->reader, buf, buflen, &tok);
    if (status) break;
    status = mpack_rpc_receive_tok(session, tok, msg);
    if (status >= MPACK_RPC_REQUEST) break;
  } while (*buflen);

  return status;
}

MPACK_API int mpack_rpc_request(mpack_rpc_session_t *session, char **buf,
    size_t *buflen, void *data)
{
  int status = MPACK_EOF;

  while (status && *buflen) {
    int write_status;
    mpack_token_t tok;
    if (!session->writer.plen) {
      status = mpack_rpc_request_tok(session, &tok, data);
    }
    write_status = mpack_write(&session->writer, buf, buflen, &tok);
    status = write_status ? write_status : status;
  }

  return status;
}

MPACK_API int mpack_rpc_reply(mpack_rpc_session_t *session, char **buf,
    size_t *buflen, mpack_uint32_t id)
{
  int status = MPACK_EOF;

  while (status && *buflen) {
    int write_status;
    mpack_token_t tok;
    if (!session->writer.plen) {
      status = mpack_rpc_reply_tok(session, &tok, id);
    }
    write_status = mpack_write(&session->writer, buf, buflen, &tok);
    status = write_status ? write_status : status;
  }

  return status;
}

MPACK_API int mpack_rpc_notify(mpack_rpc_session_t *session, char **buf,
    size_t *buflen)
{
  int status = MPACK_EOF;

  while (status && *buflen) {
    int write_status;
    mpack_token_t tok;
    if (!session->writer.plen) {
      status = mpack_rpc_notify_tok(session, &tok);
    }
    write_status = mpack_write(&session->writer, buf, buflen, &tok);
    status = write_status ? write_status : status;
  }

  return status;
}

static int mpack_rpc_validate_hdr(mpack_rpc_header_t *hdr)
{
  if (hdr->toks[0].type != MPACK_TOKEN_ARRAY)
    /* not an array */
    return MPACK_RPC_EARRAY;

  if (hdr->toks[0].length < 3 || hdr->toks[0].length > 4)
    /* invalid array length */
    return MPACK_RPC_EARRAYL;

  if (hdr->toks[1].type != MPACK_TOKEN_UINT || hdr->toks[1].length > 1
      || hdr->toks[1].data.value.lo > 2)
    /* invalid type */
    return MPACK_RPC_ETYPE;

  if (hdr->toks[1].data.value.lo < 2 && hdr->toks[0].length != 4)
    /* request or response with array length != 4 */
    return MPACK_RPC_EARRAYL;

  if (hdr->toks[1].data.value.lo == 2 && hdr->toks[0].length != 3)
    /* notification with array length != 3 */
    return MPACK_RPC_EARRAYL;

  return 0;
}

static mpack_rpc_header_t mpack_rpc_request_hdr(void)
{
  mpack_rpc_header_t hdr;
  hdr.index = 0;
  hdr.toks[0].type = MPACK_TOKEN_ARRAY;
  hdr.toks[0].length = 4;
  hdr.toks[1].type = MPACK_TOKEN_UINT;
  hdr.toks[1].data.value.lo = 0;
  hdr.toks[1].data.value.hi = 0;
  return hdr;
}

static mpack_rpc_header_t mpack_rpc_reply_hdr(void)
{
  mpack_rpc_header_t hdr = mpack_rpc_request_hdr();
  hdr.toks[1].data.value.lo = 1;
  hdr.toks[1].data.value.hi = 0;
  return hdr;
}

static mpack_rpc_header_t mpack_rpc_notify_hdr(void)
{
  mpack_rpc_header_t hdr = mpack_rpc_request_hdr();
  hdr.toks[0].length = 3;
  hdr.toks[1].data.value.lo = 2;
  hdr.toks[1].data.value.hi = 0;
  return hdr;
}

static struct mpack_rpc_bucket_s *mpack_rpc_search(mpack_rpc_session_t *session,
    mpack_uint32_t msg_id)
{
  mpack_uint32_t i;
  mpack_uint32_t idx = msg_id % session->capacity;
  assert(session->capacity % 2 == 0);

  for (i = 0; i < session->capacity; i++) {
    if (!session->pool[idx].used || session->pool[idx].msg.id == msg_id)
      return session->pool + idx;
    idx = (idx + 1) % session->capacity;
  }

  return NULL;
}

static int mpack_rpc_put(mpack_rpc_session_t *session, mpack_rpc_message_t msg)
{
  struct mpack_rpc_bucket_s *bucket = mpack_rpc_search(session, msg.id);
  if (!bucket) return -1; /* no space */
  if (bucket->msg.id == msg.id && bucket->used) return 0;  /* duplicate key */
  bucket->msg = msg;
  bucket->used = 1;
  return 1;
}

static int mpack_rpc_pop(mpack_rpc_session_t *session, mpack_rpc_message_t *msg)
{
  mpack_uint32_t idx, next;
  struct mpack_rpc_bucket_s *bucket = mpack_rpc_search(session, msg->id);
  
  if (!bucket || !bucket->used) return 0;

  *msg = bucket->msg;
  bucket->used = 0;
  idx = (mpack_uint32_t)(bucket - session->pool);
  next = idx;

  for (;;) {
    struct mpack_rpc_bucket_s *next_bucket;
    next = (next + 1) % session->capacity;
    next_bucket = session->pool + next;
    if (!next_bucket->used) {
      /* found empty bucket, finished deleting */
      break;
    } else if (next_bucket->msg.id % session->capacity <= idx) {
      /* found a bucket with index less than or equal to the one just removed.
       * shift it to the removed position and repeat the search for the newly
       * removed bucket. more details in:
       * https://en.wikipedia.org/wiki/Linear_probing#Deletion) */
      *bucket = *next_bucket;
      bucket = next_bucket;
      bucket->used = 0;
      idx = (mpack_uint32_t)(bucket - session->pool);
      next = idx;
    }
  };

  return 1;
}

static void mpack_rpc_reset_hdr(mpack_rpc_header_t *hdr)
{
  hdr->index = 0;
}
