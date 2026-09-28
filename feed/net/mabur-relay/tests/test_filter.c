#include <string.h>
#include "t.h"
#include "pcap.h"
#include "filter.h"

static uint8_t *fr[8]; static size_t fl[8];

static void t_filter_fixtures(void) {
  struct filter_result r;
  r = filter_frame(fr[0], fl[0]);
  CHECK_EQ(r.v, FV_FORWARD); CHECK_EQ(r.bad_fcs, 0); CHECK_EQ(r.rt_len, 40);
  r = filter_frame(fr[1], fl[1]);                 /* extended present bitmap */
  CHECK_EQ(r.v, FV_FORWARD); CHECK_EQ(r.bad_fcs, 0); CHECK_EQ(r.rt_len, 43);
  r = filter_frame(fr[2], fl[2]);
  CHECK_EQ(r.v, FV_FOREIGN);
  r = filter_frame(fr[3], fl[3]);                 /* garbage SA, but BADFCS */
  CHECK_EQ(r.v, FV_FORWARD); CHECK_EQ(r.bad_fcs, 1);
}

static void t_filter_malformed(void) {
  uint8_t b[256];
  CHECK_EQ(filter_frame(fr[0], 0).v, FV_MALFORMED);
  CHECK_EQ(filter_frame(fr[0], 7).v, FV_MALFORMED);              /* < radiotap min */
  CHECK_EQ(filter_frame(fr[0], 40 + 23).v, FV_MALFORMED);        /* no room for dot11 hdr */
  CHECK_EQ(filter_frame(fr[0], 40 + 24).v, FV_FORWARD);          /* exactly enough */
  memcpy(b, fr[0], fl[0]);
  b[2] = 0xFF; b[3] = 0x00;                                      /* it_len 255 > 200 */
  CHECK_EQ(filter_frame(b, fl[0]).v, FV_MALFORMED);
  memcpy(b, fr[0], fl[0]);
  b[0] = 1;                                                      /* bad it_version */
  CHECK_EQ(filter_frame(b, fl[0]).v, FV_MALFORMED);
  memcpy(b, fr[0], fl[0]);
  b[2] = 6; b[3] = 0;                                            /* it_len < 8 */
  CHECK_EQ(filter_frame(b, fl[0]).v, FV_MALFORMED);
  memcpy(b, fr[1], fl[1]);                                       /* present chain runs past it_len */
  b[2] = 12; b[3] = 0;
  CHECK_EQ(filter_frame(b, fl[1]).v, FV_MALFORMED);
}

void t_filter(const char *fx) {
  char path[512];
  snprintf(path, sizeof path, "%s/frames.pcap", fx);
  int n = pcap_load(path, fr, fl, 8);
  CHECK_EQ(n, 4);
  if (n != 4) return;
  t_filter_fixtures();
  t_filter_malformed();
}
