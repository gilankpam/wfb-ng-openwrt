#ifndef WIRE_H
#define WIRE_H
#include <stddef.h>
#include <stdint.h>

#define MR_MAGIC 0x524D
#define MR_VER 1
enum { MR_FRAME = 1, MR_HELLO = 2, MR_TUNE = 3, MR_STATUS = 4, MR_TX = 5 };
#define MR_HDR_LEN 4
#define MR_FRAME_HDR_LEN 11
#define MR_FLAGS_OFFSET 10
#define MR_TUNE_LEN 8
#define MR_STATUS_LEN 35
#define MR_FLAG_BADFCS 0x01
#define MR_FLAG_DROPPED 0x02
enum { MR_OK = 0, MR_ESHORT = -1, MR_EMAGIC = -2, MR_EVER = -3, MR_ETYPE = -4 };
struct mr_tune { uint16_t tune_id; uint8_t channel; uint8_t sec; };
struct mr_status { uint16_t tune_id; uint8_t state, channel, sec, owner, you_own;
                   uint32_t rx, fwd, foreign, bad_fcs, your_drops, uptime_s; };
size_t mr_pack_frame_hdr(uint8_t *out, uint32_t seq, uint8_t rx_channel, uint8_t sec, uint8_t flags);
size_t mr_pack_hello(uint8_t *out);
size_t mr_pack_tune(uint8_t *out, const struct mr_tune *t);
size_t mr_pack_status(uint8_t *out, const struct mr_status *s);
int mr_parse_header(const uint8_t *b, size_t n, uint8_t *type);
int mr_parse_frame_hdr(const uint8_t *b, size_t n, uint32_t *seq, uint8_t *rx_channel, uint8_t *sec, uint8_t *flags);
int mr_parse_tune(const uint8_t *b, size_t n, struct mr_tune *t);
int mr_parse_status(const uint8_t *b, size_t n, struct mr_status *s);

#endif
