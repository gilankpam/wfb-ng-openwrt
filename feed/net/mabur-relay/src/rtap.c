#include "rtap.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Radiotap-namespace field alignment/size for bits 0..27 (0 = unknown). */
static const uint8_t AL[28] = {8,1,1,2,2,1,1,2,2,2,1,1,1,1,2,2,1,1,4,1,4,2,8,2,2,0,1,2};
static const uint8_t SZ[28] = {8,1,1,4,2,1,1,2,2,2,1,1,1,1,2,2,1,1,8,3,8,12,12,12,12,0,1,4};

#define W_RT_NEXT (1u << 29)
#define W_VEND_NEXT (1u << 30)
#define W_EXT (1u << 31)
#define MAX_WORDS 16

int rtap_parse(const uint8_t *pkt, size_t len, struct rtap_info *ri) {
  memset(ri, 0, sizeof *ri);
  ri->mcs = RT_MCS_NONE;
  ri->rssi[0] = ri->rssi[1] = ri->noise[0] = ri->noise[1] = RT_ABSENT;
  if (len < 8 || pkt[0] != 0) return -1;
  size_t rt = le16(pkt + 2);
  if (rt < 8 || rt > len) return -1;
  ri->rt_len = rt;

  uint32_t w[MAX_WORDS]; int nw = 0; size_t off = 4;
  for (;;) {
    if (off + 4 > rt || nw == MAX_WORDS) return -1;
    uint32_t cur = le32(pkt + off); off += 4;
    w[nw++] = cur;
    if (!(cur & W_EXT)) break;
    /* bit31 set but neither W_RT_NEXT nor W_VEND_NEXT: the next word would
     * continue THIS SAME namespace into bits 32-63, which this parser has
     * no field table for. Stop the present-word chain here (this word's
     * own fields, bits 0-28, are still parsed below); no error. */
    if (!(cur & (W_RT_NEXT | W_VEND_NEXT))) break;
  }

  int vendor = 0;               /* the current word describes a vendor namespace */
  int8_t first_sig = RT_ABSENT, first_noi = RT_ABSENT;
  for (int k = 0; k < nw; k++) {
    if (vendor) {
      /* vendor namespace data: align 2, OUI(3) subns(1) skip_length(u16), data */
      off = (off + 1) & ~(size_t)1;
      if (off + 6 > rt) return -1;
      size_t skip = le16(pkt + off + 4);
      off += 6 + skip;
      if (off > rt) return -1;
    } else {
      int8_t sig = RT_ABSENT, noi = RT_ABSENT; int ant = -1;
      for (int b = 0; b < 29; b++) {
        if (!(w[k] & (1u << b))) continue;
        if (b >= 28 || SZ[b] == 0) return 0;           /* unknown: stop, keep what we have */
        size_t a = AL[b];
        off = (off + a - 1) & ~(a - 1);
        if (off + SZ[b] > rt) return -1;
        const uint8_t *p = pkt + off;
        switch (b) {
          case 0: if (k == 0) ri->tsf_lo = le32(p); break;
          case 1: if (k == 0) { ri->has_fcs = (p[0] & 0x10) != 0; ri->bad_fcs = (p[0] & 0x40) != 0; } break;
          case 5: sig = (int8_t)p[0]; break;
          case 6: noi = (int8_t)p[0]; break;
          case 11: ant = p[0]; break;
          case 19:
            if (k == 0) {
              uint8_t known = p[0], f = p[1];
              if (known & 0x02) ri->mcs = p[2];
              if ((known & 0x01) && (f & 0x03) == 1) ri->phy_flags |= RT_BW40;
              if ((known & 0x04) && (f & 0x04)) ri->phy_flags |= RT_SGI;
              if ((known & 0x10) && (f & 0x10)) ri->phy_flags |= RT_LDPC;
              if ((known & 0x20) && (f & 0x60)) ri->phy_flags |= RT_STBC;
            }
            break;
          default: break;
        }
        off += SZ[b];
      }
      if (k == 0) {
        if (sig != RT_ABSENT) ri->phy_flags |= RT_PHY_VALID;
        first_sig = sig; first_noi = noi;
      } else if (ant == 0 || ant == 1) {
        ri->rssi[ant] = sig; ri->noise[ant] = noi;
      }
    }
    vendor = (w[k] & W_VEND_NEXT) != 0;
  }
  /* phy_valid (first namespace carried DBM_ANTSIGNAL) but no extended
   * namespace assigned a per-chain reading: fall back to the combined
   * signal/noise on chain 0 rather than leaving it absent. */
  if ((ri->phy_flags & RT_PHY_VALID) && ri->rssi[0] == RT_ABSENT && ri->rssi[1] == RT_ABSENT) {
    ri->rssi[0] = first_sig; ri->noise[0] = first_noi;
  }
  return 0;
}
