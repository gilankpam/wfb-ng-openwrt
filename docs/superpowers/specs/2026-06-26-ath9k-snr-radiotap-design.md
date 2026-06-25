# ath9k SNR via radiotap DBM_ANTNOISE — Design Spec

**Date:** 2026-06-26
**Status:** Approved (pending spec review)
**Topic:** Patch the ath9k / mac80211 driver bundle in the CPE510 firmware so wfb-ng reports real SNR.

---

## 1. Goal & scope

Make the ath9k-based CPE510 firmware emit a **real, live noise-floor value** in the
monitor-mode radiotap header (`IEEE80211_RADIOTAP_DBM_ANTNOISE`). wfb-ng already computes
`SNR = signal − noise`; today it has no noise field to work with, so SNR is always reported
as `0`. With the noise field present, the existing wfb-ng code produces a correct SNR with
**no change to wfb-ng**.

**In scope:** a single source patch to the OpenWrt `mac80211` package (covers both mac80211
core and ath9k), and the build-pipeline change needed to compile and ship that patched
kernel-module bundle.

**Success criterion (acceptance):** on a live wifibroadcast link, `wfb_rx`'s `RX_ANT` stats
line — read on the operator host (via wfb-cli or the raw stats stream) — shows a **non-zero,
stable SNR** (`snr_min:snr_avg:snr_max`), with values consistent with the link's RSSI and a
sane noise floor.

---

## 2. Background — why SNR is `0` today (verified)

- wfb-ng computes SNR per received frame in `../wfb-ng/src/rx.hpp:111`:
  ```c
  int8_t snr = (noise != SCHAR_MAX) ? rssi - noise : 0;
  ```
  `noise` is initialised to `SCHAR_MAX` (`rx.cpp:143`) and is overwritten **only** when the
  radiotap header carries `IEEE80211_RADIOTAP_DBM_ANTNOISE` (`rx.cpp:176-178`). No noise
  field ⇒ SNR falls back to `0`.
- ath9k is a `mac80211` (softmac) driver, so the monitor-mode radiotap header is built by
  mac80211 in `ieee80211_add_rx_radiotap_header()` (`net/mac80211/rx.c`), **not** by ath9k.
- Upstream mac80211 **never** emits `DBM_ANTNOISE` — the field is intentionally absent
  (the source carries the comment `/* IEEE80211_RADIOTAP_DB_ANTNOISE is not used */`). The
  per-packet `noise` member was removed from `struct ieee80211_rx_status` years ago, so no
  mac80211 driver (ath9k, mt76, …) reports per-frame noise. (Out-of-tree Realtek drivers do,
  which is why wfb-ng shows SNR there.)
- The noise floor itself is **not lost** — ath9k computes it and folds it into `signal`.
  In `ath9k_cmn_process_rssi()` (`drivers/net/wireless/ath/ath9k/common.c`):
  ```c
  rxs->signal = ah->noise + rx_stats->rs_rssi;
  ```
  `ah->noise` is the live, calibrated noise floor in dBm. Exposing it is essentially free.

How `wfb_rx` surfaces the result (`../wfb-ng/src/rx.cpp:553`):
```c
IPC_MSG("%" PRIu64 "\tRX_ANT\t%u:%u:%u\t%" PRIx64 "\t%d:%d:%d:%d:%d:%d:%d\n", ...
        it->second.snr_min, it->second.snr_sum / it->second.count_all, it->second.snr_max);
```

---

## 3. The patch — one patch, four hunks, all in the `mac80211` package

OpenWrt's `mac80211` backports bundle ships mac80211 core **and** ath9k together, so a single
patch (`package/kernel/mac80211/patches/ath9k/999-ath9k-radiotap-antnoise.patch`) covers all
of it. Exact symbol names / line numbers are pinned against the vendored 25.12.4 source at
implementation time; the shape is:

**(1) `include/net/mac80211.h` — carrier field** in `struct ieee80211_rx_status`,
immediately after `signal`:
```c
s8 signal;
s8 noise;   /* NF in dBm; 0 = not present */
```

**(2) `drivers/net/wireless/ath/ath9k/common.c` — populate it** in
`ath9k_cmn_process_rssi()`, beside the existing signal calc:
```c
rxs->signal = ah->noise + rx_stats->rs_rssi;
rxs->noise  = ah->noise;          /* expose the live calibrated NF */
```

**(3) `net/mac80211/rx.c` — writer** in `ieee80211_add_rx_radiotap_header()`, mirroring the
existing `DBM_ANTSIGNAL` block and placed in radiotap **bit order** (ANTSIGNAL = bit 5,
ANTNOISE = bit 6, so immediately after the ANTSIGNAL block):
```c
/* IEEE80211_RADIOTAP_DBM_ANTNOISE */
if (status->noise) {
    *pos = status->noise;
    rthdr->it_present |= cpu_to_le32(BIT(IEEE80211_RADIOTAP_DBM_ANTNOISE));
    pos++;
}
```

**(4) `net/mac80211/rx.c` — length accounting** in the radiotap header-length pass
(`ieee80211_rx_radiotap_hdrlen()` / its equivalent): add the matching `len += 1`, gated on
the **identical** `status->noise` condition.

**Lock-step invariant:** hunks (3) and (4) must use the exact same condition and account for
exactly one byte. Field alignment for downstream radiotap fields is handled by mac80211's
existing per-field alignment logic, which adapts to the 1-byte shift — *provided* the writer
and the length calc agree. A mismatch corrupts the header for every monitor frame.

### Design choices (alternatives considered)

- **Real NF vs. constant.** We use the real `ah->noise`. A fixed constant (e.g. −95 dBm,
  ath9k's baseband reference) is the only alternative and is strictly worse here — the real
  value is already computed and free to expose. **Rejected.**
- **Gating the emit.** We gate on `status->noise != 0`. This firmware contains **only**
  ath9k, which sets the field on every frame, so the sentinel is safe and other-driver
  behaviour is moot. The upstream-clean alternative — a dedicated `RX_FLAG_*` bit — is noted
  but not adopted (avoids spending a scarce flag bit for a single-driver image).
- **Carrier placement.** Field added right after `signal` for readability. Because the whole
  bundle is rebuilt from one source (see §5), the struct-layout change is internally
  consistent; appending at the struct tail is unnecessary.

---

## 4. Build integration (SDK-extend)

The repo builds in two stages: the **SDK** compiles packages from source, the **ImageBuilder**
assembles images from prebuilt packages. ImageBuilder cannot compile kernel modules, so the
patched bundle is built in the SDK and handed to the ImageBuilder — the same pattern already
used for the `wfb-ng` package.

1. **Vendor** `package/kernel/mac80211` from the OpenWrt 25.12.4 source into the repo, with
   our patch added under its `patches/ath9k/`. Pin the upstream source revision in
   `versions.env`.
2. **Bump `PKG_RELEASE`** once on the vendored `mac80211` package. Every kmod it emits
   (`kmod-mac80211`, `kmod-ath9k`, `kmod-ath9k-common`, `kmod-ath`, `kmod-cfg80211`) then
   outranks the stock release, so the ImageBuilder selects ours.
3. **SDK step** (`docker/sdk-build.sh`): build the vendored package
   (`make package/kernel/mac80211/compile`) and collect the resulting `kmod-*.apk` into
   `build/packages/` alongside `wfb-ng-*.apk`.
4. **ImageBuilder step** (`docker/ib-build.sh`): copy those kmod apks into the IB's local
   `packages/` dir (mechanism already in place for `wfb-ng`), so the IB installs our
   higher-release modules. Assert in the build log that the resolved
   `kmod-mac80211` / `kmod-ath9k` are our versions.

SDK and ImageBuilder are pinned to the **same** OpenWrt release (25.12.4), so kernel vermagic
matches and SDK-built modules load on the IB-produced image.

---

## 5. ABI, correctness & risks

- **ABI consistency.** Adding a field to `ieee80211_rx_status` changes its layout, so every
  module compiled against it must agree. We rebuild the **entire** `mac80211` bundle from the
  patched source and ship all of it together; nothing in the image links the struct against
  stock headers.
- **R1 — can the SDK build `mac80211`? (highest risk).** The SDK does not ship
  `package/kernel/mac80211`, and the backports build needs the kernel prepared for module
  builds. **First implementation step is a spike**: vendor the package in and run
  `make package/kernel/mac80211/{prepare,compile}` in the SDK container. If it cannot build
  there, **fall back** to a full-buildroot stage (new `Dockerfile.buildroot`) that produces
  only the kmod apks, keeping the ImageBuilder for the image. Decision gate before any
  patch work proceeds.
- **R2 — IB pulls stock mac80211 instead of ours.** Mitigated by the `PKG_RELEASE` bump;
  verified by asserting resolved package versions in the IB log.
- **R3 — image size.** Budget is `7680k`. Recompiled kmods are ~the same size as stock;
  re-run the existing size assertion in `ib-build.sh`.
- **R4 — exact upstream symbols drift.** `ieee80211_add_rx_radiotap_header` /
  `ieee80211_rx_radiotap_hdrlen` shapes are stable across recent kernels but pinned against
  the vendored source at implementation time, not assumed.

---

## 6. Verification plan

Ordered, each gating the next; ends on hardware (per the chosen acceptance bar).

1. **Spike (R1):** vendored `mac80211` compiles in the SDK (or the buildroot fallback is
   selected). Gate for everything below.
2. **Build:** patched bundle compiles; `kmod-mac80211` + `kmod-ath9k` apks produced with the
   bumped release.
3. **Image:** ImageBuilder builds all three CPE510 profiles, installs **our** kmod versions
   (asserted), and every `*sysupgrade.bin` stays within the `7680k` budget.
4. **Device boot:** flash a CPE510; ath9k comes up and goes to monitor mode
   (`dmesg`, `iw dev`), no module-load / vermagic errors.
5. **Mechanism:** on the monitor interface, `tcpdump -i <mon> -e` / tshark shows
   `radiotap.dbm_antnoise` **present** with a sane value (≈ −90…−105 dBm) and
   `radiotap.dbm_antsignal` tracking the link.
6. **Acceptance (operator runs):** on a live link, `wfb_rx`'s `RX_ANT` stats line — observed
   on the operator host via wfb-cli or the raw stats stream — reports a non-zero, stable SNR
   triplet (`snr_min:snr_avg:snr_max`) consistent with RSSI − noise.

---

## 7. Non-goals

- No change to wfb-ng source — the fix is entirely driver-side.
- No new on-device UI / wfb-cli on the CPE510 (it remains a minimal `wfb_rx` forwarder; SNR
  is read host-side, consistent with the existing firmware design).
- Not targeting upstream submission; the patch is a local build-tree patch (the
  sentinel-gating choice reflects that).
- No attempt to add per-chain noise (multiple `DBM_ANTNOISE` per antenna); a single noise
  value per frame is sufficient for wfb-ng's SNR.

---

## 8. To pin at implementation

- Exact OpenWrt 25.12.4 kernel version and the exact line numbers / symbol forms in
  `rx.c`, `common.c`, `mac80211.h` (and confirm the `BIT()` vs `1 << …` macro style in-tree).
- The precise set of kmod apks the ImageBuilder needs from our build to satisfy dependencies
  at the bumped release.
- R1 outcome (SDK vs buildroot) — recorded in the plan before patch work begins.
