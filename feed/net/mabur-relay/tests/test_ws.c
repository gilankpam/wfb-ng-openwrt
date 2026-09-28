#include <stdlib.h>
#include <string.h>
#include "t.h"
#include "ws.h"

static void t_sha1_b64(void) {
  uint8_t d[20]; char b[64];
  ws_sha1((const uint8_t *)"abc", 3, d);
  ws_base64(d, 20, b);
  CHECK(strcmp(b, "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=") == 0);
  ws_sha1((const uint8_t *)"", 0, d);
  ws_base64(d, 20, b);
  CHECK(strcmp(b, "2jmj7l5rSw0yVb/vlWAYkK/YBwk=") == 0);
  ws_base64((const uint8_t *)"ab", 2, b); CHECK(strcmp(b, "YWI=") == 0);
  ws_base64((const uint8_t *)"a", 1, b);  CHECK(strcmp(b, "YQ==") == 0);
}

static void t_handshake(void) {
  char out[29];
  ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out);
  CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

  const char *req =
      "GET / HTTP/1.1\r\nHost: 192.168.1.1:8311\r\nUpgrade: websocket\r\n"
      "Connection: keep-alive, Upgrade\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\nEXTRA";
  char key[WS_KEY_MAX]; size_t used = 0;
  CHECK_EQ(ws_parse_upgrade(req, strlen(req), key, &used), 1);
  CHECK(strcmp(key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
  CHECK_EQ(used, strlen(req) - 5);
  CHECK_EQ(ws_parse_upgrade(req, 40, key, &used), 0);            /* incomplete */
  const char *bad = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";         /* no key */
  CHECK_EQ(ws_parse_upgrade(bad, strlen(bad), key, &used), -1);
  const char *post = "POST / HTTP/1.1\r\nSec-WebSocket-Key: abc\r\n\r\n";
  CHECK_EQ(ws_parse_upgrade(post, strlen(post), key, &used), -1);

  char resp[256];
  size_t n = ws_build_response(resp, sizeof resp, "dGhlIHNhbXBsZSBub25jZQ==");
  CHECK(n > 0 && n == strlen(resp));
  CHECK(strstr(resp, "HTTP/1.1 101") == resp);
  CHECK(strstr(resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != NULL);
  CHECK(n >= 4 && memcmp(resp + n - 4, "\r\n\r\n", 4) == 0);
}

static void t_server_header(void) {
  uint8_t h[10];
  CHECK_EQ(ws_frame_header(h, 125, WS_OP_BINARY), 2);
  CHECK_EQ(h[0], 0x82); CHECK_EQ(h[1], 125);
  CHECK_EQ(ws_frame_header(h, 126, WS_OP_BINARY), 4);
  CHECK_EQ(h[1], 126); CHECK_EQ(h[2], 0); CHECK_EQ(h[3], 126);
  CHECK_EQ(ws_frame_header(h, 65535, WS_OP_BINARY), 4);
  CHECK_EQ(h[2], 0xFF); CHECK_EQ(h[3], 0xFF);
  CHECK_EQ(ws_frame_header(h, 65536, WS_OP_BINARY), 10);
  CHECK_EQ(h[1], 127); CHECK_EQ(h[7], 1); CHECK_EQ(h[8], 0); CHECK_EQ(h[9], 0);
}

/* Build a masked client frame the way a browser does. */
static size_t client_frame(uint8_t *out, uint8_t op, const uint8_t *p, size_t n, int masked) {
  static const uint8_t mk[4] = {0x11, 0x22, 0x33, 0x44};
  size_t o = 0;
  out[o++] = (uint8_t)(0x80 | op);
  uint8_t mbit = masked ? 0x80 : 0;
  if (n < 126) out[o++] = (uint8_t)(mbit | n);
  else { out[o++] = (uint8_t)(mbit | 126); out[o++] = (uint8_t)(n >> 8); out[o++] = (uint8_t)n; }
  if (masked) { memcpy(out + o, mk, 4); o += 4; }
  for (size_t i = 0; i < n; i++) out[o + i] = masked ? (uint8_t)(p[i] ^ mk[i & 3]) : p[i];
  return o + n;
}

static void t_client_frames(void) {
  uint8_t pay[300], buf[400], op, *pl; size_t plen;
  for (int i = 0; i < 300; i++) pay[i] = (uint8_t)i;
  size_t n = client_frame(buf, WS_OP_BINARY, pay, 8, 1);
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 4096), (long)n);
  CHECK_EQ(op, WS_OP_BINARY); CHECK_EQ(plen, 8); CHECK(memcmp(pl, pay, 8) == 0);
  n = client_frame(buf, WS_OP_BINARY, pay, 300, 1);                  /* 16-bit length */
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 4096), (long)n);
  CHECK_EQ(plen, 300); CHECK(memcmp(pl, pay, 300) == 0);
  n = client_frame(buf, WS_OP_BINARY, pay, 300, 1);
  CHECK_EQ(ws_parse_frame(buf, n - 1, &op, &pl, &plen, 4096), 0);   /* partial */
  CHECK_EQ(ws_parse_frame(buf, 1, &op, &pl, &plen, 4096), 0);
  n = client_frame(buf, WS_OP_BINARY, pay, 300, 1);
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 100), -1);        /* > max */
  n = client_frame(buf, WS_OP_BINARY, pay, 8, 0);
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 4096), -1);       /* unmasked */
  n = client_frame(buf, WS_OP_BINARY, pay, 8, 1); buf[0] &= 0x7F;    /* not FIN */
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 4096), -1);
  n = client_frame(buf, WS_OP_BINARY, pay, 8, 1); buf[0] |= 0x40;    /* RSV1 */
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 4096), -1);
  n = client_frame(buf, WS_OP_CLOSE, pay, 0, 1);
  CHECK_EQ(ws_parse_frame(buf, n, &op, &pl, &plen, 4096), (long)n);
  CHECK_EQ(op, WS_OP_CLOSE);
}

void t_ws(const char *fx) {
  (void)fx;
  t_sha1_b64(); t_handshake(); t_server_header(); t_client_frames();
}
