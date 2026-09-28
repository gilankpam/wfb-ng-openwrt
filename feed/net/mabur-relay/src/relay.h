#ifndef RELAY_H
#define RELAY_H
#include <stdint.h>

struct relay_cfg {
  const char *mon;         /* "mon0" */
  const char *rx_unix;     /* NULL = AF_PACKET on mon; else bind a unix SOCK_DGRAM here (tests) */
  uint16_t udp_port, ws_port;
  const char *state_path;  /* "/tmp/mabur-relay.state" */
  uint8_t boot_channel, boot_sec;   /* used only if the startup readback fails */
  int verbose;
};
int relay_run(const struct relay_cfg *cfg);   /* returns only on fatal error: 1 */

#endif
