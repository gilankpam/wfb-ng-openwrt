#include <string.h>
#include "t.h"
#include "pcap.h"
#include "rtap.h"

static uint8_t *fr[8]; static size_t fl[8];

static void t_rtap_fixtures(void) {
  struct rtap_info ri;
  CHECK_EQ(rtap_parse(fr[0], fl[0], &ri), 0);
  CHECK_EQ(ri.rt_len, 40); CHECK_EQ(ri.has_fcs, 1); CHECK_EQ(ri.bad_fcs, 0);
  CHECK_EQ(ri.tsf_lo, 86499716u); CHECK_EQ(ri.mcs, 4);
  CHECK_EQ(ri.phy_flags, RT_STBC);
  CHECK_EQ(ri.rssi[0], RT_ABSENT); CHECK_EQ(ri.rssi[1], RT_ABSENT);
  CHECK_EQ(ri.noise[0], RT_ABSENT); CHECK_EQ(ri.noise[1], RT_ABSENT);

  CHECK_EQ(rtap_parse(fr[1], fl[1], &ri), 0);          /* 3 present words */
  CHECK_EQ(ri.rt_len, 43); CHECK_EQ(ri.tsf_lo, 86535458u); CHECK_EQ(ri.mcs, 0);
  CHECK_EQ(ri.phy_flags, RT_PHY_VALID | RT_STBC);
  CHECK_EQ(ri.rssi[0], -37); CHECK_EQ(ri.rssi[1], -44);   /* per chain, not combined -36 */
  CHECK_EQ(ri.noise[0], -95); CHECK_EQ(ri.noise[1], -95);

  CHECK_EQ(rtap_parse(fr[3], fl[3], &ri), 0);
  CHECK_EQ(ri.bad_fcs, 1); CHECK_EQ(ri.has_fcs, 1); CHECK_EQ(ri.tsf_lo, 86967089u);
  CHECK_EQ(ri.mcs, RT_MCS_NONE); CHECK_EQ(ri.phy_flags, RT_PHY_VALID);
  CHECK_EQ(ri.rssi[0], -37); CHECK_EQ(ri.rssi[1], -44);
  CHECK_EQ(ri.noise[0], -95); CHECK_EQ(ri.noise[1], -95);
}

static void t_rtap_malformed(void) {
  struct rtap_info ri; uint8_t b[256];
  CHECK_EQ(rtap_parse(fr[0], 7, &ri), -1);
  memcpy(b, fr[0], fl[0]); b[0] = 1;                    CHECK_EQ(rtap_parse(b, fl[0], &ri), -1);
  memcpy(b, fr[0], fl[0]); b[2] = 0xFF; b[3] = 0;       CHECK_EQ(rtap_parse(b, fl[0], &ri), -1);
  memcpy(b, fr[1], fl[1]); b[2] = 12; b[3] = 0;         CHECK_EQ(rtap_parse(b, fl[1], &ri), -1);
  /* it_len cut inside the AMPDU field of frame 0 (AMPDU status is the last
   * field, right after MCS) */
  memcpy(b, fr[0], fl[0]); b[2] = 30; b[3] = 0;         CHECK_EQ(rtap_parse(b, fl[0], &ri), -1);
}

/* A radiotap-namespace bit the parser has no size for (bit 25 is unassigned):
 * parsing stops there; what came before is kept; no read past it_len. */
static void t_rtap_unknown_field(void) {
  uint8_t b[64] = {0};
  b[0] = 0; b[2] = 8 + 8 + 1; b[3] = 0;                /* it_len 17 */
  uint32_t pres = 0x1 | 0x2 | (1u << 25);               /* TSFT, FLAGS, unknown */
  b[4] = pres; b[5] = pres >> 8; b[6] = pres >> 16; b[7] = pres >> 24;
  b[8] = 0x44; b[9] = 0x33; b[10] = 0x22; b[11] = 0x11;  /* TSFT lo */
  b[16] = 0x10;                                          /* FLAGS: FCS present */
  struct rtap_info ri;
  CHECK_EQ(rtap_parse(b, 64, &ri), 0);
  CHECK_EQ(ri.tsf_lo, 0x11223344u); CHECK_EQ(ri.has_fcs, 1); CHECK_EQ(ri.mcs, RT_MCS_NONE);
}

/* A vendor namespace (bit 30 in word 0) is skipped by its skip_length; the
 * radiotap fields before it are still parsed. */
static void t_rtap_vendor_ns(void) {
  uint8_t b[64] = {0};
  /* words: w0 = FLAGS | vendor-next | ext; w1 = vendor word (bits ignored) */
  uint32_t w0 = 0x2 | (1u << 30) | (1u << 31), w1 = 0x1;
  b[4] = w0; b[5] = w0 >> 8; b[6] = w0 >> 16; b[7] = w0 >> 24;
  b[8] = w1; b[9] = w1 >> 8; b[10] = w1 >> 16; b[11] = w1 >> 24;
  b[12] = 0x50;                                          /* FLAGS: FCS + bad FCS */
  /* vendor header at off 14 (align 2): OUI 3 B, subns 1 B, skip_length u16 = 4 */
  b[14] = 0x00; b[15] = 0x11; b[16] = 0x22; b[17] = 0; b[18] = 4; b[19] = 0;
  /* 4 vendor bytes 20..23 */
  b[2] = 24; b[3] = 0;                                   /* it_len 24 */
  struct rtap_info ri;
  CHECK_EQ(rtap_parse(b, 64, &ri), 0);
  CHECK_EQ(ri.bad_fcs, 1); CHECK_EQ(ri.has_fcs, 1);
  b[18] = 40;                                            /* skip past it_len */
  CHECK_EQ(rtap_parse(b, 64, &ri), -1);
}

/* F2: bit31 set on a present word but neither W_RT_NEXT (bit29) nor
 * W_VEND_NEXT (bit30) means the *next* word would continue THIS SAME
 * namespace into bits 32-63, which this parser has no field table for.
 * It must stop the present-word chain there (word 0's own fields still
 * parsed, return 0) instead of misreading that continuation word as a
 * new namespace. */
static void t_rtap_same_ns_continuation(void) {
  uint8_t b[64] = {0};
  b[0] = 0;
  uint32_t w0 = 0x2 | (1u << 31);      /* FLAGS + ext bit only */
  b[4] = w0; b[5] = w0 >> 8; b[6] = w0 >> 16; b[7] = w0 >> 24;
  /* Bytes 8-11 would be read as a second present word by the old code
   * (0x2 | W_RT_NEXT, i.e. "FLAGS + another namespace follows"); the fix
   * must never read them as such. Only byte 8 is touched, as the FLAGS
   * field's own 1-byte data. */
  uint32_t w1 = 0x2 | (1u << 29);
  b[8] = w1; b[9] = w1 >> 8; b[10] = w1 >> 16; b[11] = w1 >> 24;
  b[8] = 0x10;                          /* FLAGS: FCS present, not bad */
  b[2] = 12; b[3] = 0;                  /* it_len 12: covers the leftover bytes too */
  struct rtap_info ri;
  CHECK_EQ(rtap_parse(b, 64, &ri), 0);
  CHECK_EQ(ri.has_fcs, 1); CHECK_EQ(ri.bad_fcs, 0);
  CHECK_EQ(ri.rssi[0], RT_ABSENT); CHECK_EQ(ri.rssi[1], RT_ABSENT);
  CHECK_EQ(ri.noise[0], RT_ABSENT); CHECK_EQ(ri.noise[1], RT_ABSENT);
  CHECK_EQ(ri.phy_flags & RT_PHY_VALID, 0);
}

/* F3: a single-namespace header carrying DBM_ANTSIGNAL + DBM_ANTNOISE (no
 * extended namespace at all) falls back to rssi[0]/noise[0] = the combined
 * reading; rssi[1]/noise[1] stay absent. */
static void t_rtap_phy_valid_single_ns_fallback(void) {
  uint8_t b[64] = {0};
  b[0] = 0;
  uint32_t w0 = (1u << 5) | (1u << 6);  /* DBM_ANTSIGNAL, DBM_ANTNOISE; no ext */
  b[4] = w0; b[5] = w0 >> 8; b[6] = w0 >> 16; b[7] = w0 >> 24;
  b[8] = (uint8_t)(int8_t)-50;          /* combined signal */
  b[9] = (uint8_t)(int8_t)-90;          /* combined noise */
  b[2] = 10; b[3] = 0;                  /* it_len 10 */
  struct rtap_info ri;
  CHECK_EQ(rtap_parse(b, 64, &ri), 0);
  CHECK_EQ(ri.phy_flags & RT_PHY_VALID, RT_PHY_VALID);
  CHECK_EQ(ri.rssi[0], -50); CHECK_EQ(ri.rssi[1], RT_ABSENT);
  CHECK_EQ(ri.noise[0], -90); CHECK_EQ(ri.noise[1], RT_ABSENT);
}

void t_rtap(const char *fx) {
  char path[512];
  snprintf(path, sizeof path, "%s/frames.pcap", fx);
  int n = pcap_load(path, fr, fl, 8);
  CHECK_EQ(n, 4);
  if (n != 4) return;
  t_rtap_fixtures(); t_rtap_malformed(); t_rtap_unknown_field(); t_rtap_vendor_ns();
  t_rtap_same_ns_continuation(); t_rtap_phy_valid_single_ns_fallback();
}
