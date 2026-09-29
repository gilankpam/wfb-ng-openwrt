# mabur-relay wire protocol (version 1)

This is the committed contract between `mabur-relay` (this repo,
`feed/net/mabur-relay/src/`) and its clients — the mabur-side `RemoteCard` in
`maburgs` and the web GS's WebSocket source. It is copied verbatim from
`docs/superpowers/specs/2026-09-29-mabur-relay-design.md` (dev-machine only,
gitignored); this file is the one that ships. The source of truth for the
byte layout is `feed/net/mabur-relay/src/wire.c` plus the golden vectors in
`feed/net/mabur-relay/tests/test_wire.c`.

## 2. Wire protocol (version 1)

One message format on both transports: one message per UDP datagram, one
message per binary WebSocket message. **All fields little-endian, serialised
field by field** — the CPE is big-endian MIPS; never `memcpy` a struct.
Radiotap is little-endian by spec and passes through untouched.

**Common header (4 B):** `magic u16 = 0x524D` (bytes `4D 52`, "MR"),
`ver u8 = 1`, `type u8`.

| type | dir | body |
|---|---|---|
| `1 FRAME` | relay → client | `seq u32`, `rx_channel u8`, `sec u8`, `flags u8`, then radiotap + dot11 frame verbatim |
| `2 HELLO` | client → relay | empty |
| `3 TUNE` | client → relay | `tune_id u16`, `channel u8`, `sec u8` |
| `4 STATUS` | relay → client | `tune_id u16`, `state u8`, `channel u8`, `sec u8`, `owner u8`, `you_own u8`, then `u32`: `rx`, `fwd`, `foreign`, `bad_fcs`, `your_drops`, `uptime_s` |
| `5 TX` | client → relay | reserved (phase 2); ignored and counted |

Field semantics:

- `sec`: 0 = HT20, 1 = HT40+, 2 = HT40−. The client names the secondary; the
  relay computes no pairing.
- `seq`: incremented once per forwarded frame, shared across subscribers — a
  gap means relay→client loss (UDP / IP fragment / WS queue), distinct from air
  loss (visible in mabur's own dot11 seq).
- `rx_channel`: channel the radio was known to be on when the frame was read;
  **0 while a retune is in flight** (mirrors `RadioFrontend`'s `rx_channel`
  stamp). `sec` alongside it is the matching width.
- `flags`: bit0 = bad FCS (copied from radiotap); bit1 = this client had at
  least one frame dropped since its previous delivered frame. Other bits 0.
  Frame bytes are identical across subscribers except this per-client byte.
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

**Validation:** wrong magic, a short message, or unknown type → dropped and
counted. `ver` ≠ 1 → dropped and counted; the client learns the relay's version
from any `STATUS` (it answers every `HELLO`). No compatibility shims.

**MTU:** 11 B relay header + 54 B radiotap + 1,435 B frame exceeds a 1,472 B
UDP payload, so large video frames travel as **two IP fragments** on UDP. On
the direct cable the kernel handles this cheaply and `seq` exposes any loss;
the acceptance run measures it. Fallback if it fails the gate: raise the link
MTU. WebSocket (TCP) is unaffected.

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
   sequences end on the latest target.

## Client obligations

- Send `HELLO` every 500 ms; a silent client is dropped after 2 s.
- Treat `FRAME.rx_channel == 0` as "channel unknown" (mid-retune).
- Detect relay→client loss from `FRAME.seq` gaps and `flags` bit1; detect
  air loss from the dot11 sequence number inside the frame.
- Parse radiotap yourself. On ath9k A-MPDU traffic only MCS is valid on every
  subframe; bandwidth/GI/STBC and signal are valid only on the aggregate's last
  subframe — take width from the tuned `sec`, not radiotap. EVM is absent on
  long frames; noise floor is a constant −95 dBm on AR9344.
- Only the owner's `TUNE` moves the radio; watch `STATUS.you_own`.

## Golden byte vectors

From `feed/net/mabur-relay/tests/test_wire.c` (pinned so a big-endian MIPS
write regression fails the host tests and the qemu big-endian self-test the
same way):

**FRAME header**, `seq=0x01020304`, `rx_channel=136`, `sec=2` (HT40−),
`flags=MR_FLAG_BADFCS`:

```
4D 52 01 01 04 03 02 01 88 02 01
└magic┘ver type └──seq (LE)───┘ch sec fl
```

**TUNE**, `tune_id=0xBEEF`, `channel=149`, `sec=1` (HT40+):

```
4D 52 01 03 EF BE 95 01
└magic┘ver type └tid┘ch sec
```
