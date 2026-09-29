#include <string.h>
#include "t.h"
#include "wire.h"

static void golden_frame_hdr(void) {
  struct mr_frame_meta m = {0x01020304u, 136, 2,
                            MR_FLAG_BADFCS | MR_FLAG_PHY_VALID | MR_FLAG_STBC, 4,
                            {-37, -44}, {-95, -95}, 0xA1B2C3D4u}, u;
  uint8_t b[MR_FRAME_HDR_LEN];
  CHECK_EQ(mr_pack_frame_hdr(b, &m), MR_FRAME_HDR_LEN);
  static const uint8_t want[20] = {0x4D, 0x52, 0x03, 0x01, 0x04, 0x03, 0x02, 0x01,
                                   136, 2, 0x15, 4, 0xDB, 0xD4, 0xA1, 0xA1,
                                   0xD4, 0xC3, 0xB2, 0xA1};
  CHECK(memcmp(b, want, sizeof want) == 0);
  CHECK_EQ(mr_parse_frame_hdr(b, sizeof b, &u), MR_OK);
  CHECK_EQ(u.seq, 0x01020304u); CHECK_EQ(u.rx_channel, 136); CHECK_EQ(u.sec, 2);
  CHECK_EQ(u.flags, 0x15); CHECK_EQ(u.mcs, 4);
  CHECK_EQ(u.rssi[0], -37); CHECK_EQ(u.rssi[1], -44);
  CHECK_EQ(u.noise[0], -95); CHECK_EQ(u.noise[1], -95); CHECK_EQ(u.tsf_lo, 0xA1B2C3D4u);
  CHECK_EQ(mr_parse_frame_hdr(b, 19, &u), MR_ESHORT);
}

static void golden_tune(void) {
  struct mr_tune t = {0xBEEF, 149, 1}, u;
  uint8_t b[MR_TUNE_LEN];
  CHECK_EQ(mr_pack_tune(b, &t), MR_TUNE_LEN);
  static const uint8_t want[] = {0x4D, 0x52, 0x03, 0x03, 0xEF, 0xBE, 149, 1};
  CHECK(memcmp(b, want, sizeof want) == 0);
  CHECK_EQ(mr_parse_tune(b, sizeof b, &u), MR_OK);
  CHECK_EQ(u.tune_id, 0xBEEF); CHECK_EQ(u.channel, 149); CHECK_EQ(u.sec, 1);
}

static void golden_status(void) {
  struct mr_status s = {7, 0, 136, 2, 1, 1, 0x11223344u, 2, 3, 4, 5, 0xA0B0C0D0u,
                        0x01020304u, 6, 0xE0F00010u}, u;
  uint8_t b[MR_STATUS_LEN];
  CHECK_EQ(MR_STATUS_LEN, 47);
  CHECK_EQ(mr_pack_status(b, &s), MR_STATUS_LEN);
  static const uint8_t head[] = {0x4D, 0x52, 0x03, 0x04, 7, 0, 0, 136, 2, 1, 1,
                                 0x44, 0x33, 0x22, 0x11};
  CHECK(memcmp(b, head, sizeof head) == 0);
  static const uint8_t tail[] = {0xD0, 0xC0, 0xB0, 0xA0,     /* uptime_s */
                                 0x04, 0x03, 0x02, 0x01,     /* tx */
                                 0x06, 0x00, 0x00, 0x00,     /* tx_fail */
                                 0x10, 0x00, 0xF0, 0xE0};    /* tx_refused */
  CHECK(memcmp(b + MR_STATUS_LEN - 16, tail, 16) == 0);
  CHECK_EQ(mr_parse_status(b, sizeof b, &u), MR_OK);
  CHECK_EQ(u.tune_id, 7); CHECK_EQ(u.channel, 136); CHECK_EQ(u.rx, 0x11223344u);
  CHECK_EQ(u.fwd, 2); CHECK_EQ(u.foreign, 3); CHECK_EQ(u.bad_fcs, 4);
  CHECK_EQ(u.your_drops, 5); CHECK_EQ(u.uptime_s, 0xA0B0C0D0u);
  CHECK_EQ(u.tx, 0x01020304u); CHECK_EQ(u.tx_fail, 6); CHECK_EQ(u.tx_refused, 0xE0F00010u);
  CHECK_EQ(mr_parse_status(b, MR_STATUS_LEN - 1, &u), MR_ESHORT);
}

/* TX, mcs=0, flags=LDPC|STBC, 24-byte probe-req header (the mabur RCF shape). */
static void golden_tx(void) {
  uint8_t b[MR_TX_HDR_LEN + 24];
  CHECK_EQ(mr_pack_tx_hdr(b, 0, MR_TX_LDPC | MR_TX_STBC), MR_TX_HDR_LEN);
  static const uint8_t want[] = {0x4D, 0x52, 0x03, 0x05, 0x00, 0x03};
  CHECK(memcmp(b, want, sizeof want) == 0);
  for (int i = 0; i < 24; i++) b[MR_TX_HDR_LEN + i] = (uint8_t)(0x40 + i);
  struct mr_tx t;
  CHECK_EQ(mr_parse_tx(b, sizeof b, &t), MR_OK);
  CHECK_EQ(t.mcs, 0); CHECK_EQ(t.flags, 3); CHECK_EQ(t.dl, 24);
  CHECK(t.d == b + MR_TX_HDR_LEN);
}

static void tx_parse_rejects(void) {
  static uint8_t b[MR_TX_HDR_LEN + MR_TX_DOT11_MAX + 1];
  struct mr_tx t;
  mr_pack_tx_hdr(b, 0, 0);
  CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN - 1, &t), MR_ESHORT);
  CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + 23, &t), MR_EINVAL);            /* dot11 < 24 */
  CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + MR_TX_DOT11_MAX, &t), MR_OK);
  CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + MR_TX_DOT11_MAX + 1, &t), MR_EINVAL);
  b[4] = 8; CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + 24, &t), MR_EINVAL); b[4] = 7;
  CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + 24, &t), MR_OK);
  b[5] = 0x10; CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + 24, &t), MR_EINVAL); b[5] = 0;
  b[3] = MR_TUNE; CHECK_EQ(mr_parse_tx(b, MR_TX_HDR_LEN + 24, &t), MR_ETYPE);
}

static void header_errors(void) {
  uint8_t b[MR_HDR_LEN], type = 0;
  CHECK_EQ(mr_pack_hello(b), MR_HDR_LEN);
  CHECK_EQ(mr_parse_header(b, sizeof b, &type), MR_OK); CHECK_EQ(type, MR_HELLO);
  CHECK_EQ(mr_parse_header(b, 3, &type), MR_ESHORT);
  b[0] = 0x00; CHECK_EQ(mr_parse_header(b, 4, &type), MR_EMAGIC); b[0] = 0x4D;
  b[2] = 2; CHECK_EQ(mr_parse_header(b, 4, &type), MR_EVER); b[2] = 3;
  b[3] = 9; CHECK_EQ(mr_parse_header(b, 4, &type), MR_ETYPE);
  b[3] = 0; CHECK_EQ(mr_parse_header(b, 4, &type), MR_ETYPE);
  b[3] = MR_TX; CHECK_EQ(mr_parse_header(b, 4, &type), MR_OK); CHECK_EQ(type, MR_TX);
  CHECK_EQ(MR_FLAGS_OFFSET, 10);
  struct mr_tune t = {1, 36, 0}; uint8_t tb[MR_TUNE_LEN]; mr_pack_tune(tb, &t);
  CHECK_EQ(mr_parse_tune(tb, MR_TUNE_LEN - 1, &t), MR_ESHORT);
  tb[3] = MR_HELLO; CHECK_EQ(mr_parse_tune(tb, MR_TUNE_LEN, &t), MR_ETYPE);
}

void t_wire(const char *fx) {
  (void)fx;
  golden_frame_hdr(); golden_tune(); golden_status(); golden_tx(); tx_parse_rejects(); header_errors();
}
