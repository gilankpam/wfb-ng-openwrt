#ifndef TXRT_H
#define TXRT_H
#include <stddef.h>
#include <stdint.h>

/* Injection radiotap for one mabur uplink frame: v0, present = TX_FLAGS (15)
 * | MCS (19); TX_FLAGS = NOACK; MCS known = BW|MCS|GI|FEC|STBC. mac80211
 * honours all of these for injected frames (ieee80211_parse_tx_radiotap). */
#define TXRT_LEN 13
size_t txrt_build(uint8_t out[TXRT_LEN], uint8_t mcs, uint8_t flags);   /* flags: MR_TX_* */

#endif
