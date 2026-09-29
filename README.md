# mabur RX relay firmware for TP-Link CPE510

[![build](https://github.com/gilankpam/wfb-ng-openwrt/actions/workflows/build.yml/badge.svg)](https://github.com/gilankpam/wfb-ng-openwrt/actions/workflows/build.yml)

**[⬇ Download the latest firmware](https://github.com/gilankpam/wfb-ng-openwrt/releases/latest)** — built by CI from every push to `master` (rolling `latest` prerelease).

Minimal OpenWrt firmware that turns a TP-Link CPE510 into a **remote receive
card** for mabur, an RTP-free FPV video link: the CPE hears the drone's
downlink on its panel antenna and forwards every mabur frame — radiotap
header intact, CRC-failed frames included — to `maburgs` over UDP and to the
web GS in **Spotter** mode over plain `ws://`. Clients steer the radio
(channel + HT40 secondary) over the same link. No key or decrypt logic lives
on the device; that stays in mabur's receivers.

- OpenWrt **25.12** (`ath79/generic`), CPE510 **v1/v2/v3**.
- Ships the in-tree `mabur-relay` daemon (single-threaded C, no library
  dependencies beyond libc) as a procd service that starts at boot and stays
  passive until a client subscribes, plus the three ath9k kmod patches (SNR
  antnoise, EVM lock-quality, TX-power uncap).
- Design + plan: `docs/superpowers/specs/` and `docs/superpowers/plans/`
  (gitignored, dev-machine only); the committed protocol contract is
  `docs/mabur-relay-protocol.md`.

## Build

Requires Docker. All inputs are pinned in `versions.env`.

```sh
./build.sh all       # package (+ qemu big-endian self-test) then images
# or individually:
./build.sh package   # compile the mabur-relay .apk (+ qemu big-endian self-test)
./build.sh image     # assemble the CPE510 images
```

The first run builds two Docker images (OpenWrt SDK ~3.5 GB, ImageBuilder
~1.7 GB) — it downloads the pinned SDK/ImageBuilder and clones the `base` +
`packages` feeds, then caches all of it. Subsequent builds reuse the images.
The relay source lives in-tree at `feed/net/mabur-relay/src/`; edit it and
rerun `./build.sh package`.

> The ImageBuilder stage fetches package indexes from `downloads.openwrt.org`
> at image-build time. On a host with a broken IPv6 route to that server, GNU
> `wget` stalls retrying over IPv6 before it gives up — the build scripts do
> not force IPv4, so the fix is the host's IPv6 route, not a build flag.

Outputs land in `output/`:
- `*tplink_cpe510-v{1,2,3}-squashfs-sysupgrade.bin` — flash from OpenWrt.
- `*tplink_cpe510-v{1,2,3}-squashfs-factory.bin` — flash from TP-Link firmware / TFTP.

### Notes on OpenWrt 25.12 (apk)

25.12 uses the `apk` package manager. The package is built as
`mabur-relay-<version>-r<rel>.apk`; the ImageBuilder picks it up from its
local `packages/` dir and `ADD_LOCAL_KEY=1` makes the image trust it. The
firewall stack (`firewall4` + `nftables` + `kmod-nft-*`) is removed —
this is a one-port appliance, so it adds no value and frees ~1.6 MiB of rootfs
(installed footprint ~9.6 MiB / 87 packages). The CPE510 sysupgrade image is a
**fixed-layout** ~7.8 MB (tplink-safeloader), so removing packages frees
read-only rootfs/overlay space rather than shrinking the `.bin`; if you add
packages and the rootfs overflows the partition, trimming further defaults is
the lever.

## Flash

Pick the file matching your hardware revision. From stock TP-Link: use the
`-factory.bin` (Pharos web UI or TFTP recovery). From an existing OpenWrt:
`sysupgrade -n <...-sysupgrade.bin>` (use `-n`, do not keep settings).

## Configure & run

1. Set your computer's NIC to a static **192.168.1.x/24** and connect it to
   the CPE510 (PoE LAN port). The device is **192.168.1.1**, no DHCP.
2. The `mabur-relay` service **starts at boot** and stays passive — radio up,
   nothing forwarded to anyone — until a client subscribes. There is nothing
   to enable and no key to configure.
3. `/etc/mabur-relay.conf` (shell-sourced; edit, then reboot or
   `/etc/init.d/mabur-relay restart` to apply):

   ```
   PHY=phy0            # radio phy
   MON=mon0            # monitor vif the relay listens on
   REG=US              # regulatory domain (your legal responsibility)
   TXPOWER=            # phy TX power in mBm (e.g. 2000 = 20 dBm); empty = driver default
   BOOT_CHANNEL=136    # channel until a client tunes (a respawn uses the last tuned one)
   BOOT_SEC=HT40-      # HT20 | HT40+ | HT40-
   UDP_PORT=8310       # maburgs: HELLO/TUNE in, FRAME/STATUS out
   WS_PORT=8311        # web GS (Spotter) over ws://
   ```

4. Ports: **UDP 8310** for `maburgs`, **`ws://` 8311** for the web GS Spotter.
   Whichever subscriber owns the tune (the longest-lived UDP client, else the
   oldest WebSocket client, else none) steers the radio with `TUNE` messages.
5. Web GS note: a page served from `http://127.0.0.1` (e.g. `web/serve.py`)
   may open `ws://192.168.1.1:8311`. The hosted HTTPS page may **not** — a
   browser blocks a plain `ws://` connection from an HTTPS page as mixed
   content — so phones need TLS, which this relay does not build.

See `docs/mabur-relay-protocol.md` for the wire protocol contract and
`docs/verify-mabur-relay-on-device.md` for the on-device acceptance runbook.

## Radio metrics: SNR and EVM

The image carries patched `mac80211`/`ath9k` kmods that expose two extra
per-frame metrics in the monitor-mode radiotap header, so maburgs / the web GS
see them per antenna next to RSSI:

- **SNR [dB]** — derived from the calibrated noise floor (radiotap `DBM_ANTNOISE`).
- **EVM [dB]** — `|EVM|` in dB, higher = better, from the ar9003 RX descriptor's
  per-pilot EVM (radiotap `LOCK_QUALITY`). EVM is per *spatial stream*, not per
  antenna, so the one per-frame value is shown on every antenna that received
  the frame.

### ⚠ ath9k EVM limitation (CPE510 / AR9344)

The AR9344's ar9003 hardware only measures EVM reliably for **short frames**.
For **long frames it returns "measurement-failed" markers** instead of real EVM,
so the driver drops those readings (requires ≥3 valid pilot bytes, and discards
any frame containing a failure marker). In practice this means **EVM is reported
only for the short control streams (mavlink / tunnel) and is blank (`--`) for the
video stream** — the high-bitrate stream you'd most want it for. Control streams
typically read ~15–25 dB; the gap below SNR is genuine non-thermal impairment
(multipath / interference). For the video link, rely on **SNR + FEC-recovery**
stats instead. (This is hardware-specific to ar9003; the Realtek 88x2 path
computes EVM differently and isn't subject to this limit.)

## On-device smoke test

```sh
ssh root@192.168.1.1
logread | grep mabur-relay   # e.g. "up: mon0 on 136 HT40-"
iw dev                       # mon0 present, type monitor
```

From the host wired to the CPE:

```sh
python3 relayprobe.py 192.168.1.1
```

(`relayprobe.py` is the throwaway client in
`docs/verify-mabur-relay-on-device.md`, which also has the full acceptance
runbook: frame-rate/loss gates, retune latency, and the browser Spotter
smoke test.)
