# ath9k SNR via radiotap DBM_ANTNOISE — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Patch the OpenWrt `mac80211`/ath9k kernel-module bundle so the CPE510 firmware emits a real noise floor (`ah->noise`) in the monitor-mode radiotap header (`IEEE80211_RADIOTAP_DBM_ANTNOISE`), making wfb-ng report non-zero SNR with no wfb-ng change.

**Architecture:** One source patch to OpenWrt's `mac80211` package (covers mac80211 core `rx.c` + `mac80211.h` and the in-bundle ath9k `common.c`). The patched kmod bundle is compiled in the existing OpenWrt **SDK** Docker stage and handed to the **ImageBuilder** stage, which installs our higher-`PKG_RELEASE` kmods in place of stock — mirroring how the repo already ships the `wfb-ng` package.

**Tech Stack:** OpenWrt 25.12.4 (ath79/generic, `mips_24kc`), OpenWrt SDK + ImageBuilder in Docker, `quilt` (OpenWrt's patch-authoring tool), ath9k/mac80211 (Linux backports), `wfb-ng` (consumer, unchanged).

## Global Constraints

Copied verbatim from the spec; every task inherits these.

- OpenWrt **25.12.4**, target **ath79/generic**, arch **mips_24kc** (big-endian MIPS) — pinned in `versions.env`.
- Build profiles: **`tplink_cpe510-v1 tplink_cpe510-v2 tplink_cpe510-v3`** (build all three).
- **Image budget: every `*sysupgrade.bin` ≤ `7680 * 1024` bytes** — the existing assertion in `docker/ib-build.sh` must still pass.
- The fix is **driver-side only** — **no change to wfb-ng source**.
- Noise value is the **real `ah->noise`** (live calibrated NF), not a constant.
- Radiotap emit is **gated on `status->noise != 0`** (sentinel; safe because the image is ath9k-only).
- **One** patch file in the `mac80211` package; **bump that package's `PKG_RELEASE` once** so all its kmods outrank stock.
- SDK and ImageBuilder are the **same** OpenWrt release, so kernel vermagic matches.
- Reference spec: `docs/superpowers/specs/2026-06-26-ath9k-snr-radiotap-design.md`.
- The wfb-ng sibling checkout is at `../wfb-ng` (consumer reference only).

---

## Task 1: Spike — make the SDK build the `mac80211` kmod bundle (R1 decision gate)

De-risk before any patching: prove the existing SDK container can compile the `mac80211`
bundle and emit `kmod-mac80211` / `kmod-ath9k` `.apk`s. This decides whether we proceed with
the SDK-extend path or fall back to a full buildroot stage.

**Files:**
- Inspect: `docker/Dockerfile.sdk`, `docker/sdk-build.sh`, `versions.env`
- Possibly create: `vendor/openwrt/mac80211/` (only if the SDK does not already ship the package source)

**Interfaces:**
- Produces: a confirmed build recipe that yields `bin/targets/.../packages/kmod-mac80211_*.apk` and `kmod-ath9k_*.apk` from the SDK container, plus a recorded decision (`SDK-builds-mac80211: yes|no`) that gates Tasks 2–3.

- [ ] **Step 1: Build the SDK image (if not cached)**

```bash
cd /home/gilankpam/Projects/poc/wfb-ng-openwrt
. ./versions.env
docker build -t "wfbng-sdk:${OPENWRT_VERSION}" \
  --build-arg OPENWRT_VERSION="$OPENWRT_VERSION" \
  --build-arg OPENWRT_TARGET="$OPENWRT_TARGET" \
  --build-arg OPENWRT_SUBTARGET="$OPENWRT_SUBTARGET" \
  -f docker/Dockerfile.sdk docker
```
Expected: image `wfbng-sdk:25.12.4` builds successfully.

- [ ] **Step 2: Check whether the SDK already ships the `mac80211` package source (the failing/branching test)**

```bash
docker run --rm --network host -e HOME=/tmp -v "$PWD:/work" \
  "wfbng-sdk:${OPENWRT_VERSION}" sh -c '
    echo "--- package/kernel/mac80211 ---"; ls -d /opt/sdk/package/kernel/mac80211 2>/dev/null && cat /opt/sdk/package/kernel/mac80211/Makefile | grep -E "^PKG_(NAME|VERSION|RELEASE|SOURCE)" ;
    echo "--- kernel build dir ---"; ls -d /opt/sdk/build_dir/target-*/linux-*/ 2>/dev/null | head ;
    echo "--- target/linux ---"; ls -d /opt/sdk/target/linux/ath79 2>/dev/null'
```
Decision:
- **If `package/kernel/mac80211` is present** → record `SDK-ships-mac80211: yes`; skip Step 3 (patch the SDK's own copy in later tasks).
- **If absent** → do Step 3 to vendor it.

- [ ] **Step 3 (only if absent): Vendor the matching `mac80211` package source into the repo**

```bash
# Pin the OpenWrt source ref that matches the SDK release.
echo 'OPENWRT_SRC_REF=v25.12.4' >> versions.env   # adjust if the exact tag differs (see note)

mkdir -p vendor/openwrt
git clone --depth 1 -b v25.12.4 https://github.com/openwrt/openwrt /tmp/owrt-src
cp -a /tmp/owrt-src/package/kernel/mac80211 vendor/openwrt/mac80211
git add versions.env vendor/openwrt/mac80211
```
Note: if tag `v25.12.4` does not resolve, find the ref from the SDK itself —
`docker run --rm wfbng-sdk:${OPENWRT_VERSION} sh -c 'cat /opt/sdk/version.buildinfo 2>/dev/null; cat /opt/sdk/.vermagic 2>/dev/null'` — and clone that revision. The only thing we need is the small `package/kernel/mac80211` directory (Makefile + `patches/` + `files/`); the backports tarball itself is fetched by the package Makefile at build time.

- [ ] **Step 4: Build the bundle unpatched in the SDK (the passing test)**

Run a throwaway build to prove the toolchain produces the kmods. If `SDK-ships-mac80211: yes`, build the SDK's copy directly; otherwise first copy the vendored dir into the SDK tree.

```bash
docker run --rm --network host -e HOME=/tmp -v "$PWD:/work" \
  "wfbng-sdk:${OPENWRT_VERSION}" sh -eu -c '
    cd /opt/sdk
    if [ ! -d package/kernel/mac80211 ]; then
      mkdir -p package/kernel && cp -a /work/vendor/openwrt/mac80211 package/kernel/mac80211
    fi
    make defconfig
    echo "CONFIG_PACKAGE_kmod-ath9k=y" >> .config
    echo "CONFIG_PACKAGE_kmod-mac80211=y" >> .config
    make defconfig
    make package/kernel/mac80211/compile -j"$(nproc)" V=s 2>&1 | tail -40
    echo "=== produced apks ==="
    find bin -name "kmod-mac80211_*.apk" -o -name "kmod-ath9k_*.apk"'
```
Expected: the `find` prints `kmod-mac80211_*.apk` and `kmod-ath9k_*.apk` paths.

- [ ] **Step 5: Record the decision and commit**

If Step 4 succeeded, the SDK-extend path is viable.

```bash
# Record the outcome in the plan/spec area for the next tasks.
printf '%s\n' \
  '# R1 outcome (Task 1)' \
  'SDK-ships-mac80211: <yes|no>' \
  'SDK-builds-mac80211: yes' \
  'mac80211 source: <sdk-builtin | vendor/openwrt/mac80211 @ v25.12.4>' \
  > docs/superpowers/plans/R1-outcome.md
git add docs/superpowers/plans/R1-outcome.md
[ -d vendor/openwrt/mac80211 ] && git add vendor/openwrt/mac80211 versions.env || true
git commit -m "build: vendor+verify mac80211 kmod build in SDK (R1 spike)"
```

- [ ] **Step 6 (fallback, only if Step 4 fails): switch this task's deliverable to a buildroot stage**

If `make package/kernel/mac80211/compile` cannot run in the SDK (e.g. no prepared kernel),
do NOT proceed with SDK-extend. Instead create `docker/Dockerfile.buildroot` that clones
OpenWrt at the pinned ref, `make defconfig` for `ath79/generic`, and builds
`package/kernel/mac80211/compile` from the full tree; emit the same kmod apks to
`build/packages/`. Re-run Steps 4–5 against that container. All later tasks consume the apks
identically regardless of which stage produced them. Commit with message
`build: add buildroot fallback stage for mac80211 kmods`.

---

## Task 2: Author and build the `DBM_ANTNOISE` patch

Write the four-hunk patch with `quilt` against the *real* prepared source (so line numbers are
correct), install it into the package's `patches/`, bump `PKG_RELEASE`, and rebuild from clean
to confirm it applies and compiles.

**Files:**
- Create: `<mac80211-pkg>/patches/ath9k/999-ath9k-radiotap-antnoise.patch`
- Modify: `<mac80211-pkg>/Makefile` (`PKG_RELEASE`)
- Where `<mac80211-pkg>` is `vendor/openwrt/mac80211` (if vendored) or the SDK's `package/kernel/mac80211` copied into the repo for patching. Patches edit (via quilt) the prepared backports tree: `net/mac80211/rx.c`, `include/net/mac80211.h`, `drivers/net/wireless/ath/ath9k/common.c`.

**Interfaces:**
- Consumes: the buildable `mac80211` package recipe from Task 1.
- Produces: `kmod-mac80211_*.apk`, `kmod-ath9k_*.apk`, `kmod-ath9k-common_*.apk`, `kmod-ath_*.apk`, `kmod-cfg80211_*.apk` in `bin/`, all carrying the **bumped** `PKG_RELEASE`, with the radiotap noise emit compiled in.

- [ ] **Step 1: Open an interactive SDK shell and prepare the source under quilt**

```bash
cd /home/gilankpam/Projects/poc/wfb-ng-openwrt
. ./versions.env
docker run --rm -it --network host -e HOME=/tmp -v "$PWD:/work" \
  "wfbng-sdk:${OPENWRT_VERSION}" sh
# --- inside the container ---
cd /opt/sdk
[ -d package/kernel/mac80211 ] || { mkdir -p package/kernel && cp -a /work/vendor/openwrt/mac80211 package/kernel/mac80211; }
make defconfig
make package/kernel/mac80211/{clean,prepare} QUILT=1 V=s
PKGDIR=$(ls -d build_dir/target-*/linux-*/backports-* 2>/dev/null | head -n1)
echo "prepared tree: $PKGDIR"
cd "$PKGDIR"
quilt new ath9k/999-ath9k-radiotap-antnoise.patch
```
Expected: `quilt new` reports the new patch is at top of series.

- [ ] **Step 2: Verify the stock source lacks a noise emit (the failing baseline)**

```bash
# still inside $PKGDIR
grep -n "IEEE80211_RADIOTAP_DBM_ANTSIGNAL" net/mac80211/rx.c
grep -nc "IEEE80211_RADIOTAP_DBM_ANTNOISE" net/mac80211/rx.c   # expect 0 emits (comment only)
grep -n "rxs->signal = ah->noise" drivers/net/wireless/ath/ath9k/common.c
grep -n "s8 signal;" include/net/mac80211.h
```
Expected: ANTSIGNAL block exists; ANTNOISE emit count is 0; the ath9k signal line and the
`signal` struct field are found. These greps are your anchors for the edits.

- [ ] **Step 3: Add the carrier field to `struct ieee80211_rx_status`**

```bash
quilt add include/net/mac80211.h
```
In `include/net/mac80211.h`, immediately after the `s8 signal;` line inside
`struct ieee80211_rx_status`, add:
```c
	s8 noise;	/* NF in dBm; 0 = not present (ath9k radiotap antnoise) */
```

- [ ] **Step 4: Populate the field in ath9k**

```bash
quilt add drivers/net/wireless/ath/ath9k/common.c
```
In `ath9k_cmn_process_rssi()`, immediately after the existing
`rxs->signal = ah->noise + rx_stats->rs_rssi;` line, add:
```c
	rxs->noise = ah->noise;
```

- [ ] **Step 5: Emit the field in the radiotap writer and length calc**

```bash
quilt add net/mac80211/rx.c
```
mac80211 builds the RX radiotap header in two coordinated spots; both live in/around
`ieee80211_add_rx_radiotap_header()`. Find the `DBM_ANTSIGNAL` writer block (from Step 2's
grep), which looks like:
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
Immediately **after** that block (radiotap bit order: ANTSIGNAL=5, ANTNOISE=6), add:
```c
	/* IEEE80211_RADIOTAP_DBM_ANTNOISE */
	if (status->noise) {
		*pos = status->noise;
		rthdr->it_present |=
			cpu_to_le32(BIT(IEEE80211_RADIOTAP_DBM_ANTNOISE));
		pos++;
	}
```
Then find where the header **length** reserves a byte for `DBM_ANTSIGNAL` (search the same
file/function for the length pass — e.g. a `len += 1;` guarded by the SIGNAL_DBM check, or in
`ieee80211_rx_radiotap_hdrlen()`), and add the matching reservation guarded by the **identical**
`status->noise` condition:
```c
	if (status->noise)
		len += 1;
```
Lock-step invariant: the writer's `if (status->noise)` and the length's `if (status->noise)`
must be byte-for-byte the same condition, or every monitor frame's header corrupts.

- [ ] **Step 6: Refresh the patch and copy it back into the repo**

```bash
# inside $PKGDIR
quilt refresh
# sanity: the diff touches exactly three files and adds the noise emit
grep -c "DBM_ANTNOISE" patches/ath9k/999-ath9k-radiotap-antnoise.patch   # expect >= 1
mkdir -p /work/vendor/openwrt/mac80211/patches/ath9k 2>/dev/null || true
# copy into whichever package dir the build uses:
cp patches/ath9k/999-ath9k-radiotap-antnoise.patch \
   "$( [ -d /work/vendor/openwrt/mac80211 ] && echo /work/vendor/openwrt/mac80211 || echo /opt/sdk/package/kernel/mac80211 )/patches/ath9k/"
```
Expected: the `.patch` file now exists in the repo's package `patches/ath9k/` dir.

- [ ] **Step 7: Bump `PKG_RELEASE`**

In the package `Makefile` (`vendor/openwrt/mac80211/Makefile` or the SDK copy), increment
`PKG_RELEASE` — append a suffix so it clearly outranks stock, e.g. change `PKG_RELEASE:=N` to:
```make
PKG_RELEASE:=N.wfbsnr1
```
(Use the existing numeric `N` from the file; the `.wfbsnr1` suffix makes ours sort higher than
the stock `N`.)

- [ ] **Step 8: Rebuild from clean and verify it applies + compiles (the passing test)**

```bash
# inside the container (or a fresh `docker run ... sh -c`)
cd /opt/sdk
[ -d package/kernel/mac80211 ] || cp -a /work/vendor/openwrt/mac80211 package/kernel/mac80211
make package/kernel/mac80211/{clean,prepare} QUILT=0 V=s 2>&1 | grep -iE "Applying|999-ath9k-radiotap" 
make package/kernel/mac80211/compile -j"$(nproc)" V=s 2>&1 | tail -30
echo "=== apks (note the bumped release) ==="
find bin -name "kmod-mac80211_*.apk" -o -name "kmod-ath9k*_*.apk" -o -name "kmod-cfg80211_*.apk" -o -name "kmod-ath_*.apk"
```
Expected: log shows `999-ath9k-radiotap-antnoise.patch` applying; compile succeeds; the apks
are present and their filenames contain the bumped release (`...wfbsnr1...`).

- [ ] **Step 9: Wire the kmod build into `docker/sdk-build.sh` and commit**

Edit `docker/sdk-build.sh` so the non-interactive package stage also builds the bundle and
copies the kmod apks next to `wfb-ng`. After the existing `wfb-ng` build block, add:
```sh
# Build the patched mac80211/ath9k bundle (radiotap DBM_ANTNOISE for wfb-ng SNR).
[ -d package/kernel/mac80211 ] || cp -a /work/vendor/openwrt/mac80211 package/kernel/mac80211
echo 'CONFIG_PACKAGE_kmod-ath9k=y' >> .config
make defconfig
make package/kernel/mac80211/compile -j"$(nproc)" V=s
for k in kmod-cfg80211 kmod-mac80211 kmod-ath kmod-ath9k kmod-ath9k-common; do
  f=$(find bin -name "${k}_*.apk" | head -n1)
  [ -n "$f" ] && cp -v "$f" /work/build/packages/ || { echo "ERROR: $k apk missing"; exit 1; }
done
```
Then:
```bash
git add docker/sdk-build.sh vendor/openwrt/mac80211/patches/ath9k/999-ath9k-radiotap-antnoise.patch vendor/openwrt/mac80211/Makefile
git commit -m "feat: ath9k radiotap DBM_ANTNOISE patch + SDK kmod build"
```
Expected: clean commit; `git show --stat` lists the patch, the Makefile bump, and the sdk-build.sh change.

---

## Task 3: Make ImageBuilder install our kmods and build the images

Have the ImageBuilder pick our higher-release kmods over stock, build all three CPE510 images,
and assert both the override and the size budget.

**Files:**
- Modify: `docker/ib-build.sh`

**Interfaces:**
- Consumes: the kmod apks in `build/packages/` from Task 2.
- Produces: `output/*cpe510*sysupgrade.bin` / `*factory.bin` for v1/v2/v3, built against our
  `kmod-mac80211`/`kmod-ath9k`, each within `7680k`.

- [ ] **Step 1: Copy our kmod apks into the ImageBuilder's local repo**

In `docker/ib-build.sh`, alongside the existing `cp /work/build/packages/wfb-ng-*.apk packages/`,
add (before the per-profile `make image` loop):
```sh
# Our patched mac80211/ath9k kmods (higher PKG_RELEASE) override the stock ones.
cp /work/build/packages/kmod-*.apk packages/
```

- [ ] **Step 2: Assert the IB resolves OUR kmod versions (the test)**

Add, inside the `for p in $PROFILES` loop after `make image ...`, a check that the manifest
lists our release suffix:
```sh
  man=$(find bin -name "*${p}*.manifest" | head -n1)
  if [ -n "$man" ]; then
    grep -E '^kmod-mac80211 ' "$man" || true
    grep -Eq 'kmod-mac80211 .*wfbsnr' "$man" || { echo "ERROR: stock kmod-mac80211 used for $p"; exit 1; }
  fi
```

- [ ] **Step 3: Run the full build**

```bash
cd /home/gilankpam/Projects/poc/wfb-ng-openwrt
./build.sh package   # SDK: wfb-ng + patched kmods -> build/packages/
./build.sh image     # ImageBuilder: assemble images -> output/
```
Expected: `package` produces `wfb-ng-*.apk` and `kmod-mac80211_*wfbsnr1*.apk` etc. in
`build/packages/`; `image` prints the per-image size lines and `OK: all images within size
budget`, and the new manifest assertion passes for all three profiles.

- [ ] **Step 4: Confirm outputs and size budget**

```bash
ls -lh output/
for f in output/*sysupgrade.bin; do
  echo "$f: $(wc -c < "$f") bytes (max $((7680*1024)))"
done
```
Expected: three `sysupgrade.bin` (+ factory) images, each ≤ 7864320 bytes.

- [ ] **Step 5: Commit**

```bash
git add docker/ib-build.sh
git commit -m "build: ImageBuilder installs patched mac80211 kmods + asserts override"
```

---

## Task 4: On-device verification (operator-run)

Functional proof requires hardware, which can't be done from the build host. This task is a
documented procedure the operator runs; it is the acceptance gate.

**Files:**
- Create: `docs/verify-snr-on-device.md` (the runbook below)

**Interfaces:**
- Consumes: an `output/*cpe510*sysupgrade.bin` from Task 3 and a CPE510 + a wifibroadcast TX source.
- Produces: a recorded PASS/FAIL of live SNR.

- [ ] **Step 1: Write the verification runbook**

Create `docs/verify-snr-on-device.md` with:
```markdown
# Verify ath9k SNR on the CPE510

## 1. Flash
Flash `output/<rev>/...-cpe510-vX-...-sysupgrade.bin` (sysupgrade or TFTP recovery).

## 2. Driver + module sanity
SSH/serial to the device:
- `dmesg | grep -i ath9k` — no vermagic/load errors.
- `iw dev` / `iw phy` — phy present.
- `cat /sys/module/mac80211/srcversion` and compare to the built module's `modinfo`
  srcversion to confirm OUR module loaded (optional but conclusive).

## 3. Radiotap mechanism check (proves the field is emitted)
With the monitor interface up (the wfb-ng launcher creates it):
- `tcpdump -i <mon> -e -c 20 -y IEEE802_11_RADIO` — confirm frames show a noise figure, or
- capture to pcap and open in tshark/Wireshark: field `radiotap.dbm_antnoise` is **present**
  and sane (≈ -90…-105 dBm); `radiotap.dbm_antsignal` tracks the link.

## 4. Acceptance — live SNR
With a wifibroadcast TX transmitting on the configured channel, run the device's `wfb_rx`
the way the firmware launcher does, and read its stats on the operator host (wfb-cli or the
raw stats stream). Confirm the `RX_ANT` line's SNR triplet `snr_min:snr_avg:snr_max` is
**non-zero and stable**, and that `snr_avg ≈ rssi_avg − noise`.

PASS = non-zero, physically sane SNR on a live link.
```

- [ ] **Step 2: Commit the runbook**

```bash
git add docs/verify-snr-on-device.md
git commit -m "docs: on-device SNR verification runbook"
```

- [ ] **Step 3: Operator runs the runbook and records the result**

Run `docs/verify-snr-on-device.md` end to end on real hardware. If SNR stays `0`:
- re-check Step 5 of Task 2 (writer vs length lock-step) — a mismatch breaks the header;
- confirm the loaded module is ours (Task 4 Step 2);
- capture a pcap and inspect `radiotap.present` bit 6 to see whether the field is emitted at all.

---

## Self-Review

**Spec coverage:**
- Spec §3 hunk (1) struct field → Task 2 Step 3. ✓
- Spec §3 hunk (2) ath9k populate → Task 2 Step 4. ✓
- Spec §3 hunks (3)(4) writer + length lock-step → Task 2 Step 5. ✓
- Spec §3 sentinel gating → Task 2 Step 5 (`if (status->noise)`) + Global Constraints. ✓
- Spec §4 vendor + PKG_RELEASE bump + SDK build + IB override → Tasks 1–3. ✓
- Spec §5 R1 spike + buildroot fallback → Task 1 (Steps 2, 6). ✓
- Spec §5 ABI (whole bundle rebuilt/shipped) → Task 2 Step 9 copies all five kmods; Task 3 installs them. ✓
- Spec §5 R2 override-takes-effect → Task 3 Step 2 manifest assertion. ✓
- Spec §5 R3 size budget → Task 3 Step 4. ✓
- Spec §6 verification ladder (build→image→device→mechanism→acceptance) → Tasks 2–4. ✓
- Spec §1/§6 acceptance = live `RX_ANT` SNR host-side → Task 4. ✓

**Placeholder scan:** No "TBD"/"handle edge cases" — every code edit shows the exact C; the only
deferred specifics are real source line numbers, which `quilt` resolves against live source by
design (Task 2 Steps 2–6), not guesses.

**Type consistency:** Carrier field named `noise` (`s8`) everywhere — defined in `mac80211.h`
(Task 2 Step 3), set in `common.c` as `rxs->noise = ah->noise` (Step 4), read in `rx.c` as
`status->noise` (Step 5). Gate condition `status->noise` identical in writer and length (Step 5).
PKG_RELEASE suffix `wfbsnr1` is the same token asserted in the IB manifest grep (Task 3 Step 2).
Consistent.
