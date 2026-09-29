#include "txrt.h"
#include "wire.h"

#define RT_KNOWN_BW 0x01
#define RT_KNOWN_MCS 0x02
#define RT_KNOWN_GI 0x04
#define RT_KNOWN_FEC 0x10
#define RT_KNOWN_STBC 0x20
#define RT_F_BW40 0x01
#define RT_F_SGI 0x04
#define RT_F_LDPC 0x10
#define RT_F_STBC1 0x20      /* STBC field (bits 5-6) = 1 spatial stream */

size_t txrt_build(uint8_t out[TXRT_LEN], uint8_t mcs, uint8_t flags) {
  out[0] = 0; out[1] = 0; out[2] = TXRT_LEN; out[3] = 0;
  out[4] = 0x00; out[5] = 0x80; out[6] = 0x08; out[7] = 0x00;   /* bits 15 + 19, LE */
  out[8] = 0x08; out[9] = 0x00;                                 /* TX_FLAGS: NOACK */
  out[10] = RT_KNOWN_BW | RT_KNOWN_MCS | RT_KNOWN_GI | RT_KNOWN_FEC | RT_KNOWN_STBC;
  out[11] = (uint8_t)(((flags & MR_TX_BW40) ? RT_F_BW40 : 0) | ((flags & MR_TX_SGI) ? RT_F_SGI : 0) |
                      ((flags & MR_TX_LDPC) ? RT_F_LDPC : 0) | ((flags & MR_TX_STBC) ? RT_F_STBC1 : 0));
  out[12] = mcs;
  return TXRT_LEN;
}
