# GitHub repo + Actions (rolling firmware builds) — Design Spec

**Date:** 2026-06-25
**Status:** Approved (pending spec review)
**Topic:** Publish the wfb-ng CPE510 build to a public GitHub repo and add a GitHub Actions
workflow that builds the firmware on every push to `master` and publishes it as a rolling release.

---

## 1. Goal & scope

Make this build project public on GitHub and automate firmware delivery:

- Create a **public** GitHub repository and push the existing `master` branch.
- Add **one** GitHub Actions workflow that, on every push to `master` (and on manual dispatch),
  runs the existing Docker build (`./build.sh all`) and publishes the resulting CPE510 images to a
  continuously-updated **`latest` prerelease**.
- Make the minimal supporting changes: a `build.sh` flag so CI can reuse pre-built (buildx-cached)
  Docker images, and README badges/links.

**Non-goals:** version-tagged stable releases, PR/branch validation builds, publishing the `.apk`
or to an apk/opkg repository, multi-arch/other devices, signing of release assets, self-hosted
runners. (All are possible later; out of scope here.)

---

## 2. Context & constraints (already true in this repo)

- `gh` is installed and authenticated as `gilankpam` (can create the repo and push).
- The build source fork `github.com/gilankpam/wfb-ng` is **public**, so CI clones the pinned
  `swfec` commit with no secret.
- The build is **Docker-based**: `build.sh` builds two images (`wfbng-sdk` ~3.5 GB,
  `wfbng-ib` ~1.7 GB) then runs the package + image stages. A full `./build.sh all` is ~25 min cold.
- `output/` (the 6 `.bin` images + `drone.key`) is gitignored and produced by the build.
- The committed `keys/gs.key` + `keys/drone.key` are a **deliberately shared, insecure test pair**;
  publishing them in a public repo is acceptable and already documented (regenerate for a real link).
- Actions minutes are **free** on public repos, so build frequency is not cost-constrained.

---

## 3. Repository creation (one-time)

```sh
gh repo create gilankpam/wfb-ng-openwrt --public --source=. --remote=origin --push \
  --description "Minimal wfb-ng ground-station firmware for the TP-Link CPE510 (OpenWrt 25.12)"
```

- Repo name: **`wfb-ng-openwrt`**; visibility: **public**; default branch: `master`.
- Pushes the current `master` (all existing history) and sets `origin`.

---

## 4. Workflow: `.github/workflows/build.yml`

### Triggers
- `push:` on `branches: [master]`
- `workflow_dispatch:` (manual "Run workflow" button)

### Top-level settings
- `permissions: contents: write` — required to create/update the release and move the `latest` tag.
- `concurrency: { group: rolling-build, cancel-in-progress: true }` — a newer push cancels an
  in-flight build so the rolling release reflects the latest commit.

### Job (`build`, `runs-on: ubuntu-latest`) steps, in order

1. **Checkout** — `actions/checkout@v4`.
2. **Free disk space** — `jlumbroso/free-disk-space@main` (remove preinstalled dotnet/android/ghc
   etc.) so the ~5 GB of Docker images plus the multi-GB OpenWrt `build_dir` fit on the runner.
3. **Set up Buildx** — `docker/setup-buildx-action@v3`.
4. **Build SDK image** — `docker/build-push-action@v6`:
   - `context: docker`, `file: docker/Dockerfile.sdk`
   - `tags: wfbng-sdk:${OPENWRT_VERSION}` (must match what `build.sh` expects)
   - `load: true`
   - `cache-from: type=gha,scope=sdk`, `cache-to: type=gha,scope=sdk,mode=max`
   - `build-args:` `OPENWRT_VERSION`, `OPENWRT_TARGET`, `OPENWRT_SUBTARGET` (read from `versions.env`)
5. **Build ImageBuilder image** — same action with `file: docker/Dockerfile.imagebuilder`,
   `tags: wfbng-ib:${OPENWRT_VERSION}`, `scope=ib`.
6. **Build firmware** — `WFB_REUSE_IMAGES=1 ./build.sh all`. With the flag set (see §5) `build.sh`
   skips rebuilding the Docker images (buildx already built+loaded them) and runs: launcher tests →
   SDK package + qemu FEC self-test → ImageBuilder (3 variants) → size assertion.
7. **Publish rolling release** — `softprops/action-gh-release@v2`:
   - `tag_name: latest`, `prerelease: true`
   - `name: Latest firmware build (<short-sha>)`
   - `files: output/*`
   - `body:` generated notes (see §6).

`${OPENWRT_VERSION}` etc. are exported by sourcing `versions.env` in a step (or via a step that
emits them to `$GITHUB_ENV`), so the image tags/build-args stay pinned in one place.

---

## 5. `build.sh` change (CI image reuse)

Add a one-line guard at the top of each image-build function:

```sh
build_sdk_image() {
  [ -n "${WFB_REUSE_IMAGES:-}" ] && return 0
  docker build ...   # unchanged
}
build_ib_image() {
  [ -n "${WFB_REUSE_IMAGES:-}" ] && return 0
  docker build ...   # unchanged
}
```

- **CI** sets `WFB_REUSE_IMAGES=1` (buildx already built and `--load`ed the images), so `build.sh`
  reuses them instead of rebuilding.
- **Local** default is unchanged: `WFB_REUSE_IMAGES` unset → images build as before (Docker layer
  cache keeps that fast, and local Dockerfile edits still take effect).
- Documented in the README/`build.sh` comment.

---

## 6. Rolling release behavior

- A single GitHub Release tagged **`latest`**, marked **prerelease**, whose **assets are replaced**
  on each run and whose **tag re-points** to the built commit (handled by
  `softprops/action-gh-release@v2` with a fixed `tag_name`).
- Assets: the six `…tplink_cpe510-v{1,2,3}-squashfs-{factory,sysupgrade}.bin` files and `drone.key`.
- Release notes generated by the workflow include: this repo's commit SHA, the pinned wfb-ng commit
  (`WFB_COMMIT` from `versions.env`), the OpenWrt version, the per-image sizes, and the standard
  **test-key warning** (regenerate + reflash for a real link).

---

## 7. README updates

- Add a **CI status badge** (workflow status) at the top.
- Add a "**Download the latest firmware**" link to the `latest` release
  (`https://github.com/gilankpam/wfb-ng-openwrt/releases/latest`).
- One line noting CI builds every push to `master` into the rolling `latest` release.

---

## 8. Verification

- **Workflow validity:** the YAML parses and the job graph is well-formed (lint locally if a tool is
  available; otherwise the first push exercises it).
- **End-to-end:** after the repo is created and `master` pushed, the first workflow run completes
  green and produces a `latest` release carrying the 6 images + `drone.key`; a downloaded
  `sysupgrade.bin` matches the local build's size.
- **`build.sh` flag:** `WFB_REUSE_IMAGES=1 ./build.sh all` locally (with the images already present)
  skips the image builds and still produces the images; unset still builds them.
- **Cache:** the second workflow run is materially faster than the first (GHA cache hit on the
  SDK/IB layers), confirming caching works.

---

## 9. Risks & notes

- **Cold first run** ≈25 min (populates the GHA cache); warm runs ≈12–15 min.
- **Runner disk** is the main risk; the free-disk step is required. The OpenWrt `build_dir` is the
  largest consumer.
- **GHA cache** is limited to ~10 GB/repo with LRU eviction; the SDK/IB layer caches should fit, but
  a cold rebuild is the worst case if evicted.
- **Public test keys** ship in the repo by design — documented as insecure/test-only.
- **`cancel-in-progress`** means a rapid series of pushes only publishes the last; intended for a
  rolling release.

---

## 10. Deliverables

- A public `gilankpam/wfb-ng-openwrt` repo with `master` pushed.
- `.github/workflows/build.yml` (rolling build + publish).
- `build.sh` `WFB_REUSE_IMAGES` guard.
- README badge + latest-release link.
- A green first workflow run with a populated `latest` prerelease.
