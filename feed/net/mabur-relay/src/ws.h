#ifndef WS_H
#define WS_H
#include <stddef.h>
#include <stdint.h>

#define WS_OP_TEXT 0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA
#define WS_KEY_MAX 64

/* 1 = complete valid upgrade (key copied, *consumed = bytes through the blank
 * line), 0 = need more bytes, -1 = invalid request. */
int ws_parse_upgrade(const char *req, size_t n, char key[WS_KEY_MAX], size_t *consumed);

/* Writes the 28-char Sec-WebSocket-Accept value + NUL. */
void ws_accept_key(const char *client_key, char out[29]);

/* Full "101 Switching Protocols" response; returns its length (0 if cap too small). */
size_t ws_build_response(char *out, size_t cap, const char *client_key);

/* Server->client header for one FIN frame; returns 2, 4 or 10. */
size_t ws_frame_header(uint8_t out[10], size_t payload_len, uint8_t opcode);

/* One client frame from buf. >0 = bytes consumed (payload unmasked in place,
 * *payload points into buf); 0 = need more; -1 = protocol error (unmasked,
 * not FIN, reserved bits, payload > max_payload, control frame > 125). */
long ws_parse_frame(uint8_t *buf, size_t n, uint8_t *opcode, uint8_t **payload,
                    size_t *plen, size_t max_payload);

/* exposed for tests */
void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
size_t ws_base64(const uint8_t *in, size_t n, char *out);   /* NUL-terminates */

#endif
