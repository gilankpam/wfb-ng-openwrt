#ifndef RTAP_H
#define RTAP_H
#include <stddef.h>
#include <stdint.h>

#define RT_ABSENT (-128)
#define RT_MCS_NONE 0xFF
/* phy_flags bits == the v2 FRAME flag bits */
#define RT_PHY_VALID 0x04
#define RT_SGI 0x08
#define RT_STBC 0x10
#define RT_LDPC 0x20
#define RT_BW40 0x40

struct rtap_info {
  size_t rt_len;
  int has_fcs, bad_fcs;
  uint32_t tsf_lo;
  uint8_t mcs;        /* RT_MCS_NONE if absent/unknown */
  uint8_t phy_flags;  /* RT_* bits */
  int8_t rssi[2], noise[2];  /* RT_ABSENT if absent */
};

/* 0 = ok (fields it could not reach stay at their "absent" defaults);
 * -1 = malformed (bad version, it_len < 8 or > len, present chain past it_len,
 *      a known field past it_len). */
int rtap_parse(const uint8_t *pkt, size_t len, struct rtap_info *ri);

#endif
