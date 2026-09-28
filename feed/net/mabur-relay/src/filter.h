#ifndef FILTER_H
#define FILTER_H
#include <stddef.h>
#include <stdint.h>

enum filter_verdict { FV_FORWARD = 0, FV_FOREIGN = 1, FV_MALFORMED = 2 };
struct filter_result { enum filter_verdict v; int bad_fcs; size_t rt_len; };
struct filter_result filter_frame(const uint8_t *pkt, size_t len);
extern const uint8_t MABUR_SA[6];   /* 57:42:75:05:d6:00 */

#endif
