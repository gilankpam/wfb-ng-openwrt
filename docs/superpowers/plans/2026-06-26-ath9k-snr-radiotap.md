# ath9k SNR via radiotap DBM_ANTNOISE — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Patch the OpenWrt `mac80211`/ath9k kernel-module bundle so the CPE510 firmware emits a real noise floor (`ah->noise`) in the monitor-mode radiotap header (`IEEE80211_RADIOTAP_DBM_ANTNOISE`), making wfb-ng report non-zero SNR with no wfb-ng change.

**Architecture:** One source patch to OpenWrt's `mac80211` package (covers mac80211 core `rx.c` + `mac80211.h` and the in-bundle ath9k `common.c`). The patched kmod bundle is compiled in the existing OpenWrt **SDK** Docker stage and handed to the **ImageBuilder** stage, which installs our higher-`PKG_RELEASE` kmods in place of stock — mirroring how the repo already ships the `wfb-ng` package.

**Tech Stack:** OpenWrt 25.12.4 (ath79/generic, `mips_24kc`, **kernel linux-6.12.87**), **mac80211 backports 6.18.26-1**, OpenWrt SDK + ImageBuilder in Docker, `quilt` (OpenWrt's patch-authoring tool), ath9k/mac80211, `wfb-ng` (consumer, unchanged).

## Global Constraints

Copied verbatim from the spec; every task inherits these.

- OpenWrt **25.12.4** (code `r32933-4ccb782af7`), target **ath79/generic**, arch **mips_24kc** (big-endian MIPS) — pinned in `versions.env`; package format is **`.apk`** (not `.ipk`).
- Build profiles: **`tplink_cpe510-v1 tplink_cpe510-v2 tplink_cpe510-v3`** (build all three).
- **Image budget: every `*sysupgrade.bin` ≤ `7680 * 1024` = 7864320 bytes** — the existing assertion in `docker/ib-build.sh` must still pass.
- The fix is **driver-side only** — **no change to wfb-ng source**.
- Noise value is the **real `ah->noise`** (live calibrated NF), not a constant.
- Radiotap emit is **gated on `status->noise != 0`** (sentinel; safe because the image is ath9k-only); the writer and the header-length calc must use the **identical** condition.
- The vendored package is the **`mac80211` backports bundle**; **bump its `PKG_RELEASE` 1 → 2 once** so all its kmods (`kmod-mac80211`, `kmod-cfg80211`, `kmod-ath`, `kmod-ath9k`, `kmod-ath9k-common`, …) outrank stock and are shipped together (struct-layout/ABI consistency).
- All Docker build runs use **`--network host`** (host resolver) and **`-e HOME=/tmp -v "$PWD:/work"`**, matching the repo's existing `build.sh`.
- Reference spec: `docs/superpowers/specs/2026-06-26-ath9k-snr-radiotap-design.md`.
- The wfb-ng sibling checkout is at `../wfb-ng` (consumer reference only).

## Spike findings (resolved before execution — do not re-investigate)

- SDK image `wfbng-sdk:25.12.4` and IB image `wfbng-ib:25.12.4` are already built locally.
- The SDK does **not** ship `package/kernel/mac80211`, but the **exact-matching source is in the SDK at `feeds/base/kernel/mac80211`** (backports `PKG_VERSION:=6.18.26`, `PKG_RELEASE:=1`). Vendor from there — no external clone.
- The SDK has the prepared kernel tree `build_dir/target-mips_24kc_musl/linux-ath79_generic/linux-6.12.87/` with **`Module.symvers` + `.config`** present → `make package/kernel/mac80211/compile` builds modules in the SDK. **SDK-extend confirmed; no buildroot fallback.**
- mac80211 module builds can take several minutes — **run them with Bash `run_in_background: true` and poll**, since a foreground call may exceed the 10-min tool cap.

---

## Task 1: Vendor the mac80211 source recipe and confirm the unpatched bundle builds

Establish (and wire into `docker/sdk-build.sh`) the recipe that copies the SDK's own
`mac80211` source into the package tree and compiles it, emitting `kmod-*.apk`s — unpatched.
This proves the build path before any patching and is the foundation Task 2 patches.

**Files:**
- Modify: `docker/sdk-build.sh` (append a mac80211 build block; guarded so it is reusable)

**Interfaces:**
- Produces: `build/packages/kmod-mac80211_*.apk`, `kmod-cfg80211_*.apk`, `kmod-ath_*.apk`, `kmod-ath9k_*.apk`, `kmod-ath9k-common_*.apk` (stock `…-r1` at this task; bumped in Task 2). A reusable build block later tasks extend with the patch + release bump.

- [ ] **Step 1: Confirm the source location in the SDK image (sanity, fast)**

```bash
cd /home/gilankpam/Projects/poc/wfb-ng-openwrt
docker run --rm -e HOME=/tmp wfbng-sdk:25.12.4 sh -c \
  'ls -d /opt/sdk/feeds/base/kernel/mac80211 && grep -E "^PKG_(VERSION|RELEASE):" /opt/sdk/feeds/base/kernel/mac80211/Makefile'
```
Expected: prints the dir and `PKG_VERSION:=6.18.26`, `PKG_RELEASE:=1`.

- [ ] **Step 2: Add the mac80211 build block to `docker/sdk-build.sh`**

Append, after the existing `wfb-ng` build + copy block (keep the existing arch-check block last):
```sh
# --- Patched mac80211/ath9k bundle: radiotap DBM_ANTNOISE so wfb-ng reports SNR. ---
# Source comes from the SDK's own base feed (exact 25.12.4 rev), copied into the package
# tree so we can apply our patch and bump PKG_RELEASE. PATCH is applied only if present
# (Task 1 builds stock; Task 2 adds the patch + the release bump).
MAC_SRC=feeds/base/kernel/mac80211
MAC_PKG=package/kernel/mac80211
if [ ! -d "$MAC_PKG" ]; then cp -a "$MAC_SRC" "$MAC_PKG"; fi
if [ -f /work/patches/mac80211/999-ath9k-radiotap-antnoise.patch ]; then
  cp /work/patches/mac80211/999-ath9k-radiotap-antnoise.patch "$MAC_PKG/patches/subsys/"
  sed -i 's/^PKG_RELEASE:=.*/PKG_RELEASE:=2/' "$MAC_PKG/Makefile"
fi
echo 'CONFIG_PACKAGE_kmod-ath9k=y' >> .config
make defconfig
make package/kernel/mac80211/compile -j"$(nproc)" V=s
for k in kmod-cfg80211 kmod-mac80211 kmod-ath kmod-ath9k kmod-ath9k-common; do
  f=$(find bin -name "${k}_*.apk" | head -n1)
  [ -n "$f" ] || { echo "ERROR: $k apk not produced"; exit 1; }
  cp -v "$f" /work/build/packages/
done
```

- [ ] **Step 3: Run the package stage and confirm kmod apks appear (background build)**

The compile is long; launch in the background and poll.
```bash
mkdir -p build/packages
./build.sh package    # run_in_background: true
```
Then poll until it returns, and check:
```bash
ls -1 build/packages/kmod-*.apk
```
Expected: the five `kmod-*_*.apk` files exist (stock release, filenames contain `-r1`).
If the compile fails, read the `V=s` tail for the real error before changing anything.

- [ ] **Step 4: Commit**

```bash
git add docker/sdk-build.sh
git commit -m "build(sdk): compile mac80211/ath9k kmod bundle from base feed"
```
Expected: clean commit; `git show --stat` lists only `docker/sdk-build.sh`.

---

## Task 2: Author the DBM_ANTNOISE patch and build the patched bundle

Author the four-hunk patch with `quilt` against the real prepared source, store it in the repo,
let `sdk-build.sh` apply it and bump `PKG_RELEASE`, and rebuild to confirm it applies + compiles.

**Files:**
- Create: `patches/mac80211/999-ath9k-radiotap-antnoise.patch`
- (Task 1 already made `docker/sdk-build.sh` apply it + bump release when present.)

**Interfaces:**
- Consumes: the build block from Task 1.
- Produces: the five `kmod-*.apk`s at **`6.18.26-r2`** with the radiotap noise emit compiled in.

- [ ] **Step 1: Open an interactive SDK shell and prepare the source under quilt**

```bash
cd /home/gilankpam/Projects/poc/wfb-ng-openwrt
docker run --rm -it --network host -e HOME=/tmp -v "$PWD:/work" wfbng-sdk:25.12.4 sh
# --- inside ---
cd /opt/sdk
[ -d package/kernel/mac80211 ] || cp -a feeds/base/kernel/mac80211 package/kernel/mac80211
make defconfig
make package/kernel/mac80211/{clean,prepare} QUILT=1 V=s
PKGDIR=$(ls -d build_dir/target-*/linux-*/backports-* | head -n1); echo "$PKGDIR"; cd "$PKGDIR"
```
Expected: `$PKGDIR` is the prepared, fully-patched backports tree (existing patches already applied).

- [ ] **Step 2: Verify stock source lacks a noise emit (failing baseline) and capture anchors**

```bash
grep -n "IEEE80211_RADIOTAP_DBM_ANTSIGNAL" net/mac80211/rx.c
grep -c "IEEE80211_RADIOTAP_DBM_ANTNOISE" net/mac80211/rx.c     # expect 0 (comment only)
grep -n "rxs->signal = ah->noise" drivers/net/wireless/ath/ath9k/common.c
grep -n "__le8 signal\|s8 signal;" include/net/mac80211.h
```
Expected: ANTSIGNAL block present; ANTNOISE emit count 0; the ath9k signal line and the
`s8 signal;` struct field found.

- [ ] **Step 3: Create the quilt patch and add the three files**

```bash
quilt new subsys/999-ath9k-radiotap-antnoise.patch
quilt add include/net/mac80211.h drivers/net/wireless/ath/ath9k/common.c net/mac80211/rx.c
```

- [ ] **Step 4: Edit `include/net/mac80211.h`** — after the `s8 signal;` line in
`struct ieee80211_rx_status`, add:
```c
	s8 noise;	/* NF in dBm; 0 = not present (ath9k radiotap antnoise) */
```

- [ ] **Step 5: Edit `drivers/net/wireless/ath/ath9k/common.c`** — in
`ath9k_cmn_process_rssi()`, immediately after `rxs->signal = ah->noise + rx_stats->rs_rssi;`, add:
```c
	rxs->noise = ah->noise;
```

- [ ] **Step 6: Edit `net/mac80211/rx.c`** — find the `DBM_ANTSIGNAL` writer block in
`ieee80211_add_rx_radiotap_header()`:
```c
	/* IEEE80211_RADIOTAP_DBM_ANTSIGNAL */
	if (ieee80211_hw_check(&local->hw, SIGNAL_DBM) &&
	    !(status->flag & RX_FLAG_NO_SIGNAL_VAL)) {
		*pos = status->signal;
		rthdr->it_present |=
			cpu_to_le32(BIT(IEEE80211_RADIOTAP_DBM_ANTSIGNAL));
		pos++;
	}
```
Immediately **after** it (radiotap bit order: ANTSIGNAL=5, ANTNOISE=6), add:
```c
	/* IEEE80211_RADIOTAP_DBM_ANTNOISE */
	if (status->noise) {
		*pos = status->noise;
		rthdr->it_present |=
			cpu_to_le32(BIT(IEEE80211_RADIOTAP_DBM_ANTNOISE));
		pos++;
	}
```
Then find where header **length** reserves the `DBM_ANTSIGNAL` byte (search the same file —
likely `ieee80211_rx_radiotap_hdrlen()`, the `len += 1` under the SIGNAL_DBM guard) and add the
matching reservation under the **identical** condition:
```c
	if (status->noise)
		len += 1;
```
**Lock-step invariant:** the writer's `if (status->noise)` and the length's `if (status->noise)`
must be byte-for-byte identical, or every monitor frame's radiotap header corrupts.

- [ ] **Step 7: Refresh, copy the patch into the repo, exit the container**

```bash
quilt refresh
grep -c "DBM_ANTNOISE" patches/subsys/999-ath9k-radiotap-antnoise.patch   # expect >= 1
mkdir -p /work/patches/mac80211
cp patches/subsys/999-ath9k-radiotap-antnoise.patch /work/patches/mac80211/
git -C /work diff --stat --no-index /dev/null /work/patches/mac80211/999-ath9k-radiotap-antnoise.patch || true
exit
```
Expected: `patches/mac80211/999-ath9k-radiotap-antnoise.patch` now exists in the repo and
references all three files plus `DBM_ANTNOISE`.

- [ ] **Step 8: Clean rebuild — confirm it applies + compiles + release is bumped (passing test)**

`sdk-build.sh` (from Task 1) auto-applies the patch and bumps the release when the patch file is
present. Rebuild from clean:
```bash
docker run --rm --network host -e HOME=/tmp -v "$PWD:/work" wfbng-sdk:25.12.4 sh -eu -c '
  cd /opt/sdk; rm -rf package/kernel/mac80211
  cp -a feeds/base/kernel/mac80211 package/kernel/mac80211
  cp /work/patches/mac80211/999-ath9k-radiotap-antnoise.patch package/kernel/mac80211/patches/subsys/
  sed -i "s/^PKG_RELEASE:=.*/PKG_RELEASE:=2/" package/kernel/mac80211/Makefile
  echo CONFIG_PACKAGE_kmod-ath9k=y >> .config; make defconfig
  make package/kernel/mac80211/{clean,prepare} V=s 2>&1 | grep -iE "Applying.*999-ath9k|Patch failed" || true
  make package/kernel/mac80211/compile -j"$(nproc)" V=s 2>&1 | tail -20
  find bin -name "kmod-mac80211_*.apk" -o -name "kmod-ath9k_*.apk"'   # run_in_background: true
```
Poll to completion. Expected: log shows `Applying ... 999-ath9k-radiotap-antnoise.patch` (no
"Patch failed"); compile succeeds; the apk filenames carry **`-r2`**.
If the patch fails to apply, the issue is series ordering — re-author in Step 6 against the
prepared tree (quilt context is authoritative) or rename so it sorts after conflicting patches.

- [ ] **Step 9: Commit**

```bash
git add patches/mac80211/999-ath9k-radiotap-antnoise.patch
git commit -m "feat: ath9k radiotap DBM_ANTNOISE patch (real ah->noise -> wfb-ng SNR)"
```

---

## Task 3: Make ImageBuilder install our kmods and build the images

Have the ImageBuilder pick our `-r2` kmods over stock, build all three CPE510 images, and assert
both the override and the size budget.

**Files:**
- Modify: `docker/ib-build.sh`

**Interfaces:**
- Consumes: the `kmod-*.apk`s (`-r2`) in `build/packages/` from Task 2.
- Produces: `output/*cpe510*sysupgrade.bin` / `*factory.bin` for v1/v2/v3, built against our
  `kmod-mac80211`/`kmod-ath9k`, each ≤ 7864320 bytes.

- [ ] **Step 1: Copy our kmod apks into the ImageBuilder's local repo**

In `docker/ib-build.sh`, next to the existing `cp /work/build/packages/wfb-ng-*.apk packages/`,
add (before the per-profile `make image` loop):
```sh
# Our patched mac80211/ath9k kmods (PKG_RELEASE=2) override the stock -r1 ones.
cp /work/build/packages/kmod-*.apk packages/
```

- [ ] **Step 2: Assert the IB resolves OUR kmod version (test)**

Inside the `for p in $PROFILES` loop, after `make image ...`, add:
```sh
  man=$(find bin -name "*${p}*.manifest" | head -n1)
  if [ -n "$man" ]; then
    grep -E '^kmod-mac80211 ' "$man" || true
    grep -Eq '^kmod-mac80211 .*-r2' "$man" || { echo "ERROR: stock kmod-mac80211 used for $p"; exit 1; }
  fi
```
Note: confirm the exact manifest version token against a real build (apk renders
`PKG_VERSION-rPKG_RELEASE`, i.e. `6.18.26-r2`); adjust the grep if the format differs.

- [ ] **Step 3: Run the full build (background)**

```bash
cd /home/gilankpam/Projects/poc/wfb-ng-openwrt
./build.sh package    # run_in_background: true — SDK: wfb-ng + patched -r2 kmods
# poll to completion, then:
./build.sh image      # run_in_background: true — ImageBuilder: assemble images
```
Expected: `package` puts `wfb-ng-*.apk` and `kmod-*_*-r2_*.apk` in `build/packages/`; `image`
prints the size lines, `OK: all images within size budget`, and the manifest assertion passes
for all three profiles.

- [ ] **Step 4: Confirm outputs and budget**

```bash
ls -lh output/
for f in output/*sysupgrade.bin; do echo "$f: $(wc -c < "$f") bytes (max 7864320)"; done
```
Expected: three `sysupgrade.bin` (+ factory) images, each ≤ 7864320 bytes.

- [ ] **Step 5: Commit**

```bash
git add docker/ib-build.sh
git commit -m "build(ib): install patched mac80211 kmods + assert override"
```

---

## Task 4: On-device verification (operator-run)

Functional proof requires hardware, which can't be done from the build host. This task delivers
the runbook; the operator runs it as the acceptance gate.

**Files:**
- Create: `docs/verify-snr-on-device.md`

**Interfaces:**
- Consumes: an `output/*cpe510*sysupgrade.bin` from Task 3 + a CPE510 + a wifibroadcast TX source.
- Produces: a recorded PASS/FAIL of live SNR.

- [ ] **Step 1: Write `docs/verify-snr-on-device.md`**

```markdown
# Verify ath9k SNR on the CPE510

## 1. Flash
Flash `output/<rev>/...-cpe510-vX-...-sysupgrade.bin` (sysupgrade or TFTP recovery).

## 2. Driver + module sanity
SSH/serial to the device:
- `dmesg | grep -i ath9k` — no vermagic/load errors.
- `iw phy` / `iw dev` — phy present.
- Optional, conclusive: compare `modinfo mac80211 | grep srcversion` against the built
  module to confirm OUR `-r2` module loaded.

## 3. Radiotap mechanism check (proves the field is emitted)
With the monitor interface up (the wfb-ng launcher creates it):
- `tcpdump -i <mon> -e -c 20 -y IEEE802_11_RADIO` — frames show a noise figure, or
- capture to pcap + open in tshark/Wireshark: `radiotap.dbm_antnoise` is **present** and sane
  (≈ -90…-105 dBm); `radiotap.dbm_antsignal` tracks the link.

## 4. Acceptance — live SNR
With a wifibroadcast TX transmitting on the configured channel, run the device's `wfb_rx`
as the firmware launcher does, and read its stats on the operator host (wfb-cli or the raw
stats stream). Confirm the `RX_ANT` line's SNR triplet `snr_min:snr_avg:snr_max` is **non-zero
and stable**, and `snr_avg ≈ rssi_avg − noise`.

PASS = non-zero, physically sane SNR on a live link.
```

- [ ] **Step 2: Commit**

```bash
git add docs/verify-snr-on-device.md
git commit -m "docs: on-device SNR verification runbook"
```

- [ ] **Step 3: Operator runs the runbook and records the result**

If SNR stays `0`: re-check Task 2 Step 6 (writer vs length lock-step — a mismatch breaks the
header); confirm the loaded module is ours (§2); capture a pcap and inspect `radiotap.present`
bit 6 to see whether the field is emitted at all.

---

## Self-Review

**Spec coverage:**
- §3 hunk (1) struct field → Task 2 Step 4. ✓
- §3 hunk (2) ath9k populate → Task 2 Step 5. ✓
- §3 hunks (3)(4) writer + length lock-step → Task 2 Step 6. ✓
- §3 sentinel gating `status->noise != 0` → Task 2 Step 6 + Global Constraints. ✓
- §4 vendor (from SDK base feed) + PKG_RELEASE bump + SDK build + IB override → Tasks 1–3. ✓
- §5 ABI whole-bundle ship → Task 1/2 copy all five kmods; Task 3 installs them. ✓
- §5 R1 (SDK builds mac80211) → resolved in Spike findings (Module.symvers present); no fallback needed. ✓
- §5 R2 override-takes-effect → Task 3 Step 2 manifest assertion. ✓
- §5 R3 size budget → Task 3 Step 4. ✓
- §6 verification ladder (build→image→device→mechanism→acceptance) → Tasks 1–4. ✓
- §1/§6 acceptance = live `RX_ANT` SNR host-side → Task 4. ✓

**Placeholder scan:** No "TBD"/"handle edge cases"; every code edit shows exact C. Real source
line numbers are resolved by `quilt` against live source by design (Task 2 Steps 2–7), not guessed.

**Type consistency:** Carrier field `noise` (`s8`) — defined in `mac80211.h` (T2 S4), set
`rxs->noise = ah->noise` in `common.c` (T2 S5), read `status->noise` in `rx.c` (T2 S6), identical
gate in writer and length (T2 S6). Release token `-r2` is the same token asserted in the IB
manifest grep (T3 S2). Consistent.
