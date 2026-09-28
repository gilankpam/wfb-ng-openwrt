#include <string.h>
#include "t.h"
#include "wire.h"

static void golden_frame_hdr(void) {
  uint8_t b[MR_FRAME_HDR_LEN];
  CHECK_EQ(mr_pack_frame_hdr(b, 0x01020304u, 136, 2, MR_FLAG_BADFCS), MR_FRAME_HDR_LEN);
  static const uint8_t want[] = {0x4D, 0x52, 0x01, 0x01, 0x04, 0x03, 0x02, 0x01, 136, 2, 0x01};
  CHECK(memcmp(b, want, sizeof want) == 0);
  uint32_t seq; uint8_t ch, sec, fl;
  CHECK_EQ(mr_parse_frame_hdr(b, sizeof b, &seq, &ch, &sec, &fl), MR_OK);
  CHECK_EQ(seq, 0x01020304u); CHECK_EQ(ch, 136); CHECK_EQ(sec, 2); CHECK_EQ(fl, 1);
}

static void golden_tune(void) {
  struct mr_tune t = {0xBEEF, 149, 1}, u;
  uint8_t b[MR_TUNE_LEN];
  CHECK_EQ(mr_pack_tune(b, &t), MR_TUNE_LEN);
  static const uint8_t want[] = {0x4D, 0x52, 0x01, 0x03, 0xEF, 0xBE, 149, 1};
  CHECK(memcmp(b, want, sizeof want) == 0);
  CHECK_EQ(mr_parse_tune(b, sizeof b, &u), MR_OK);
  CHECK_EQ(u.tune_id, 0xBEEF); CHECK_EQ(u.channel, 149); CHECK_EQ(u.sec, 1);
}

static void golden_status(void) {
  struct mr_status s = {7, 0, 136, 2, 1, 1, 0x11223344u, 2, 3, 4, 5, 0xA0B0C0D0u}, u;
  uint8_t b[MR_STATUS_LEN];
  CHECK_EQ(mr_pack_status(b, &s), MR_STATUS_LEN);
  static const uint8_t head[] = {0x4D, 0x52, 0x01, 0x04, 7, 0, 0, 136, 2, 1, 1,
                                 0x44, 0x33, 0x22, 0x11};
  CHECK(memcmp(b, head, sizeof head) == 0);
  static const uint8_t tail[] = {0xD0, 0xC0, 0xB0, 0xA0};
  CHECK(memcmp(b + MR_STATUS_LEN - 4, tail, 4) == 0);
  CHECK_EQ(mr_parse_status(b, sizeof b, &u), MR_OK);
  CHECK_EQ(u.tune_id, 7); CHECK_EQ(u.channel, 136); CHECK_EQ(u.rx, 0x11223344u);
  CHECK_EQ(u.fwd, 2); CHECK_EQ(u.foreign, 3); CHECK_EQ(u.bad_fcs, 4);
  CHECK_EQ(u.your_drops, 5); CHECK_EQ(u.uptime_s, 0xA0B0C0D0u);
}

static void header_errors(void) {
  uint8_t b[MR_HDR_LEN], type = 0;
  CHECK_EQ(mr_pack_hello(b), MR_HDR_LEN);
  CHECK_EQ(mr_parse_header(b, sizeof b, &type), MR_OK); CHECK_EQ(type, MR_HELLO);
  CHECK_EQ(mr_parse_header(b, 3, &type), MR_ESHORT);
  b[0] = 0x00; CHECK_EQ(mr_parse_header(b, 4, &type), MR_EMAGIC); b[0] = 0x4D;
  b[2] = 2; CHECK_EQ(mr_parse_header(b, 4, &type), MR_EVER); b[2] = 1;
  b[3] = 9; CHECK_EQ(mr_parse_header(b, 4, &type), MR_ETYPE);
  b[3] = 0; CHECK_EQ(mr_parse_header(b, 4, &type), MR_ETYPE);
  struct mr_tune t = {1, 36, 0}; uint8_t tb[MR_TUNE_LEN]; mr_pack_tune(tb, &t);
  CHECK_EQ(mr_parse_tune(tb, MR_TUNE_LEN - 1, &t), MR_ESHORT);
  tb[3] = MR_HELLO; CHECK_EQ(mr_parse_tune(tb, MR_TUNE_LEN, &t), MR_ETYPE);
}

void t_wire(const char *fx) {
  (void)fx;
  golden_frame_hdr(); golden_tune(); golden_status(); header_errors();
}
