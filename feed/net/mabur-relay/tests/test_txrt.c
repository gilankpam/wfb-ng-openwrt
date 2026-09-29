#include <string.h>
#include "t.h"
#include "txrt.h"
#include "wire.h"
#include "rtap.h"

/* MCS0 + LDPC + STBC, 20 MHz: the mabur RCF rate. */
static void golden_rcf(void) {
  uint8_t b[TXRT_LEN];
  CHECK_EQ(txrt_build(b, 0, MR_TX_LDPC | MR_TX_STBC), TXRT_LEN);
  static const uint8_t want[TXRT_LEN] = {0x00, 0x00, 0x0D, 0x00,      /* v0, pad, it_len 13 */
                                         0x00, 0x80, 0x08, 0x00,      /* present: TX_FLAGS|MCS */
                                         0x08, 0x00,                  /* TX_FLAGS = NOACK */
                                         0x37, 0x30, 0x00};           /* MCS known, flags, idx */
  CHECK(memcmp(b, want, sizeof want) == 0);
}

static void flags_map(void) {
  uint8_t b[TXRT_LEN];
  txrt_build(b, 7, MR_TX_SGI | MR_TX_BW40);
  CHECK_EQ(b[10], 0x37); CHECK_EQ(b[11], 0x05); CHECK_EQ(b[12], 7);   /* BW40=1, SGI=0x04 */
  txrt_build(b, 3, 0);
  CHECK_EQ(b[11], 0x00); CHECK_EQ(b[12], 3);
}

/* Our own header must parse as an echo; a normal RX header must not. */
static void echo_detect(void) {
  uint8_t b[TXRT_LEN + 24];
  txrt_build(b, 0, MR_TX_LDPC | MR_TX_STBC);
  memset(b + TXRT_LEN, 0, 24);
  struct rtap_info ri;
  CHECK_EQ(rtap_parse(b, sizeof b, &ri), 0);
  CHECK_EQ(ri.tx_echo, 1);
  CHECK_EQ(ri.mcs, 0);
}

void t_txrt(const char *fx) { (void)fx; golden_rcf(); flags_map(); echo_detect(); }
