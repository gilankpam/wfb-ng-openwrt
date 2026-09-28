#include "filter.h"
#include <string.h>

const uint8_t MABUR_SA[6] = {0x57, 0x42, 0x75, 0x05, 0xd6, 0x00};

#define RT_F_BADFCS 0x40
#define DOT11_HDR_MIN 24
#define DOT11_SA_OFF 10

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Radiotap: u8 version (0), u8 pad, u16 it_len (LE), u32 present words
 * (bit31 = another word follows), then fields. Only TSFT (bit0, u64,
 * 8-aligned) can precede FLAGS (bit1, u8), so FLAGS is found by skipping the
 * present chain and, if present, TSFT. Every read stays inside it_len. */
struct filter_result filter_frame(const uint8_t *pkt, size_t len) {
  struct filter_result r = {FV_MALFORMED, 0, 0};
  if (len < 8 || pkt[0] != 0) return r;
  size_t rt_len = (size_t)pkt[2] | ((size_t)pkt[3] << 8);
  if (rt_len < 8 || rt_len > len) return r;
  if (len - rt_len < DOT11_HDR_MIN) return r;
  uint32_t present0 = le32(pkt + 4);
  size_t off = 4;
  for (;;) {
    if (off + 4 > rt_len) return r;
    uint32_t w = le32(pkt + off);
    off += 4;
    if (!(w & 0x80000000u)) break;
  }
  if (present0 & 0x1) off = ((off + 7) & ~(size_t)7) + 8;     /* TSFT */
  if (present0 & 0x2) {                                         /* FLAGS */
    if (off + 1 > rt_len) return r;
    r.bad_fcs = (pkt[off] & RT_F_BADFCS) != 0;
  }
  r.rt_len = rt_len;
  const uint8_t *sa = pkt + rt_len + DOT11_SA_OFF;
  r.v = (r.bad_fcs || memcmp(sa, MABUR_SA, 6) == 0) ? FV_FORWARD : FV_FOREIGN;
  return r;
}
