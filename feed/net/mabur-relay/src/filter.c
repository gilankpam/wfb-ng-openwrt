#include "filter.h"
#include <string.h>

const uint8_t MABUR_SA[6] = {0x57, 0x42, 0x75, 0x05, 0xd6, 0x00};
#define DOT11_HDR_MIN 24
#define DOT11_SA_OFF 10

struct filter_result filter_frame(const uint8_t *pkt, size_t len) {
  struct filter_result r;
  memset(&r, 0, sizeof r);
  r.v = FV_MALFORMED;
  if (rtap_parse(pkt, len, &r.ri) != 0) return r;
  if (len - r.ri.rt_len < DOT11_HDR_MIN) return r;
  r.rt_len = r.ri.rt_len;
  r.bad_fcs = r.ri.bad_fcs;
  const uint8_t *sa = pkt + r.rt_len + DOT11_SA_OFF;
  r.v = (r.bad_fcs || memcmp(sa, MABUR_SA, 6) == 0) ? FV_FORWARD : FV_FOREIGN;
  return r;
}
