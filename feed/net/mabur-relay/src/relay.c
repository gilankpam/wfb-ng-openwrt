#include "relay.h"
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include "filter.h"
#include "tune.h"
#include "wire.h"

#define MAX_UDP 4
#define HELLO_TIMEOUT_MS 2000
#define WATCHDOG_MS 1000
#define RX_MAX 4096
#define RX_BATCH 64

struct udp_sub { int used; struct sockaddr_in addr; uint64_t joined_ms, last_hello_ms; uint32_t drops; int dropped; };

struct tune_st {
  int busy; pid_t pid; uint64_t started_ms;
  struct mr_tune cur;                 /* in flight */
  int have_pending; struct mr_tune pending;
  uint16_t last_id; uint8_t state;    /* last finished: 0 tuned / 2 failed */
  uint8_t channel, sec;               /* radio position (readback) */
};

/* Owner identity: kind 0 none, 1 UDP, 2 WS; idx into that table. */
struct owner { int kind, idx; };

static struct {
  const struct relay_cfg *cfg;
  int rx_fd, udp_fd, ws_fd, sig_rd, sig_wr;
  struct udp_sub udp[MAX_UDP];
  struct tune_st tn;
  struct owner owner;
  uint32_t seq, rx, fwd, foreign, bad_fcs, malformed, bad_msg, refused;
  uint64_t start_ms;
} R;

static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void on_sigchld(int s) { (void)s; int e = errno; (void)!write(R.sig_wr, "c", 1); errno = e; }

static int set_nonblock(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

/* ---------- status ---------- */

static void fill_status(struct mr_status *s, int kind, int idx) {
  memset(s, 0, sizeof *s);
  s->tune_id = R.tn.busy ? R.tn.cur.tune_id : R.tn.last_id;
  s->state = R.tn.busy ? 1 : R.tn.state;
  s->channel = R.tn.channel; s->sec = R.tn.sec;
  s->owner = (uint8_t)R.owner.kind;
  s->you_own = kind != 0 && R.owner.kind == kind && R.owner.idx == idx;
  s->rx = R.rx; s->fwd = R.fwd; s->foreign = R.foreign; s->bad_fcs = R.bad_fcs;
  s->uptime_s = (uint32_t)((now_ms() - R.start_ms) / 1000);
}

static void udp_send_status(int idx, const struct mr_status *s) {
  uint8_t b[MR_STATUS_LEN];
  mr_pack_status(b, s);
  sendto(R.udp_fd, b, sizeof b, MSG_DONTWAIT, (struct sockaddr *)&R.udp[idx].addr, sizeof R.udp[idx].addr);
}

static void ws_broadcast_status(void);   /* Task 6 */

static void broadcast_status(void) {
  for (int i = 0; i < MAX_UDP; i++) {
    if (!R.udp[i].used) continue;
    struct mr_status s; fill_status(&s, 1, i); s.your_drops = R.udp[i].drops;
    udp_send_status(i, &s);
  }
  ws_broadcast_status();
}

/* ---------- ownership ---------- */

static struct owner ws_oldest(void);     /* Task 6 */

static void update_owner(void) {
  struct owner o = {0, -1};
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < MAX_UDP; i++)
    if (R.udp[i].used && R.udp[i].joined_ms < best) { best = R.udp[i].joined_ms; o.kind = 1; o.idx = i; }
  if (o.kind == 0) o = ws_oldest();
  if (o.kind != R.owner.kind || o.idx != R.owner.idx) {
    R.owner = o;
    syslog(LOG_INFO, "owner -> %s #%d", o.kind == 1 ? "udp" : o.kind == 2 ? "ws" : "none", o.idx);
    broadcast_status();
  }
}

/* ---------- retune ---------- */

static void write_state(void) {
  FILE *f = fopen(R.cfg->state_path, "w");
  if (!f) return;
  fprintf(f, "%u %s\n", (unsigned)R.tn.channel, tune_sec_str(R.tn.sec));
  fclose(f);
}

static void finish_tune(int exit_ok);

static void start_tune(struct mr_tune t) {
  R.tn.busy = 1; R.tn.cur = t; R.tn.started_ms = now_ms();
  R.tn.pid = tune_spawn(R.cfg->mon, t.channel, t.sec);
  syslog(LOG_INFO, "retune id=%u -> %u %s", t.tune_id, t.channel, tune_sec_str(t.sec));
  broadcast_status();
  if (R.tn.pid < 0) finish_tune(0);
}

static void finish_tune(int exit_ok) {
  uint8_t ch, sec;
  int rb = tune_readback(R.cfg->mon, &ch, &sec);
  if (rb == 0) { R.tn.channel = ch; R.tn.sec = sec; }
  int ok = exit_ok && rb == 0 && ch == R.tn.cur.channel && sec == R.tn.cur.sec;
  R.tn.state = ok ? 0 : 2;
  R.tn.last_id = R.tn.cur.tune_id;
  R.tn.busy = 0; R.tn.pid = 0;
  if (ok) write_state();
  syslog(ok ? LOG_INFO : LOG_WARNING, "retune id=%u %s, radio on %u %s", R.tn.cur.tune_id,
         ok ? "done" : "FAILED", (unsigned)R.tn.channel, tune_sec_str(R.tn.sec));
  broadcast_status();
  if (R.tn.have_pending) { R.tn.have_pending = 0; start_tune(R.tn.pending); }
}

static void reap_child(void) {
  char b[16];
  while (read(R.sig_rd, b, sizeof b) > 0) {}
  if (!R.tn.busy || R.tn.pid <= 0) { while (waitpid(-1, NULL, WNOHANG) > 0) {} return; }
  int st;
  pid_t p = waitpid(R.tn.pid, &st, WNOHANG);
  if (p == R.tn.pid) finish_tune(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void check_watchdog(void) {
  if (!R.tn.busy || R.tn.pid <= 0 || now_ms() - R.tn.started_ms < WATCHDOG_MS) return;
  kill(R.tn.pid, SIGKILL);
  waitpid(R.tn.pid, NULL, 0);
  syslog(LOG_WARNING, "retune id=%u watchdog", R.tn.cur.tune_id);
  finish_tune(0);
}

/* A TUNE from (kind, idx). reply(kind, idx, status) sends to that requester only. */
static void reply_to(int kind, int idx, const struct mr_status *s);   /* UDP here, WS in Task 6 */

static void handle_tune(int kind, int idx, const struct mr_tune *t) {
  update_owner();
  struct mr_status s;
  if (R.owner.kind != kind || R.owner.idx != idx) {
    fill_status(&s, kind, idx); s.state = 3; s.tune_id = t->tune_id;
    reply_to(kind, idx, &s); return;
  }
  if (!tune_valid(t->channel, t->sec)) {
    fill_status(&s, kind, idx); s.state = 2; s.tune_id = t->tune_id;
    reply_to(kind, idx, &s); return;
  }
  if (R.tn.busy) { R.tn.pending = *t; R.tn.have_pending = 1; return; }
  start_tune(*t);
}

/* ---------- RX ---------- */

static void ws_send_frame(const uint8_t *hdr, const uint8_t *pkt, size_t len);   /* Task 6 */

static void forward(const uint8_t *pkt, size_t len, int bad_fcs) {
  uint8_t hdr[MR_FRAME_HDR_LEN];
  R.fwd++;
  if (bad_fcs) R.bad_fcs++;
  mr_pack_frame_hdr(hdr, R.seq++, R.tn.busy ? 0 : R.tn.channel, R.tn.sec, bad_fcs ? MR_FLAG_BADFCS : 0);
  for (int i = 0; i < MAX_UDP; i++) {
    struct udp_sub *u = &R.udp[i];
    if (!u->used) continue;
    hdr[MR_FLAGS_OFFSET] = (uint8_t)((bad_fcs ? MR_FLAG_BADFCS : 0) | (u->dropped ? MR_FLAG_DROPPED : 0));
    struct iovec iov[2] = {{hdr, sizeof hdr}, {(void *)pkt, len}};
    struct msghdr m = {0};
    m.msg_name = &u->addr; m.msg_namelen = sizeof u->addr; m.msg_iov = iov; m.msg_iovlen = 2;
    if (sendmsg(R.udp_fd, &m, MSG_DONTWAIT) < 0) { u->drops++; u->dropped = 1; }
    else u->dropped = 0;
  }
  hdr[MR_FLAGS_OFFSET] = bad_fcs ? MR_FLAG_BADFCS : 0;
  ws_send_frame(hdr, pkt, len);
}

static int rx_drain(void) {
  static uint8_t buf[RX_MAX];
  for (int i = 0; i < RX_BATCH; i++) {
    ssize_t n = recv(R.rx_fd, buf, sizeof buf, MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
      if (errno == ENETDOWN || errno == ENODEV || errno == ENXIO) {
        syslog(LOG_ERR, "rx: %s gone (%s)", R.cfg->mon, strerror(errno));
        return -1;
      }
      return 0;
    }
    R.rx++;
    if ((size_t)n > sizeof buf) { R.malformed++; continue; }
    struct filter_result f = filter_frame(buf, (size_t)n);
    if (f.v == FV_MALFORMED) R.malformed++;
    else if (f.v == FV_FOREIGN) R.foreign++;
    else forward(buf, (size_t)n, f.bad_fcs);
  }
  return 0;
}

/* ---------- UDP control ---------- */

static int udp_find(const struct sockaddr_in *a, int create) {
  int free_i = -1;
  for (int i = 0; i < MAX_UDP; i++) {
    if (R.udp[i].used && R.udp[i].addr.sin_addr.s_addr == a->sin_addr.s_addr &&
        R.udp[i].addr.sin_port == a->sin_port) return i;
    if (!R.udp[i].used && free_i < 0) free_i = i;
  }
  if (!create || free_i < 0) return -1;
  struct udp_sub *u = &R.udp[free_i];
  memset(u, 0, sizeof *u);
  u->used = 1; u->addr = *a; u->joined_ms = u->last_hello_ms = now_ms();
  syslog(LOG_INFO, "udp #%d join %s:%u", free_i, inet_ntoa(a->sin_addr), ntohs(a->sin_port));
  return free_i;
}

static void reply_udp_refused(const struct sockaddr_in *a, uint16_t tune_id) {
  struct mr_status s; uint8_t b[MR_STATUS_LEN];
  fill_status(&s, 0, -1); s.state = 3; s.tune_id = tune_id;
  mr_pack_status(b, &s);
  sendto(R.udp_fd, b, sizeof b, MSG_DONTWAIT, (const struct sockaddr *)a, sizeof *a);
  R.refused++;
}

static void ws_reply(int idx, const struct mr_status *s);   /* Task 6 */

static void reply_to(int kind, int idx, const struct mr_status *s) {
  if (kind == 1) { struct mr_status c = *s; c.your_drops = R.udp[idx].drops; udp_send_status(idx, &c); }
  else ws_reply(idx, s);
}

/* Shared by UDP and WS: one relay message from subscriber (kind, idx). */
static void ws_touch(int idx);   /* Task 6 */

static void handle_msg(int kind, int idx, const uint8_t *b, size_t n) {
  uint8_t type;
  if (mr_parse_header(b, n, &type) != MR_OK) { R.bad_msg++; return; }
  if (type == MR_HELLO) {
    if (kind == 1) R.udp[idx].last_hello_ms = now_ms(); else ws_touch(idx);
    update_owner();
    struct mr_status s; fill_status(&s, kind, idx);
    reply_to(kind, idx, &s);
  } else if (type == MR_TUNE) {
    struct mr_tune t;
    if (mr_parse_tune(b, n, &t) != MR_OK) { R.bad_msg++; return; }
    handle_tune(kind, idx, &t);
  } else {
    R.bad_msg++;          /* FRAME/STATUS from a client, or reserved TX */
  }
}

static void udp_drain(void) {
  uint8_t b[512];
  for (;;) {
    struct sockaddr_in a; socklen_t al = sizeof a;
    ssize_t n = recvfrom(R.udp_fd, b, sizeof b, MSG_DONTWAIT, (struct sockaddr *)&a, &al);
    if (n < 0) return;
    uint8_t type;
    if (mr_parse_header(b, (size_t)n, &type) != MR_OK || (type != MR_HELLO && type != MR_TUNE)) {
      R.bad_msg++; continue;
    }
    int i = udp_find(&a, 1);
    if (i < 0) {
      uint16_t tune_id = R.tn.busy ? R.tn.cur.tune_id : R.tn.last_id;
      if (type == MR_TUNE) {
        struct mr_tune t;
        if (mr_parse_tune(b, (size_t)n, &t) == MR_OK) tune_id = t.tune_id;
      }
      reply_udp_refused(&a, tune_id);
      continue;
    }
    handle_msg(1, i, b, (size_t)n);
  }
}

static void reap_udp(void) {
  uint64_t t = now_ms();
  for (int i = 0; i < MAX_UDP; i++)
    if (R.udp[i].used && t - R.udp[i].last_hello_ms > HELLO_TIMEOUT_MS) {
      syslog(LOG_INFO, "udp #%d lapsed", i);
      R.udp[i].used = 0;
    }
}

/* ---------- WS (Task 6 replaces these stubs) ---------- */

static void ws_broadcast_status(void) {}
static struct owner ws_oldest(void) { struct owner o = {0, -1}; return o; }
static void ws_send_frame(const uint8_t *h, const uint8_t *p, size_t l) { (void)h; (void)p; (void)l; }
static void ws_reply(int idx, const struct mr_status *s) { (void)idx; (void)s; }
static void ws_touch(int idx) { (void)idx; }
static int ws_open_listener(void) { return -1; }
static int ws_poll_fill(struct pollfd *p) { (void)p; return 0; }
static void ws_poll_handle(const struct pollfd *p, int n) { (void)p; (void)n; }
static void reap_ws(void) {}

/* ---------- sockets ---------- */

static int open_rx(void) {
  if (R.cfg->rx_unix) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", R.cfg->rx_unix);
    unlink(a.sun_path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    return fd;
  }
  int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) return -1;
  struct sockaddr_ll ll = {0};
  ll.sll_family = AF_PACKET; ll.sll_protocol = htons(ETH_P_ALL);
  ll.sll_ifindex = (int)if_nametoindex(R.cfg->mon);
  if (ll.sll_ifindex == 0 || bind(fd, (struct sockaddr *)&ll, sizeof ll) < 0) { close(fd); return -1; }
  int rcv = 1 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
  return fd;
}

static int open_udp(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int one = 1, snd = 1 << 20, pmtu = IP_PMTUDISC_DONT;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
  setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu);   /* fragment, never DF */
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET; a.sin_port = htons(R.cfg->udp_port); a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
  return fd;
}

int relay_run(const struct relay_cfg *cfg) {
  memset(&R, 0, sizeof R);
  R.cfg = cfg; R.start_ms = now_ms(); R.owner.idx = -1;
  int pp[2];
  if (pipe(pp) < 0) return 1;
  R.sig_rd = pp[0]; R.sig_wr = pp[1];
  set_nonblock(R.sig_rd); set_nonblock(R.sig_wr);
  struct sigaction sa = {0};
  sa.sa_handler = on_sigchld; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  if (tune_readback(cfg->mon, &R.tn.channel, &R.tn.sec) != 0) {
    R.tn.channel = cfg->boot_channel; R.tn.sec = cfg->boot_sec;
  }
  R.rx_fd = open_rx(); R.udp_fd = open_udp(); R.ws_fd = ws_open_listener();
  if (R.rx_fd < 0 || R.udp_fd < 0) { syslog(LOG_ERR, "socket setup failed: %s", strerror(errno)); return 1; }
  syslog(LOG_INFO, "up: %s on %u %s, udp %u ws %u", cfg->mon, (unsigned)R.tn.channel,
         tune_sec_str(R.tn.sec), cfg->udp_port, cfg->ws_port);

  uint64_t last_log = now_ms();
  for (;;) {
    struct pollfd p[16];
    int n = 0;
    p[n++] = (struct pollfd){R.rx_fd, POLLIN, 0};
    p[n++] = (struct pollfd){R.udp_fd, POLLIN, 0};
    p[n++] = (struct pollfd){R.sig_rd, POLLIN, 0};
    int ws_first = n;
    n += ws_poll_fill(p + n);
    if (poll(p, (nfds_t)n, 100) < 0 && errno != EINTR) return 1;
    if (p[2].revents & POLLIN) reap_child();
    if ((p[0].revents & POLLIN) && rx_drain() < 0) return 1;
    if (p[0].revents & (POLLERR | POLLHUP)) { syslog(LOG_ERR, "rx socket error"); return 1; }
    if (p[1].revents & POLLIN) udp_drain();
    ws_poll_handle(p + ws_first, n - ws_first);
    check_watchdog();
    reap_udp();
    reap_ws();
    update_owner();
    if (cfg->verbose && now_ms() - last_log >= 1000) {
      last_log = now_ms();
      syslog(LOG_DEBUG, "rx %u fwd %u foreign %u badfcs %u malformed %u badmsg %u refused %u",
             R.rx, R.fwd, R.foreign, R.bad_fcs, R.malformed, R.bad_msg, R.refused);
    }
  }
}
