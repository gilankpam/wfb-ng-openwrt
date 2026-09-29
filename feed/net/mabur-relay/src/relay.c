#define _GNU_SOURCE   /* SOCK_CLOEXEC, accept4, pipe2 (musl has them under -std=gnu99 too) */
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
#include <sys/uio.h>
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
#include "ws.h"
#include <netinet/tcp.h>

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

#define MAX_WS 2
#define WS_QCAP 64
#define WS_SLOT (10 + MR_FRAME_HDR_LEN + RX_MAX)
#define WS_IN_MAX 16384
#define WS_SNDBUF 65536

struct ws_slot { uint16_t len; uint8_t is_status; uint8_t data[WS_SLOT]; };
struct ws_client {
  int fd, used, upgraded;
  uint64_t joined_ms, last_hello_ms;
  uint8_t in[WS_IN_MAX]; size_t in_len;
  struct ws_slot q[WS_QCAP]; int q_head, q_count; size_t q_off;
  uint32_t drops; int dropped;
};

static struct {
  const struct relay_cfg *cfg;
  int rx_fd, udp_fd, ws_fd, sig_rd, sig_wr;
  struct udp_sub udp[MAX_UDP];
  struct ws_client ws[MAX_WS];
  struct tune_st tn;
  struct owner owner;
  uint32_t seq, rx, fwd, foreign, bad_fcs, malformed, bad_msg, refused, rxdrop;
  uint64_t start_ms;
} R;

static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void on_sigchld(int s) { (void)s; int e = errno; (void)!write(R.sig_wr, "c", 1); errno = e; }

static int set_nonblock(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

/* FORCE bypasses the rmem_max/wmem_max admin cap (root/CAP_NET_ADMIN only);
 * fall back to the plain (capped) setsockopt so host tests (run as a normal
 * user, EPERM on FORCE) still get a real, just smaller, buffer. */
static void set_rcvbuf(int fd, int bytes) {
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof bytes) < 0)
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
}

static void set_sndbuf(int fd, int bytes) {
  if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &bytes, sizeof bytes) < 0)
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
}

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

/* Batching (Task 4): one rx_drain() pass fills these, then flush_batch()
 * sends the whole pass to each subscriber in one sendmmsg()/writev() call.
 * RX buffers must stay valid until the flush, hence one slot per frame
 * rather than a single reused buffer. Both are file-scope statics (like the
 * old per-call static buf[]) rather than stack arrays: 64 * 4096 B would be
 * a heavy stack frame on the CPE's MIPS stack. Single-threaded, so reuse
 * across rx_drain() calls is safe. */
static uint8_t rxbuf[RX_BATCH][RX_MAX];
static struct { uint8_t hdr[MR_FRAME_HDR_LEN]; const uint8_t *d; size_t dl; } batch[RX_BATCH];

static void flush_batch(int nb);   /* defined after ws_enqueue/ws_flush below */

/* Packs one frame's header and appends {hdr, dot11 pointer/len} to batch[];
 * does not send. Counters (fwd/bad_fcs) and seq assignment happen here,
 * once per forwarded frame, same as the old per-frame forward(). */
static void batch_append(int *nb, const uint8_t *pkt, size_t len, const struct rtap_info *ri) {
  int i = *nb;
  R.fwd++;
  if (ri->bad_fcs) R.bad_fcs++;
  size_t dl = len - ri->rt_len;
  if (ri->has_fcs && dl >= 28) dl -= 4;
  const uint8_t *d = pkt + ri->rt_len;
  uint8_t base_flags = (uint8_t)((ri->bad_fcs ? MR_FLAG_BADFCS : 0) | ri->phy_flags);
  struct mr_frame_meta fm = {R.seq++, (uint8_t)(R.tn.busy ? 0 : R.tn.channel), R.tn.sec,
                             base_flags, ri->mcs,
                             {ri->rssi[0], ri->rssi[1]}, {ri->noise[0], ri->noise[1]}, ri->tsf_lo};
  mr_pack_frame_hdr(batch[i].hdr, &fm);
  batch[i].d = d; batch[i].dl = dl;
  *nb = i + 1;
}

static int rx_drain(void) {
  int nb = 0;
  for (int i = 0; i < RX_BATCH; i++) {
    uint8_t *buf = rxbuf[i];
    ssize_t n = recv(R.rx_fd, buf, sizeof rxbuf[i], MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
      if (errno == ENETDOWN || errno == ENODEV || errno == ENXIO) {
        syslog(LOG_ERR, "rx: %s gone (%s)", R.cfg->mon, strerror(errno));
        return -1;   /* fatal: caller exits the process, nothing to flush */
      }
      break;
    }
    R.rx++;
    if ((size_t)n > sizeof rxbuf[i]) { R.malformed++; continue; }
    struct filter_result f = filter_frame(buf, (size_t)n);
    if (f.v == FV_MALFORMED) R.malformed++;
    else if (f.v == FV_FOREIGN) R.foreign++;
    else batch_append(&nb, buf, (size_t)n, &f.ri);
  }
  flush_batch(nb);
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

/* ---------- WS ---------- */

static void ws_close(int i) {
  if (!R.ws[i].used) return;
  close(R.ws[i].fd);
  R.ws[i].used = 0;
  syslog(LOG_INFO, "ws #%d closed", i);
}

/* Enqueue one relay message. Frames drop (newest) when full; STATUS evicts the
 * oldest frame not partially written. Returns 0 queued, -1 dropped. */
static int ws_enqueue(int i, const uint8_t *a, size_t al, const uint8_t *b, size_t bl, int is_status) {
  struct ws_client *c = &R.ws[i];
  if (c->q_count == WS_QCAP) {
    if (!is_status) return -1;
    int victim = -1;
    for (int k = 0; k < c->q_count; k++) {
      int s = (c->q_head + k) % WS_QCAP;
      if (k == 0 && c->q_off > 0) continue;
      if (!c->q[s].is_status) { victim = k; break; }
    }
    if (victim < 0) return -1;
    for (int k = victim; k < c->q_count - 1; k++)
      c->q[(c->q_head + k) % WS_QCAP] = c->q[(c->q_head + k + 1) % WS_QCAP];
    c->q_count--;
    c->drops++; c->dropped = 1;
  }
  struct ws_slot *s = &c->q[(c->q_head + c->q_count) % WS_QCAP];
  size_t h = ws_frame_header(s->data, al + bl, WS_OP_BINARY);
  memcpy(s->data + h, a, al);
  if (bl) memcpy(s->data + h + al, b, bl);
  s->len = (uint16_t)(h + al + bl); s->is_status = (uint8_t)is_status;
  c->q_count++;
  return 0;
}

/* writev() the whole pending queue in one syscall instead of one send() per
 * slot. SIGPIPE is ignored process-wide (main.c), so a peer RST just gets us
 * EPIPE here, same as the old send(..., MSG_NOSIGNAL). */
static void ws_flush(int i) {
  struct ws_client *c = &R.ws[i];
  while (c->q_count) {
    int cnt = c->q_count;   /* q_count never exceeds WS_QCAP */
    struct iovec iov[WS_QCAP];
    size_t total = 0;
    for (int k = 0; k < cnt; k++) {
      struct ws_slot *s = &c->q[(c->q_head + k) % WS_QCAP];
      size_t off = (k == 0) ? c->q_off : 0;
      iov[k].iov_base = s->data + off;
      iov[k].iov_len = s->len - off;
      total += iov[k].iov_len;
    }
    ssize_t n = writev(c->fd, iov, cnt);
    if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) return; ws_close(i); return; }
    size_t rem = (size_t)n;
    while (rem > 0 && c->q_count) {
      struct ws_slot *s = &c->q[c->q_head];
      size_t avail = s->len - c->q_off;
      if (rem < avail) { c->q_off += rem; rem = 0; }
      else { rem -= avail; c->q_off = 0; c->q_head = (c->q_head + 1) % WS_QCAP; c->q_count--; }
    }
    if ((size_t)n < total) return;   /* partial write: retry on next POLLOUT */
  }
}

static void ws_reply(int idx, const struct mr_status *s) {
  struct mr_status c = *s; uint8_t b[MR_STATUS_LEN];
  c.your_drops = R.ws[idx].drops;
  mr_pack_status(b, &c);
  ws_enqueue(idx, b, sizeof b, NULL, 0, 1);
  ws_flush(idx);
}

static void ws_broadcast_status(void) {
  for (int i = 0; i < MAX_WS; i++) {
    if (!R.ws[i].used || !R.ws[i].upgraded) continue;
    struct mr_status s; fill_status(&s, 2, i);
    ws_reply(i, &s);
  }
}

static struct owner ws_oldest(void) {
  struct owner o = {0, -1}; uint64_t best = UINT64_MAX;
  for (int i = 0; i < MAX_WS; i++)
    if (R.ws[i].used && R.ws[i].upgraded && R.ws[i].joined_ms < best) {
      best = R.ws[i].joined_ms; o.kind = 2; o.idx = i;
    }
  return o;
}

/* Batched sends for one rx_drain() pass (Task 4). UDP: one sendmmsg() per
 * subscriber carrying all nb frames; only frame 0 carries that subscriber's
 * pending MR_FLAG_DROPPED (the bit means "since your previous *delivered*
 * frame", and frame 0 of this batch is that next delivered frame). A short
 * send (s < nb, including s < 0 treated as 0) counts the rest as dropped and
 * arms the bit for the next batch; a full send clears it. WS: enqueue every
 * frame (same per-client drop/flag bookkeeping as before) without flushing,
 * then one writev()-based ws_flush() per client after the whole pass. */
static void flush_udp(int nb) {
  if (nb == 0) return;
  static uint8_t hdrs[RX_BATCH][MR_FRAME_HDR_LEN];
  static struct iovec iov[RX_BATCH][2];
  static struct mmsghdr mm[RX_BATCH];
  for (int ui = 0; ui < MAX_UDP; ui++) {
    struct udp_sub *u = &R.udp[ui];
    if (!u->used) continue;
    for (int i = 0; i < nb; i++) {
      memcpy(hdrs[i], batch[i].hdr, MR_FRAME_HDR_LEN);
      if (i == 0 && u->dropped) hdrs[i][MR_FLAGS_OFFSET] |= MR_FLAG_DROPPED;
      iov[i][0].iov_base = hdrs[i];             iov[i][0].iov_len = MR_FRAME_HDR_LEN;
      iov[i][1].iov_base = (void *)batch[i].d;  iov[i][1].iov_len = batch[i].dl;
      memset(&mm[i], 0, sizeof mm[i]);
      mm[i].msg_hdr.msg_name = &u->addr;
      mm[i].msg_hdr.msg_namelen = sizeof u->addr;
      mm[i].msg_hdr.msg_iov = iov[i];
      mm[i].msg_hdr.msg_iovlen = 2;
    }
    int s = sendmmsg(R.udp_fd, mm, (unsigned)nb, MSG_DONTWAIT);
    if (s < 0) s = 0;
    if (s < nb) { u->drops += (uint32_t)(nb - s); u->dropped = 1; }
    else u->dropped = 0;
  }
}

static void flush_ws(int nb) {
  uint8_t h[MR_FRAME_HDR_LEN];
  for (int i = 0; i < MAX_WS; i++) {
    struct ws_client *c = &R.ws[i];
    if (!c->used || !c->upgraded) continue;
    for (int k = 0; k < nb; k++) {
      memcpy(h, batch[k].hdr, sizeof h);
      if (c->dropped) h[MR_FLAGS_OFFSET] |= MR_FLAG_DROPPED;
      if (ws_enqueue(i, h, sizeof h, batch[k].d, batch[k].dl, 0) < 0) {
        /* Queue is full (WS_QCAP) only because this pass hasn't flushed
         * yet, not because the client is actually behind: drain once and
         * retry before counting a real drop. */
        ws_flush(i);
        if (!c->used) break;   /* ws_flush may have closed the client */
        if (ws_enqueue(i, h, sizeof h, batch[k].d, batch[k].dl, 0) < 0) {
          /* Still full after a flush-and-retry: this client is stalled
           * for the rest of the pass. Charge every remaining frame
           * (including this one) as a drop in one step and stop touching
           * this client until the next pass, instead of repeating a
           * doomed enqueue/flush per frame. */
          c->drops += (uint32_t)(nb - k); c->dropped = 1;
          break;
        }
        else c->dropped = 0;
      } else c->dropped = 0;
    }
    if (c->used) ws_flush(i);
  }
}

static void flush_batch(int nb) {
  flush_udp(nb);
  flush_ws(nb);
}

static void ws_touch(int idx) { R.ws[idx].last_hello_ms = now_ms(); }

static int ws_open_listener(void) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET; a.sin_port = htons(R.cfg->ws_port); a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
  set_nonblock(fd);
  return fd;
}

static void ws_accept(void) {
  for (;;) {
    int fd = accept4(R.ws_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    int i;
    for (i = 0; i < MAX_WS && R.ws[i].used; i++) {}
    if (i == MAX_WS) { close(fd); R.refused++; continue; }
    set_nonblock(fd);
    int snd = WS_SNDBUF, one = 1;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct ws_client *c = &R.ws[i];
    c->fd = fd; c->used = 1; c->upgraded = 0; c->in_len = 0;
    c->q_head = c->q_count = 0; c->q_off = 0; c->drops = 0; c->dropped = 0;
    c->joined_ms = c->last_hello_ms = now_ms();
    syslog(LOG_INFO, "ws #%d connected", i);
  }
}

static void ws_read(int i) {
  struct ws_client *c = &R.ws[i];
  ssize_t n = recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len, MSG_DONTWAIT);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { ws_close(i); return; }
  if (n < 0) return;
  c->in_len += (size_t)n;
  if (!c->upgraded) {
    char key[WS_KEY_MAX]; size_t used = 0;
    int r = ws_parse_upgrade((const char *)c->in, c->in_len, key, &used);
    if (r < 0) { ws_close(i); return; }
    if (r == 0) { if (c->in_len == sizeof c->in) ws_close(i); return; }
    char resp[256];
    size_t rl = ws_build_response(resp, sizeof resp, key);
    if (rl == 0 || send(c->fd, resp, rl, MSG_NOSIGNAL) != (ssize_t)rl) { ws_close(i); return; }
    c->upgraded = 1; c->joined_ms = c->last_hello_ms = now_ms();
    memmove(c->in, c->in + used, c->in_len - used); c->in_len -= used;
  }
  for (;;) {
    uint8_t op, *pl; size_t plen;
    long used = ws_parse_frame(c->in, c->in_len, &op, &pl, &plen, 512);
    if (used < 0) { ws_close(i); return; }
    if (used == 0) { if (c->in_len == sizeof c->in) ws_close(i); return; }
    if (op == WS_OP_CLOSE) { ws_close(i); return; }
    if (op == WS_OP_BINARY) handle_msg(2, i, pl, plen);
    else if (op == WS_OP_TEXT) { ws_close(i); return; }
    if (!R.ws[i].used) return;
    memmove(c->in, c->in + used, c->in_len - (size_t)used); c->in_len -= (size_t)used;
  }
}

/* Poll slots: [0] listener, then one per used client, in index order. */
static int ws_poll_fill(struct pollfd *p) {
  int n = 0;
  if (R.ws_fd < 0) return 0;
  p[n++] = (struct pollfd){R.ws_fd, POLLIN, 0};
  for (int i = 0; i < MAX_WS; i++)
    if (R.ws[i].used)
      p[n++] = (struct pollfd){R.ws[i].fd, (short)(POLLIN | (R.ws[i].q_count ? POLLOUT : 0)), 0};
  return n;
}

static void ws_poll_handle(const struct pollfd *p, int n) {
  if (n == 0) return;
  int k = 1;
  for (int i = 0; i < MAX_WS && k < n; i++) {
    if (!R.ws[i].used || R.ws[i].fd != p[k].fd) continue;
    short ev = p[k++].revents;
    if (ev & (POLLERR | POLLHUP)) { ws_close(i); continue; }
    if (ev & POLLIN) ws_read(i);
    if (R.ws[i].used && (ev & POLLOUT)) ws_flush(i);
  }
  if (p[0].revents & POLLIN) ws_accept();
}

static void reap_ws(void) {
  uint64_t t = now_ms();
  for (int i = 0; i < MAX_WS; i++)
    if (R.ws[i].used && t - R.ws[i].last_hello_ms > HELLO_TIMEOUT_MS) {
      syslog(LOG_INFO, "ws #%d lapsed", i);
      ws_close(i);
    }
}

/* ---------- sockets ---------- */

static int open_rx(void) {
  if (R.cfg->rx_unix) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", R.cfg->rx_unix);
    unlink(a.sun_path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    return fd;
  }
  /* protocol 0 at socket() time: the socket receives nothing until bind()
   * below switches it to ETH_P_ALL, so there is no window where frames can
   * arrive (and be queued against the default, tiny rcvbuf) before this
   * function has finished setting the real buffer size. */
  int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_ll ll = {0};
  ll.sll_family = AF_PACKET; ll.sll_protocol = htons(ETH_P_ALL);
  ll.sll_ifindex = (int)if_nametoindex(R.cfg->mon);
  if (ll.sll_ifindex == 0 || bind(fd, (struct sockaddr *)&ll, sizeof ll) < 0) { close(fd); return -1; }
  set_rcvbuf(fd, 1 << 20);
  return fd;
}

static int open_udp(void) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
#ifdef SO_NO_CHECK
  /* Skip UDP checksums on outgoing video/telemetry: the Ethernet CRC
   * already covers this one-hop cable, so the UDP checksum only pays for
   * itself as per-byte CPU work on a CPE with no checksum offload. */
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof one);
#endif
  int pmtu = IP_PMTUDISC_DONT;
  /* No SO_REUSEADDR: two relays racing for the same UDP port must fail one
   * of them at bind(), not silently share it. */
  set_sndbuf(fd, 1 << 20);
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
  if (pipe2(pp, O_CLOEXEC) < 0) return 1;
  R.sig_rd = pp[0]; R.sig_wr = pp[1];
  set_nonblock(R.sig_rd); set_nonblock(R.sig_wr);
  struct sigaction sa = {0};
  sa.sa_handler = on_sigchld; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  if (tune_readback(cfg->mon, &R.tn.channel, &R.tn.sec) != 0) {
    R.tn.channel = cfg->boot_channel; R.tn.sec = cfg->boot_sec;
  }
  R.rx_fd = open_rx();
  if (R.rx_fd < 0) { syslog(LOG_ERR, "rx socket setup failed: %s", strerror(errno)); return 1; }
  R.udp_fd = open_udp();
  if (R.udp_fd < 0) { syslog(LOG_ERR, "udp socket setup failed: %s", strerror(errno)); return 1; }
  R.ws_fd = ws_open_listener();
  if (R.ws_fd < 0) { syslog(LOG_ERR, "ws socket setup failed: %s", strerror(errno)); return 1; }
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
    /* RX before reap_child/finish_tune: frames the kernel queued before a
     * retune completed must still be stamped rx_channel=0 (the retune-in-
     * flight value in effect when they arrived), not the new channel that
     * finish_tune() is about to set. */
    if ((p[0].revents & POLLIN) && rx_drain() < 0) return 1;
    if (p[0].revents & (POLLERR | POLLHUP)) { syslog(LOG_ERR, "rx socket error"); return 1; }
    if (p[2].revents & POLLIN) reap_child();
    if (p[1].revents & POLLIN) udp_drain();
    ws_poll_handle(p + ws_first, n - ws_first);
    check_watchdog();
    reap_udp();
    reap_ws();
    update_owner();
    if (now_ms() - last_log >= 1000) {
      last_log = now_ms();
      if (!cfg->rx_unix) {
        struct tpacket_stats st;
        socklen_t sl = sizeof st;
        if (getsockopt(R.rx_fd, SOL_PACKET, PACKET_STATISTICS, &st, &sl) == 0 && st.tp_drops) {
          R.rxdrop += st.tp_drops;
          syslog(LOG_WARNING, "rx socket dropped %u frames", st.tp_drops);
        }
      }
      if (cfg->verbose)
        syslog(LOG_DEBUG, "rx %u fwd %u foreign %u badfcs %u malformed %u badmsg %u refused %u rxdrop %u",
               R.rx, R.fwd, R.foreign, R.bad_fcs, R.malformed, R.bad_msg, R.refused, R.rxdrop);
    }
  }
}
