#ifndef WIRE_H
#define WIRE_H
#include <stddef.h>
#include <stdint.h>

#define MR_MAGIC 0x524D
#define MR_VER 3
enum { MR_FRAME = 1, MR_HELLO = 2, MR_TUNE = 3, MR_STATUS = 4, MR_TX = 5 };
#define MR_HDR_LEN 4
#define MR_FRAME_HDR_LEN 20
#define MR_FLAGS_OFFSET 10
#define MR_TUNE_LEN 8
#define MR_STATUS_LEN 47
#define MR_TX_HDR_LEN 6
#define MR_TX_DOT11_MIN 24
#define MR_TX_DOT11_MAX 1500
#define MR_TX_LDPC 0x01
#define MR_TX_STBC 0x02
#define MR_TX_SGI 0x04
#define MR_TX_BW40 0x08
#define MR_TX_FLAGS_ALL 0x0F
#define MR_FLAG_BADFCS 0x01
#define MR_FLAG_DROPPED 0x02
#define MR_FLAG_PHY_VALID 0x04
#define MR_FLAG_SGI 0x08
#define MR_FLAG_STBC 0x10
#define MR_FLAG_LDPC 0x20
#define MR_FLAG_BW40 0x40
#define MR_MCS_NONE 0xFF
#define MR_DBM_ABSENT (-128)
enum { MR_OK = 0, MR_ESHORT = -1, MR_EMAGIC = -2, MR_EVER = -3, MR_ETYPE = -4, MR_EINVAL = -5 };
struct mr_tune { uint16_t tune_id; uint8_t channel; uint8_t sec; };
struct mr_status { uint16_t tune_id; uint8_t state, channel, sec, owner, you_own;
                   uint32_t rx, fwd, foreign, bad_fcs, your_drops, uptime_s,
                            tx, tx_fail, tx_refused; };
struct mr_frame_meta { uint32_t seq; uint8_t rx_channel, sec, flags, mcs;
                       int8_t rssi[2], noise[2]; uint32_t tsf_lo; };
/* TX body view: d points INTO the parsed buffer (no copy). */
struct mr_tx { uint8_t mcs, flags; const uint8_t *d; size_t dl; };
size_t mr_pack_frame_hdr(uint8_t *out, const struct mr_frame_meta *m);
size_t mr_pack_hello(uint8_t *out);
size_t mr_pack_tune(uint8_t *out, const struct mr_tune *t);
size_t mr_pack_status(uint8_t *out, const struct mr_status *s);
size_t mr_pack_tx_hdr(uint8_t *out, uint8_t mcs, uint8_t flags);
int mr_parse_header(const uint8_t *b, size_t n, uint8_t *type);
int mr_parse_frame_hdr(const uint8_t *b, size_t n, struct mr_frame_meta *m);
int mr_parse_tune(const uint8_t *b, size_t n, struct mr_tune *t);
int mr_parse_status(const uint8_t *b, size_t n, struct mr_status *s);
int mr_parse_tx(const uint8_t *b, size_t n, struct mr_tx *t);

#endif
