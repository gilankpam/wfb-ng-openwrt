#include "wire.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_hdr(uint8_t *out, uint8_t type) {
  put16(out, MR_MAGIC); out[2] = MR_VER; out[3] = type;
}

size_t mr_pack_frame_hdr(uint8_t *out, uint32_t seq, uint8_t rx_channel, uint8_t sec, uint8_t flags) {
  put_hdr(out, MR_FRAME); put32(out + 4, seq);
  out[8] = rx_channel; out[9] = sec; out[10] = flags;
  return MR_FRAME_HDR_LEN;
}

size_t mr_pack_hello(uint8_t *out) { put_hdr(out, MR_HELLO); return MR_HDR_LEN; }

size_t mr_pack_tune(uint8_t *out, const struct mr_tune *t) {
  put_hdr(out, MR_TUNE); put16(out + 4, t->tune_id); out[6] = t->channel; out[7] = t->sec;
  return MR_TUNE_LEN;
}

size_t mr_pack_status(uint8_t *out, const struct mr_status *s) {
  put_hdr(out, MR_STATUS); put16(out + 4, s->tune_id);
  out[6] = s->state; out[7] = s->channel; out[8] = s->sec; out[9] = s->owner; out[10] = s->you_own;
  put32(out + 11, s->rx); put32(out + 15, s->fwd); put32(out + 19, s->foreign);
  put32(out + 23, s->bad_fcs); put32(out + 27, s->your_drops); put32(out + 31, s->uptime_s);
  return MR_STATUS_LEN;
}

int mr_parse_header(const uint8_t *b, size_t n, uint8_t *type) {
  if (n < MR_HDR_LEN) return MR_ESHORT;
  if (get16(b) != MR_MAGIC) return MR_EMAGIC;
  if (b[2] != MR_VER) return MR_EVER;
  if (b[3] < MR_FRAME || b[3] > MR_TX) return MR_ETYPE;
  *type = b[3];
  return MR_OK;
}

static int expect(const uint8_t *b, size_t n, uint8_t want, size_t len) {
  uint8_t type;
  int rc = mr_parse_header(b, n, &type);
  if (rc != MR_OK) return rc;
  if (type != want) return MR_ETYPE;
  return n < len ? MR_ESHORT : MR_OK;
}

int mr_parse_frame_hdr(const uint8_t *b, size_t n, uint32_t *seq, uint8_t *rx_channel, uint8_t *sec, uint8_t *flags) {
  int rc = expect(b, n, MR_FRAME, MR_FRAME_HDR_LEN);
  if (rc != MR_OK) return rc;
  *seq = get32(b + 4); *rx_channel = b[8]; *sec = b[9]; *flags = b[10];
  return MR_OK;
}

int mr_parse_tune(const uint8_t *b, size_t n, struct mr_tune *t) {
  int rc = expect(b, n, MR_TUNE, MR_TUNE_LEN);
  if (rc != MR_OK) return rc;
  t->tune_id = get16(b + 4); t->channel = b[6]; t->sec = b[7];
  return MR_OK;
}

int mr_parse_status(const uint8_t *b, size_t n, struct mr_status *s) {
  int rc = expect(b, n, MR_STATUS, MR_STATUS_LEN);
  if (rc != MR_OK) return rc;
  s->tune_id = get16(b + 4);
  s->state = b[6]; s->channel = b[7]; s->sec = b[8]; s->owner = b[9]; s->you_own = b[10];
  s->rx = get32(b + 11); s->fwd = get32(b + 15); s->foreign = get32(b + 19);
  s->bad_fcs = get32(b + 23); s->your_drops = get32(b + 27); s->uptime_s = get32(b + 31);
  return MR_OK;
}
