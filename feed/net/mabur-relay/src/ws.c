#include "ws.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* --- SHA-1 (FIPS 180-1), only used for the handshake --- */
static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t blk[64];
  size_t i = 0;
  for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
  size_t rem = len - i;
  memset(blk, 0, 64);
  memcpy(blk, data + i, rem);
  blk[rem] = 0x80;
  if (rem >= 56) { sha1_block(h, blk); memset(blk, 0, 64); }
  uint64_t bits = (uint64_t)len * 8;
  for (int j = 0; j < 8; j++) blk[63 - j] = (uint8_t)(bits >> (8 * j));
  sha1_block(h, blk);
  for (int j = 0; j < 5; j++) {
    out[4 * j] = (uint8_t)(h[j] >> 24); out[4 * j + 1] = (uint8_t)(h[j] >> 16);
    out[4 * j + 2] = (uint8_t)(h[j] >> 8); out[4 * j + 3] = (uint8_t)h[j];
  }
}

size_t ws_base64(const uint8_t *in, size_t n, char *out) {
  static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = tb[(v >> 18) & 63];
    out[o++] = tb[(v >> 12) & 63];
    out[o++] = i + 1 < n ? tb[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? tb[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

int ws_accept_key(const char *client_key, char out[29]) {
  static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char cat[WS_KEY_MAX + sizeof guid];
  size_t kl = strlen(client_key);
  if (kl >= WS_KEY_MAX) { out[0] = 0; return -1; }
  memcpy(cat, client_key, kl);
  memcpy(cat + kl, guid, sizeof guid - 1);
  uint8_t d[20];
  ws_sha1((const uint8_t *)cat, kl + sizeof guid - 1, d);
  ws_base64(d, 20, out);
  return 0;
}

/* Finds "\r\n\r\n"; requires "GET " and a Sec-WebSocket-Key header
 * (case-insensitive name, value trimmed). Other headers are not enforced. */
int ws_parse_upgrade(const char *req, size_t n, char key[WS_KEY_MAX], size_t *consumed) {
  size_t end = 0;
  for (size_t i = 0; i + 3 < n; i++)
    if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') { end = i + 4; break; }
  if (!end) return n > 8192 ? -1 : 0;
  if (n < 4 || memcmp(req, "GET ", 4) != 0) return -1;
  static const char name[] = "sec-websocket-key:";
  size_t nl = sizeof name - 1;
  for (size_t i = 0; i + nl < end; i++) {
    if ((i == 0 || req[i - 1] == '\n') && strncasecmp(req + i, name, nl) == 0) {
      size_t v = i + nl;
      while (v < end && (req[v] == ' ' || req[v] == '\t')) v++;
      size_t e = v;
      while (e < end && req[e] != '\r') e++;
      while (e > v && (req[e - 1] == ' ' || req[e - 1] == '\t')) e--;
      if (e == v || e - v >= WS_KEY_MAX) return -1;
      memcpy(key, req + v, e - v);
      key[e - v] = 0;
      *consumed = end;
      return 1;
    }
  }
  return -1;
}

size_t ws_build_response(char *out, size_t cap, const char *client_key) {
  char acc[29];
  if (ws_accept_key(client_key, acc) != 0) return 0;
  int n = snprintf(out, cap,
                   "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

size_t ws_frame_header(uint8_t out[10], size_t len, uint8_t opcode) {
  out[0] = (uint8_t)(0x80 | opcode);
  if (len < 126) { out[1] = (uint8_t)len; return 2; }
  if (len <= 0xFFFF) { out[1] = 126; out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len; return 4; }
  out[1] = 127;
  uint64_t l = len;
  for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)(l >> (56 - 8 * i));
  return 10;
}

long ws_parse_frame(uint8_t *buf, size_t n, uint8_t *opcode, uint8_t **payload,
                    size_t *plen, size_t max_payload) {
  if (n < 2) return 0;
  uint8_t b0 = buf[0], b1 = buf[1];
  if (!(b0 & 0x80) || (b0 & 0x70) || !(b1 & 0x80)) return -1;
  uint8_t op = b0 & 0x0F;
  uint64_t len = b1 & 0x7F;
  size_t o = 2;
  if (len == 126) {
    if (n < 4) return 0;
    len = ((uint64_t)buf[2] << 8) | buf[3]; o = 4;
  } else if (len == 127) {
    if (n < 10) return 0;
    len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | buf[2 + i];
    o = 10;
  }
  if ((op & 0x8) && len > 125) return -1;
  if (len > max_payload) return -1;
  if (n < o + 4 + len) return 0;
  uint8_t *mk = buf + o;
  uint8_t *p = buf + o + 4;
  for (uint64_t i = 0; i < len; i++) p[i] ^= mk[i & 3];
  *opcode = op; *payload = p; *plen = (size_t)len;
  return (long)(o + 4 + len);
}
