#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include "relay.h"

static void usage(void) {
  fprintf(stderr, "usage: mabur-relay -i MON -u UDP_PORT -w WS_PORT -c CH -s SEC "
                  "[-S STATE] [-R UNIX_RX] [-T UNIX_TX] [-v]\n");
  exit(2);
}

int main(int argc, char **argv) {
  struct relay_cfg c = {"mon0", NULL, NULL, 8310, 8311, "/tmp/mabur-relay.state", 136, 2, 0};
  int o;
  while ((o = getopt(argc, argv, "i:u:w:c:s:S:R:T:v")) != -1) {
    switch (o) {
      case 'i': c.mon = optarg; break;
      case 'u': c.udp_port = (uint16_t)atoi(optarg); break;
      case 'w': c.ws_port = (uint16_t)atoi(optarg); break;
      case 'c': c.boot_channel = (uint8_t)atoi(optarg); break;
      case 's': c.boot_sec = (uint8_t)atoi(optarg); break;
      case 'S': c.state_path = optarg; break;
      case 'R': c.rx_unix = optarg; break;
      case 'T': c.tx_unix = optarg; break;
      case 'v': c.verbose = 1; break;
      default: usage();
    }
  }
  signal(SIGPIPE, SIG_IGN);
  openlog("mabur-relay", LOG_PID | LOG_PERROR, LOG_DAEMON);
  return relay_run(&c);
}
