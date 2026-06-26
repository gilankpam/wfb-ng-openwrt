# Verify ath9k TX-power uncap on the CPE510

Acceptance gate for the TX-power uncap (patch `998-ath9k-txpower-uncap` + the wfb-ng launcher
fix). Run on the bench unit (`root@192.168.1.1`). The build host cannot prove RF behaviour, so
an operator runs this against real hardware.

## 1. Flash & module sanity

Flash `output/<rev>/...-cpe510-v3-...-sysupgrade.bin` (sysupgrade or TFTP recovery). Then:

- `dmesg | grep -i ath9k` — no vermagic / module-load errors.
- Confirm OUR build loaded: `kmod-ath9k` / `kmod-mac80211` are at **`-r4`**
  (`apk list --installed 2>/dev/null | grep -E 'kmod-(ath9k|mac80211)'`, or check the image
  manifest). A stock `-r1` module mixed in re-clamps power.

## 2. Higher power is reachable (primary proof)

Bring up a monitor vif on a 5.8 GHz channel (the wfb-ng launcher does this on start), then:

```
iw phy phy0 set txpower fixed 3000      # request 30 dBm (note: phy, not "dev mon0" — that is -122)
iw dev mon0 info | grep txpower         # EXPECT: txpower 30.00 dBm  (was clamped to 25.00)
iw phy phy0 set txpower fixed 4000      # request 40 dBm
iw dev mon0 info | grep txpower         # EXPECT: txpower 31.50 dBm  (silicon cap, saturates)
```

Register-level proof — the hardware was actually programmed, not just the software readback:

```
# AR_PHY_POWER_TX_RATE* hold per-rate power in half-dB. After 'fixed 3000', the
# active rate fields should read ~0x3c (=60 = 30 dBm), up from ~0x32 (=50 = 25 dBm).
cat /sys/kernel/debug/ieee80211/phy0/ath9k/regdump | grep -iE 'POWER_TX_RATE'
# (or read individually via regidx/regval)
```

> `iw phy phy0 info` still shows the channel max as the calibrated ~25 dBm — that is expected and
> correct. The per-interface `iw dev mon0 info` reflects the actual set value; the advertised
> channel max stays calibrated by design (so the default is safe — see §3).

## 3. Safe default (regression — must still hold)

```
iw phy phy0 set txpower auto
iw dev mon0 info | grep txpower         # EXPECT: ~23-25 dBm (calibrated), NOT 30/31.5
```

Then, with `TXPOWER=` empty in `/etc/wfb-ng.conf`, restart wfb-ng and re-check: power must come
up at the calibrated ~25, never hot. `iw phy phy0 info` channel max must still read the
calibrated value even after the §2 high set (proves the advertised default was not raised).

## 4. wfb-ng knob end-to-end

Set `TXPOWER=2700` in `/etc/wfb-ng.conf`, restart wfb-ng, then:

```
iw dev mon0 info | grep txpower         # EXPECT: 27.00 dBm
```

This proves the launcher fix (sets power via `iw phy`, since `iw dev mon0 set txpower` is -122 on
this AR9344) drives the knob end-to-end.

## 5. RF / link validation (gold standard, if gear available)

A/B against a second wfb-ng node at fixed geometry; log RSSI / SNR (the shipped antnoise→SNR
patch enables SNR) and FEC error rate across 23 → 25 → 27 → 30 dBm. Expect RSSI to climb
~linearly to ~27, then flatten (external PA compression), and FEC errors to rise past ~28 —
empirically locating the usable ceiling. Watch the AR9344 die temperature under sustained high
power. If an SDR / RF power meter is available, measure conducted power at the SMA directly.

## PASS criteria

- §2 shows **30 dBm** (and 31.5 at the cap) at `iw dev mon0 info` AND the register dump, **and**
- §3 still shows the **calibrated ~25 dBm default** (auto / empty `TXPOWER`).

## If it fails

- §2 still 25: confirm the loaded `kmod-ath9k`/`kmod-mac80211` are `-r4` (a stock module mixed in
  re-clamps); confirm the monitor vif is up and you used `iw phy`, not `iw dev mon0`.
- §3 default drifted above calibrated: the ar9003 gate fired at default — the calibrated max and
  the requested limit differ by a half-dB rounding step. Widen the gate to
  `regulatory->power_limit > regulatory->max_power_level + 1`, rebuild, re-verify.
