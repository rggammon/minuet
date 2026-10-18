# Minuet — agent guidelines

Rust/Slint touchscreen Spotify Connect player for an original Jetson Nano (Devuan 6,
L4T 32.7.6, kernel 4.9) driving a 1024x600 BTT HDMI7 with USB touch. Board bring-up
history, licensing rationale and diagnostics are in [README.md](README.md).

## Build and test

- Toolchains: Rust 1.92.0 (`~/.cargo/bin`, pinned in `rust-toolchain.toml`), Go 1.27.2
  (`~/.local/bin`). Non-login shells may need absolute paths.
- `cargo build --locked`, `cargo fmt --check`,
  `cargo clippy --locked --all-targets -- -D warnings` must pass.
- Builds are slow on the Nano: a cold build takes about 13 minutes, and Cargo uses
  two jobs to fit 4 GB RAM. Run long builds asynchronously; don't run a second Cargo
  command concurrently, since it only waits on the build-directory lock.
- Keep Slint pinned (`=1.18.1`). Cargo falls back to dependencies compatible with
  Rust 1.92.

## Display: no X11, no DRM/GBM

- Devuan's Xorg ABI is rejected by the NVIDIA driver; never use `-ignoreABI`, and
  never replace or upgrade NVIDIA L4T packages.
- There is no `/dev/dri`. Output goes through NVIDIA's NVDC shim: link
  `-l:libdrm_nvdc.so` and call `drmOpen("drm-nvdc", NULL)`. Then use
  EGLDevice → EGLStream → EGLOutput.
- **Required, or the screen stays blank while `eglSwapBuffers` still succeeds:**
  `drmSetMaster`, `drmSetClientCap(DRM_CLIENT_CAP_ATOMIC, 1)`, and an atomic commit
  of CRTC `ACTIVE=1` plus a `MODE_ID` blob of the existing 1024x600 mode, using flags 0.
  Pass the same fd to EGL via `EGL_DRM_MASTER_FD_EXT`. Choose the output layer by
  matching `EGL_DRM_CRTC_EXT` to the connected CRTC.
- [tools/display-touch-test.c](tools/display-touch-test.c) is the verified reference
  implementation; mirror it in the Slint platform backend.
- No software-renderer fallback: graphics failures must be visible.
- `src/main.rs` currently uses Slint's winit/X11 backend only as a compile harness;
  it cannot run on this board.

## Touch

- Device: `/dev/input/by-id/usb-BIQU_BTT-HDMI7_SN0000000003-event-if00`, using the
  kernel `hid-multitouch` driver (type B multitouch, 5 slots). Raw X/Y are 0-4096
  and map linearly to 1024x600, with no calibration or rotation.
- If touch emits constant invalid reports (contact ID 255, X 65532), the panel's
  internal touch ribbon is unseated. That is a hardware fault, not a software one.
- Diagnose with `sudo python3 tools/touch-capture.py 30 --quiet`.

## Conventions

- Licence GPL-3.0-only; add `// SPDX-License-Identifier: GPL-3.0-only` to new
  source files.
- go-librespot runs as a separate service: REST/WebSocket on `127.0.0.1:3678`,
  `device_auth` pairing via `GET /auth/code`, ALSA HDMI output
  (`plughw:CARD=tegrahda,DEV=3`). Never bind its API beyond loopback, and keep
  credentials/caches out of the repository.
- Home Assistant controls playback through its Spotify integration, not this API.
- The app must eventually run as `jetson`, not root. `video` group membership exists;
  touch access still needs `input` group membership or a udev rule.
- Don't enable boot services or modify boot configuration without asking.
