# Verify ath9k SNR on the CPE510

This firmware patches the in-kernel `mac80211`/ath9k driver to emit the real noise floor
(`ah->noise`) as the monitor-mode radiotap field `IEEE80211_RADIOTAP_DBM_ANTNOISE`. wfb-ng then
computes `SNR = signal − noise` per frame; without the patch it reports SNR `0`.

The build host cannot exercise the radio, so this on-device runbook is the acceptance gate.

## 1. Flash

Flash the image for your hardware revision (sysupgrade from a running OpenWrt, or TFTP recovery):

```
output/openwrt-25.12.4-ath79-generic-tplink_cpe510-v1-squashfs-sysupgrade.bin   # (or -v2 / -v3)
```

## 2. Confirm OUR patched driver loaded

Over SSH/serial on the device:

```sh
dmesg | grep -i ath9k          # no vermagic / module-load errors
iw phy                         # phy0 present
apk list --installed | grep -E 'kmod-(mac80211|ath9k)'   # expect 6.12.87.6.18.26-r2 (our build), not -r1
```

The `-r2` release is the marker that the patched bundle (not stock `-r1`) is installed.

## 3. Radiotap mechanism check — is DBM_ANTNOISE emitted?

Bring the radio up in monitor mode the way the wfb-ng launcher does (or manually create a `monN`
interface on the configured channel), then capture a few frames:

```sh
# quick look on-device
tcpdump -i mon0 -e -c 20 -y IEEE802_11_RADIO

# or capture and inspect off-device with tshark/Wireshark:
tcpdump -i mon0 -y IEEE802_11_RADIO -c 200 -w /tmp/cap.pcap
#   tshark -r cap.pcap -T fields -e radiotap.dbm_antnoise -e radiotap.dbm_antsignal
```

Pass: `radiotap.dbm_antnoise` is **present** and physically sane (≈ −90…−105 dBm), and
`radiotap.dbm_antsignal` tracks the link. If `dbm_antnoise` is absent, the patch is not active
(re-check step 2). Note: ath9k zeroes `rx_status` per frame and sets `noise` on every received
frame, so the field should appear on all data frames; it is intentionally suppressed only when
`noise == 0` (which should not occur on a live ath9k link).

## 4. Acceptance — live SNR

This firmware ships no on-device UI; SNR is read on the **operator host** from `wfb_rx`'s stats
stream (e.g. via `wfb-cli`, or by reading the `RX_ANT` lines directly). With a wifibroadcast TX
transmitting on the configured channel, run the device's `wfb_rx` as the launcher does and observe
the per-antenna stats line:

```
<ts>  RX_ANT  <freq:mcs:bw>  <antenna_id>  <count:rssi_min:rssi_avg:rssi_max:snr_min:snr_avg:snr_max>
```

**Pass = the SNR triplet `snr_min:snr_avg:snr_max` is non-zero and stable**, and
`snr_avg ≈ rssi_avg − noise` (with a sane noise floor). Before the patch this triplet was `0:0:0`.

## If SNR is still 0

1. Confirm the loaded module is our `-r2` (step 2) — a stock `-r1` kmod has no antnoise emit.
2. Capture a pcap (step 3) and check the radiotap `present` bitmap bit 6 (DBM_ANTNOISE) — if the
   field is absent, the wrong driver is loaded.
3. If `dbm_antnoise` is present but wfb-ng still shows 0, confirm wfb-ng's `rx.hpp` SNR path
   (`snr = (noise != SCHAR_MAX) ? rssi - noise : 0`) is seeing the field — i.e. the radiotap
   parser picks up `IEEE80211_RADIOTAP_DBM_ANTNOISE`.
