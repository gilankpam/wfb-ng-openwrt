# mabur-relay wire protocol (version 2)

This is the committed contract between `mabur-relay` (this repo,
`feed/net/mabur-relay/src/`) and its clients — the mabur-side `RemoteCard` in
`maburgs` and the web GS's WebSocket source. It is copied verbatim from
`docs/superpowers/specs/2026-09-29-mabur-relay-design.md` (dev-machine only,
gitignored); this file is the one that ships. The source of truth for the
byte layout is `feed/net/mabur-relay/src/wire.c` plus the golden vectors in
`feed/net/mabur-relay/tests/test_wire.c`.

## 2. Wire protocol (version 2)

One message format on both transports: one message per UDP datagram, one
message per binary WebSocket message. **All fields little-endian, serialised
field by field** — the CPE is big-endian MIPS; never `memcpy` a struct. The
relay parses radiotap itself, on the CPE; no radiotap bytes cross the wire.

**Common header (4 B):** `magic u16 = 0x524D` (bytes `4D 52`, "MR"),
`ver u8 = 2`, `type u8`. `ver` is 2 for every message type — v1 is removed,
no compatibility shim.

| type | dir | body |
|---|---|---|
| `1 FRAME` | relay → client | 20 B header (below), then the 802.11 frame with the trailing 4-byte FCS stripped |
| `2 HELLO` | client → relay | empty |
| `3 TUNE` | client → relay | `tune_id u16`, `channel u8`, `sec u8` |
| `4 STATUS` | relay → client | `tune_id u16`, `state u8`, `channel u8`, `sec u8`, `owner u8`, `you_own u8`, then `u32`: `rx`, `fwd`, `foreign`, `bad_fcs`, `your_drops`, `uptime_s` |
| `5 TX` | client → relay | reserved (phase 2); ignored and counted |

**FRAME header (20 B), immediately after the 4 B common header:**

| bytes | field |
|---|---|
| 0–3 | common header: magic, `ver=2`, `type=1` |
| 4–7 | `seq u32` LE |
| 8 | `rx_channel u8` (0 = mid-retune) |
| 9 | `sec u8` |
| 10 | `flags u8`: bit0 bad FCS, bit1 dropped-before-this (per client), bit2 phy_valid, bit3 short GI, bit4 STBC, bit5 LDPC, bit6 40 MHz |
| 11 | `mcs u8` (`0xFF` = unknown) |
| 12–13 | `rssi[2] int8` dBm, per chain (`0x80` = absent) |
| 14–15 | `noise[2] int8` dBm, per chain (`0x80` = absent) |
| 16–19 | `tsf_lo u32` LE (radiotap TSFT, low 32 bits) |

then the 802.11 frame, FCS-stripped when the source radiotap had it (see
below).

Field semantics:

- `sec`: 0 = HT20, 1 = HT40+, 2 = HT40−. The client names the secondary; the
  relay computes no pairing.
- `seq`: incremented once per forwarded frame, shared across subscribers — a
  gap means relay→client loss (UDP / IP fragment / WS queue), distinct from air
  loss (visible in mabur's own dot11 seq).
- `rx_channel`: channel the radio was known to be on when the frame was read;
  **0 while a retune is in flight** (mirrors `RadioFrontend`'s `rx_channel`
  stamp). `sec` alongside it is the matching width.
- `flags`: bit0 bad FCS (copied from radiotap, when present); bit1 this client
  had at least one frame dropped since its previous delivered frame; bit2
  `phy_valid` — the first radiotap namespace carried `DBM_ANTSIGNAL` (on
  ath9k A-MPDU traffic, only the last subframe); bit3 short GI; bit4 STBC;
  bit5 LDPC; bit6 40 MHz. Bits 3–6 and `rssi`/`noise` are meaningful only
  when bit2 is set; `mcs` is valid on every frame regardless of `phy_valid`.
  The `flags` byte (and `dropped-before-this`, bit1) is per-client; every
  other FRAME field is identical across subscribers.
- `mcs`: the MCS index from radiotap; `0xFF` when absent/unknown.
- `rssi[2]` / `noise[2]`: per-chain (antenna 0/1) signal and noise in dBm,
  read from the extended radiotap namespaces whose `ANTENNA` field is 0 or 1;
  `0x80` (-128) means that chain's reading is absent. EVM is not carried —
  the ar9003 hardware only measures it reliably on short frames, so it is
  meaningless for the video stream and stays CPE-local.
- `tsf_lo`: the low 32 bits of the first radiotap namespace's TSFT.
- `STATUS.state`: 0 tuned, 1 retuning, 2 failed, 3 refused (not owner, or
  subscriber table full). `owner`: 0 none, 1 UDP, 2 WS. `you_own`: 1 if the
  recipient is the owner.
- `STATUS.tune_id`: the request the status concerns — the in-flight one for
  state 1, the last finished one for 0/2 (0 before any `TUNE`), and the
  refused request's own id for 3 (a refusal goes only to the requester and
  does not change the relay's tune state). The reply to an *invalid* `TUNE`
  (state 2, sent only to the requester) likewise carries that request's own
  id, even while another retune is in flight.
- `STATUS` is sent to all subscribers on retune start, completion/failure and
  ownership change, and to the sender in reply to every `HELLO`.
- `STATUS`'s `u32` counters (`rx`, `fwd`, `foreign`, `bad_fcs`, `your_drops`,
  `uptime_s`) wrap at 2^32 (~15 days at full video rate); a client comparing
  two readings must compute the delta modulo 2^32, not by plain subtraction
  with a sign check.
- A HELLO refused because the subscriber table is full (`state=3`) carries the
  relay's current/last `tune_id` (the same value a `STATUS` would carry for
  state 0/1/2) — `HELLO` has no request id of its own for the refusal to echo
  back, unlike a refused `TUNE`, which carries the request's own `tune_id`.
- A pending `TUNE` that is replaced by a newer one before it runs (see retune
  step 6 below) never gets its own `STATUS`: only the request that is
  actually latest-and-executing generates `state=1`/`0`/`2` traffic. A client
  that sent an intermediate `TUNE` in a rapid sequence should not wait for a
  `STATUS` carrying that request's `tune_id` — it will not arrive.

**Validation:** wrong magic, a short message, or unknown type → dropped and
counted. `ver` ≠ 2 → dropped and counted; the client learns the relay's version
from any `STATUS` (it answers every `HELLO`). No compatibility shims — v1 is
removed.

**RX-socket drops.** The relay's AF_PACKET receive socket can drop frames in
the kernel before `mabur-relay` ever reads them (burst beyond `SO_RCVBUF`,
even after the 1 MiB `SO_RCVBUFFORCE`/`SO_RCVBUF` sizing). These drops are
**not visible to clients in-band** — there is no wire field for them, and to
a client they look identical to air loss visible in the dot11 sequence number
inside the frame. They are counted (`PACKET_STATISTICS`'s `tp_drops`,
accumulated into the relay's own `rxdrop` counter) and logged on the device
only: the `-v` per-second debug line includes `rxdrop N`, and each nonzero
poll additionally logs `rx socket dropped N frames` at `LOG_WARNING`. Diagnose
with `logread | grep mabur-relay` on the CPE, not from client-side counters.

**MTU:** the largest video frame today is 20 + 1,431 + 28 = 1,479 B — one IP
packet; bodies above ~1,400 B still fragment. On the direct cable the kernel
handles fragmentation cheaply and `seq` exposes any loss; the acceptance run
measures it. WebSocket (TCP) is unaffected.

**Ports (defaults):** UDP 8310, WebSocket 8311.

## 3. State, retune, errors (subscribers, tune ownership, retune)

**Subscribers.** Up to 4 UDP (keyed by `addr:port`) and 2 WebSocket clients.
UDP: created by the first `HELLO` or `TUNE`, reaped after **2 s without a
`HELLO`**. WS: lives with its TCP connection and must also `HELLO` within 2 s
(reaps hung tabs). A full UDP table → `STATUS state=3` to the sender, counted.
A full WS table → the new TCP connection is closed at accept (no `STATUS` can
precede the handshake), counted. Accepted WS sockets get `SO_SNDBUF` 64 KiB so
kernel buffering cannot hide a stalled tab from the 64-message queue. Clients send
`HELLO` every 500 ms.

**Tune ownership**, re-evaluated each loop pass: the longest-lived UDP
subscriber; else the oldest WS client; else none. A `TUNE` from a non-owner →
`state=3`, radio untouched. On ownership change the radio stays put. With no
subscribers the radio stays on its last channel (a restarting `maburgs` finds
the CPE where the link was).

**Retune:**

1. Owner sends a valid `TUNE` (channel in the 5 GHz range 36–177, `sec` ∈
   {0,1,2}); invalid → `state=2`, nothing spawned. A channel the phy or
   regdomain disallows is left to `iw` to reject, which surfaces as `state=2`
   in step 4.
2. Set `rx_channel = 0`, broadcast `state=1`, fork/exec
   `iw dev mon0 set channel <ch> <HT20|HT40+|HT40->`.
3. Frames keep flowing, stamped `rx_channel = 0`.
4. On child exit (SIGCHLD → self-pipe in `poll`): exit 0 → read back
   `iw dev mon0 info` and trust the readback, not the request; set
   `rx_channel`/`sec`, write `/tmp/mabur-relay.state`, broadcast `state=0` with
   the `tune_id`. Non-zero exit or readback ≠ request → broadcast `state=2`
   with the readback channel; radio left as is.
5. Watchdog: a child still running after 1 s is killed → `state=2`.
6. A `TUNE` arriving mid-retune becomes the single pending request (a newer
   one replaces it) and runs when the current one finishes — rapid hop
   sequences end on the latest target. A replaced pending request is simply
   discarded: it never gets a `STATUS` of its own (see the field-semantics
   note above).

## Client obligations

- Send `HELLO` every 500 ms; a silent client is dropped after 2 s.
- After a client restarts (typically a new UDP socket, so a new ephemeral
  source port), its *old* `addr:port` entry is a distinct subscriber slot
  that the relay does not know is gone: it keeps its subscriber slot, and
  keeps tune ownership if it held it, until the relay reaps it (≤ 2 s
  without a `HELLO`). A `TUNE` sent by the newly-started client during that
  window is refused (`state=3`) even though it is, from the operator's point
  of view, the same logical client re-owning the link. Retry on `state=3`
  rather than treating it as a hard refusal.
- The `FRAME` payload's trailing bytes are the dot11 frame with the trailing
  4-byte FCS already stripped by the relay (when the source radiotap's
  `flags` field had bit 4, `0x10`, "frame includes FCS", set — ath9k sets
  this in monitor mode — and the dot11 part was at least 28 B). Do not parse
  radiotap or strip the FCS yourself; there is no radiotap on the wire at
  all in v2.
- **WS subscription starts at the upgrade, not the first `HELLO`.** As soon as
  the WebSocket handshake completes, the client is a subscriber: it starts
  receiving `STATUS` and `FRAME` messages immediately, and — if it is the
  oldest WebSocket client and no UDP subscriber exists — it is already tune
  owner, all before it has sent its first `HELLO`. `HELLO` is purely the 2 s
  keepalive that stops the relay reaping an otherwise-idle connection; it is
  not what makes the connection a subscriber.
- A `Sec-WebSocket-Key` of 64 characters or more is rejected: the relay closes
  the TCP connection without sending an HTTP response (no `101`, no error
  status — the handshake just fails). Real browsers always send a 24-byte
  key, so this only matters to a hand-rolled client.
- Treat `FRAME.rx_channel == 0` as "channel unknown" (mid-retune).
- Detect relay→client loss from `FRAME.seq` gaps and `flags` bit1; detect
  air loss from the dot11 sequence number inside the frame.
- `flags` bits 3–6 (short GI, STBC, LDPC, 40 MHz) and `rssi`/`noise` are
  meaningful only when bit2 (`phy_valid`) is set — on ath9k A-MPDU traffic
  that's only the aggregate's last subframe. `mcs` is valid on every frame
  regardless. Width should still come from the tuned `sec`, not bit6, since
  a channel-width claim only exists on the frames where bit2 is set.
  EVM is not carried on the wire at all.
- Only the owner's `TUNE` moves the radio; watch `STATUS.you_own`.

## Golden byte vectors

From `feed/net/mabur-relay/tests/test_wire.c` (pinned so a big-endian MIPS
write regression fails the host tests and the qemu big-endian self-test the
same way):

**FRAME header**, `seq=0x01020304`, `rx_channel=136`, `sec=2` (HT40−),
`flags=MR_FLAG_BADFCS|MR_FLAG_PHY_VALID|MR_FLAG_STBC` (`0x15`), `mcs=4`,
`rssi=[-37,-44]`, `noise=[-95,-95]`, `tsf_lo=0xA1B2C3D4`:

```
4D 52 02 01 04 03 02 01 88 02 15 04 DB D4 A1 A1 D4 C3 B2 A1
└magic┘ver type └──seq (LE)───┘ch sec fl mcs └rssi┘ └noise┘ └─tsf_lo (LE)──┘
```

**TUNE**, `tune_id=0xBEEF`, `channel=149`, `sec=1` (HT40+):

```
4D 52 02 03 EF BE 95 01
└magic┘ver type └tid┘ch sec
```
