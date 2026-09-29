# Verifying mabur-relay on device

On-device acceptance for `mabur-relay`, run against the actual CPE510 and
(for step 2's frame-rate gate) a live drone. Host unit tests, the qemu
big-endian self-test and the loop integration test (`./build.sh test`) cover
everything else; this runbook is what those can't reach — real radio, real
`iw` retune latency, a real browser.

The wire protocol referenced below (`HELLO`/`TUNE`/`STATUS`/`FRAME`, ports
8310/8311) is defined in `docs/mabur-relay-protocol.md`.

> The drone and GS are shared bench equipment — ask before powering either on
> or using them for this run.

## 1. Flash and boot check

From an existing OpenWrt CPE510:

```sh
sysupgrade -n /tmp/mabur-relay-tplink_cpe510-v2-squashfs-sysupgrade.bin
```

(use `-n`, do not keep settings; pick the `-sysupgrade.bin` matching the
board's hardware revision). After it reboots:

```sh
ssh root@192.168.1.1
logread | grep mabur-relay     # expect: "up: mon0 on 136 HT40-"
ls /etc/rc.d | grep mabur-relay  # expect: S99mabur-relay
iw dev                          # expect: mon0, type monitor
```

## 2. Throwaway client: frame rate and relay-loss gate

Run on the host wired to the CPE (host NIC on the `192.168.1.0/24` subnet,
e.g. `192.168.1.10/24`). Save as `relayprobe.py`:

```python
#!/usr/bin/env python3
# relayprobe.py HOST [CH SEC] -- subscribe, optionally tune, count 10 s of frames.
import socket, struct, sys, time
host = sys.argv[1]; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20); s.settimeout(0.2)
hello = struct.pack('<HBB', 0x524D, 1, 2)
if len(sys.argv) > 3:
    s.sendto(struct.pack('<HBBHBB', 0x524D, 1, 3, 1, int(sys.argv[2]), int(sys.argv[3])), (host, 8310))
n = gaps = 0; last = None; t0 = time.time(); th = 0
while time.time() - t0 < 10:
    if time.time() - th > 0.5: s.sendto(hello, (host, 8310)); th = time.time()
    try: m = s.recv(65536)
    except socket.timeout: continue
    if m[3] == 4:
        tid, st, ch, sec = struct.unpack('<HBBB', m[4:9]); print('STATUS id', tid, 'state', st, 'ch', ch, 'sec', sec); continue
    seq = struct.unpack('<I', m[4:8])[0]
    if last is not None and seq != last + 1: gaps += seq - last - 1
    last = seq; n += 1
print(f'frames {n} ({n/10:.0f}/s) relay seq gaps {gaps} ({100*gaps/max(n+gaps,1):.3f} %)')
```

With the drone flying (or benched and transmitting) on channel 136 HT40−:

```sh
python3 relayprobe.py 192.168.1.1 136 2
```

While it runs, on the CPE:

```sh
top -bn1 | head -3
```

**Gates:**
- `relayprobe.py`'s `frames/s` is within 5 % of the Realtek GS card's
  `frames` delta over the same 10 s window (read from the `maburgs` stats
  line or sideport).
- relay `seq` gap rate < 0.1 %.
- CPE CPU < 50 % during the run (`top -bn1` above).

## 3. Retune latency

Extend the probe (or loop invocations of it) to send 20 alternating
`TUNE 136 2` / `TUNE 149 1` requests and time each `TUNE` send to the matching
`STATUS state=0` (tuned) for that `tune_id`.

**Gate:** each retune < 150 ms, `TUNE` to `STATUS state=0`.

## 4. Browser Spotter smoke test

Serve a one-file page from the host, over plain HTTP so it can open a plain
`ws://` connection (a hosted HTTPS page cannot — mixed content):

```sh
python3 -m http.server -d /tmp 8000
```

The page:
- opens `new WebSocket('ws://192.168.1.1:8311')` with `binaryType =
  'arraybuffer'`;
- sends the `HELLO` message every 500 ms;
- prints incoming `STATUS` messages and a rolling frames/s count from `FRAME`
  messages.

Open `http://127.0.0.1:8000/<page>.html` in a browser on the host.

**Gate:** with `relayprobe.py` from step 2 still running (it is the tune
owner as the longer-lived UDP subscriber), the page's own `TUNE` attempt gets
back `STATUS state=3` (refused, not owner).

## 5. Restore

None needed — `mabur-relay` is the image's own service; there is no
config to revert.

## Acceptance 2026-09-29

Bench run: the relay binary was run by hand from `/tmp` on the CPE's old
(wfb-ng) image; the `sysupgrade` + boot/procd path (step 1 above) was not
exercised.

**Setup:** CPE510 v3 on current wfb-ng firmware; mabur-relay cross-built from feat/mabur-relay 83cafad with 25.12.4 SDK toolchain (-Os, 31 KB); mon0 created by hand with `flags fcsfail otherbss`; channel 136 HT40−, drone in low-rate mode at ~110 frames/s. Host on CPE's LAN via USB NIC (192.168.1.101).

**Frames (30 s window):** relay 3228 frames (108/s), relay seq gaps 0 (0.000%), ch0 timestamps 0; GS Realtek cards over same window: c0 2742, c1 2670 → relay >= Realtek cards (gate: within 5%) **PASS**.

**Relay STATUS counters:** rx 7591, fwd 7590, foreign 0, bad_fcs 1, drops 0.

**CPE CPU:** 4% (gate < 50%) **PASS**.

**Retune latency:** 20 alternating `TUNE 136 2` ↔ `TUNE 149 1` requests; all 20 completed; min 45 ms / median 46 ms / max 49 ms (gate < 150 ms) **PASS**; state file reads "136 HT40−" after run.

**WS Spotter smoke test:** Python WS client in lieu of browser; handshake 101, 372 frames over 3 s. With `relayprobe.py` UDP subscriber still owning the tune, WS `TUNE` attempt → `STATUS state=3, tune_id 77, owner 1, you_own 0`, radio remained on 136 **PASS**.

**Not exercised:** high-rate video (drone was at ~110 frames/s, so IP-fragment loss at full rate is unmeasured); sysupgrade flash and boot/procd path (user's step).

**Open pre-merge items:**

1. ~~Runbook step 1~~ — **done 2026-09-29**, see "Flash + boot check" below.
2. A full-rate run has not been done: drone on a high rung (~3,000 frames/s,
   not this run's ~110 frames/s), checking relay `seq` gaps, CPE CPU, and the
   new `rxdrop` counter (F1) under that load — the IP-fragment loss and RX
   socket buffer sizing this fix wave targets are only exercised at rate.

## Flash + boot check 2026-09-29

CPE510 v3 flashed with `sysupgrade -n` from master `b0a3039`
(`openwrt-25.12.4-ath79-generic-tplink_cpe510-v3-squashfs-sysupgrade.bin`,
7,803,748 B; `sysupgrade -T` OK). No drone on air for this check.

- Boot: `S99mabur-relay` + `K10mabur-relay` in `/etc/rc.d`; `mon0` type monitor
  on 136 HT40- (boot channel); `logread`: `up: mon0 on 136 HT40-, udp 8310 ws 8311`;
  no `wfb_rx`. **PASS**
- Kmods: `kmod-ath9k` and `kmod-mac80211` both `-r4` (our patched builds). **PASS**
- Respawn: `kill -9` of the daemon → procd restarted it (new pid) within 7 s. **PASS**
- `/etc/init.d/mabur-relay stop` → daemon gone and `mon0` removed; `start` →
  daemon up, `mon0` back on 136 HT40-. **PASS**
- Live `HELLO` from the host → 35-byte `STATUS` (state 0, ch 136, sec 2, owner UDP,
  you_own 1). **PASS**
- Footprint: 29.7 MB RAM free, overlay 2.3 MB free.

Full-rate run: see below.

## Full-rate run 2026-09-29

Flashed CPE (master `b0a3039` image), drone low-power mode temporarily disabled
(restored afterwards), link on 136 HT40- at mcs4/40, drone ~3,100 frames/s.
30 s windows, host on the CPE LAN.

| | UDP subscriber only | UDP + one WS subscriber | WS subscriber only |
|---|---|---|---|
| Relay frames read | 92,832 (3,094/s, 36.1 Mb/s) | 48,181 (1,606/s) | 72,766 (2,426/s) |
| GS Realtek cards, same window | 91,396 / 90,496 | — | — |
| Datagrams IP-fragmented | 96 % | 98 % | n/a (TCP) |
| Relay `seq` gaps (relay→client) | 0 | 0 (UDP), 0 (WS) | 0 |
| RX-socket drops (`rx socket dropped`) | 0 | ~1,000–1,700 frames/s | ~12,200 in the window (~400–650/s) |
| CPE CPU | **65 %** (relay ~75 % in `top`) | **100 %** (saturated) | **95 %** (relay ~85 %) |

- **One UDP subscriber at full rate: works, no loss anywhere, but CPU 65 % —
  FAILS the < 50 % gate** (headroom ~35 %).
- **WS alone at full rate: also NOT viable** — 95 % CPU and ~20 % of frames
  lost at the RX socket; the WS path costs more than the UDP path.
- **UDP + WS at full rate: NOT viable as built.** The relay saturates the
  560 MHz MIPS and loses about half the air frames at the RX socket; the
  `rxdrop` warning makes this visible, as intended.
- Cost drivers not yet profiled (no `perf` on the image). Candidates: one
  `recv` + one `sendmsg` syscall per frame per subscriber, kernel IP
  fragmentation of every ~1.5 KB datagram (54 B radiotap pushes 1,435 B frames
  over the 1,472 B UDP payload), and the WS path's per-frame slot copy + TCP.

