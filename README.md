# Minuet

A Rust/Slint touchscreen Spotify Connect player for an original NVIDIA Jetson
Nano, with Home Assistant control through its Spotify integration.

## Architecture

- **Minuet:** a 1024x600 touchscreen UI, using Slint's FemtoVG renderer and
  explicitly requesting OpenGL ES 3.0. The target is a custom direct
  EGLDevice/EGLStream backend, with no X11 or desktop environment. The initial
  application is a compile-checkable UI prototype, not a working playback client.
- **go-librespot:** an independent Spotify Connect playback process. The planned
  UI connection is REST and WebSocket on `127.0.0.1:3678`. Playback should not
  depend on the UI or Home Assistant being available.
- **Home Assistant:** its Spotify integration controls the Connect device through
  Spotify. It does not need access to the local go-librespot API. Spotify
  credentials for Home Assistant are configured separately from device pairing.

Start with ALSA HDMI output. A pipe backend can be added if custom audio routing
becomes necessary.

## Development

Rust 1.92.0 and Slint 1.18.1 are pinned. Cargo limits compilation to two jobs and
omits development debug information to keep native builds manageable on 4 GB RAM.
Rust is installed in `~/.cargo/bin`, and Go 1.27.2 in
`~/.local/lib/go1.27.2`, with launchers in `~/.local/bin`. New Bash sessions
include both bin directories in PATH. Existing terminals can run
`. "$HOME/.cargo/env"; export PATH="$HOME/.local/bin:$PATH"` or use absolute
commands:

```sh
~/.cargo/bin/cargo build --locked
~/.cargo/bin/cargo fmt --check
~/.cargo/bin/cargo clippy --locked --all-targets -- -D warnings
```

VS Code build and check tasks are provided. Git, GCC/G++, pkg-config, EGL/GLES,
X11, input, font and ALSA development dependencies are installed, along with the
Ogg/Vorbis/FLAC/MP3 development libraries needed for go-librespot.

The current prototype uses Slint's built-in X11 backend solely as a compilation
harness. It is **not runnable on this board's installed Xorg**. On a separate,
compatible X11/EGL development environment it can be launched with:

```sh
./target/debug/minuet
```

The next graphics milestone is to replace that harness with a custom Slint
platform implementation using EGLDevice/EGLStream for output and USB touch input.
It has no software-renderer fallback: graphics failures must be visible rather
than silently running on the CPU.

## Board baseline

- Devuan 6 (excalibur), aarch64.
- Original Nano, approximately 4 GB RAM and 2 GB swap.
- NVIDIA L4T 32.7.6, kernel `4.9.337-tegra`.
- NVIDIA EGL 1.5 initialized successfully through `EGL_PLATFORM_DEVICE_EXT`.
- A hardware GLES 3 context was created successfully. It reports
  `NVIDIA Tegra X1 (nvgpu)/integrated`, OpenGL ES 3.2 NVIDIA 32.7.6.
- No `/dev/dri` node at initial inspection.
- BTT HDMI7: HDMI reports 1024x600 at approximately 60 Hz.
- USB touch identified as `BIQU BTT-HDMI7`, USB ID `04d9:8030`.
- ALSA lists `tegrahda` and `tegrasndt210ref`.

Preserve the installed NVIDIA stack. Generic modern DRM/GBM or Wayland recipes
must not be assumed compatible with this legacy Tegra driver.

Devuan's Xorg 21.1.16 reports video ABI 25.2; the installed NVIDIA driver rejects
that ABI and exits with no screens found. Do not work around this using
`-ignoreABI`. The selected direction is direct EGLDevice/EGLStream rather than
downgrading Xorg or replacing the NVIDIA stack.

Headless EGL/GLES initialization and touchscreen enumeration are verified.
A direct EGLStream test on output layer 0 submitted 4555 frames in 90 seconds
without API errors, but the user reported no visible test pattern; the console
returned after exit. Successful buffer submission is therefore not proof of
visible scanout.

Investigation mapped EGL layer 0 to CRTC 1000 and the connected HDMI connector
1000. A syscall trace showed the NVDC consumer issuing `FBIOBLANK(4)` on every
frame, followed by display flips failing with `ENXIO`, despite successful EGL
buffer swaps. The controller's `enable` attribute was 0 during the test.

NVDC initially reports atomic CRTC `ACTIVE=0` even while the framebuffer console
is lit. Legacy `drmModeSetCrtc()` alone did not fix the blank screen. The revised
diagnostic enables `DRM_CLIENT_CAP_ATOMIC`, commits `ACTIVE=1` and a `MODE_ID` blob
for the existing mode, and passes the same NVDC handle to EGL using
`EGL_DRM_MASTER_FD_EXT`. A three-second trace then recorded 108 successful display
flip ioctls, no powerdown requests, and no display `ENXIO` failures.

The corrected 90-second run submitted 4462 frames. The user confirmed correctly
sized RGB bars, the moving cyan marker and white corner targets, followed by
console restoration. Direct accelerated HDMI scanout is therefore verified.

**Touch is verified** with the kernel's built-in `hid-multitouch` driver. In a
90-second run with the panel's internal touch ribbon reseated, 5169 frames were
submitted. The user confirmed the yellow marker sat under their finger at all
four corners, the center and while dragging, with no offset, mirroring or
scaling. Mapped contacts spanned X 0-1022 and Y 4-597. Raw touch X/Y ranges are
0-4096 and map linearly to 1024x600; no calibration is required.

### Touch fault (resolved)

Touch initially failed on the Jetson, a second Linux machine and finally Windows. The controller
(Holtek-based, USB `04d9:8030`) enumerated normally but streamed a constant,
invalid report about 12-16 times per second, touched or not: five "active"
contacts with ID 255 and out-of-range coordinates. Its fields contained
uninitialized-looking values such as `0xCCCC` and `0xFFFC`. Neither driver
changes, power cycling, other USB ports nor disconnecting HDMI helped.

**Root cause:** a poorly seated internal touch ribbon cable between the glass and
the controller board. After it was reseated, the controller reports valid single
and multi-contact data with proper down/up transitions, and stays silent when
idle. The same symptom recurring would point back to that connector first.

The diagnostics only issued two read-only HID feature requests; nothing was
written to the panel. BIGTREETECH's hardware repository contains manuals only,
with no touch firmware or Linux touch guidance. Its issue #43 reports incorrect
HDMI7 V1.2 EDID timing and sync polarity, which is relevant to portability but
not to touch.

```sh
sudo python3 tools/touch-capture.py 25          # per-change log
sudo python3 tools/touch-capture.py 30 --quiet  # distinct-report summary
```

No display server is left running and no boot service is enabled.

### Direct display diagnostic

Build the standalone diagnostic with EGL/GLES and `libdrm-dev` development
headers, explicitly linking NVIDIA's NVDC shim:

```sh
mkdir -p target/diagnostics
cc -std=c11 -Wall -Wextra -Werror -O2 tools/display-touch-test.c \
    -o target/diagnostics/display-touch-test \
    $(pkg-config --cflags --libs egl glesv2) \
    $(pkg-config --cflags libdrm) -l:libdrm_nvdc.so
sudo timeout --signal=TERM --kill-after=5s 100s \
    ./target/diagnostics/display-touch-test --display --configure-output
```

With `--configure-output`, it selects the connected NVDC connector and matching
EGL output layer, verifies the existing mode is 1024x600, initializes its atomic
display state, and attempts an RGB test pattern for 90 seconds. Without this
option it reproduces the original, unsuccessful layer-0 test.
It reads only the BTT touchscreen's stable input path, without grabbing input
or changing permissions.
Primary-finger coordinates are logged, with a yellow marker drawn at the mapped
position. The program releases its EGL resources on normal exit or SIGTERM;
the external timeout also bounds a blocked driver call.

Root is used for touchscreen access, not as a proposed application deployment
model. Display configuration is process-local through NVDC, uses the existing
mode timing, and does not change boot files. Confirm visible output, touch
alignment and console restoration physically.

For a read-only connector, CRTC and atomic-property report:

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 tools/nvdc-info.c \
    -o target/diagnostics/nvdc-info \
    $(pkg-config --cflags libdrm) -l:libdrm_nvdc.so
sudo ./target/diagnostics/nvdc-info
```

## Spotify bring-up

The template in [config/go-librespot.yml](config/go-librespot.yml) targets
[go-librespot v0.10.5](https://github.com/devgianlu/go-librespot/tree/v0.10.5).
Confirm the HDMI ALSA PCM with `aplay -l` before using it.

Keep runtime configuration, credentials and caches outside the repository.
Device authorization uses `credentials.type: device_auth`; while pairing is
pending, `GET /auth/code` exposes the link and code for display in the UI.
Never expose the unauthenticated control API beyond loopback by default.

The direct display backend, daemon, pairing UI, playback controls and Home
Assistant setup are subsequent milestones, not implemented by this bootstrap.

## License

Minuet is licensed under **GPL-3.0-only**; see [LICENSE](LICENSE).

- [Slint 1.18.1 licensing](https://github.com/slint-ui/slint/blob/v1.18.1/LICENSE.md)
  offers GPLv3 for embedded use. Minuet uses that option, not Slint's
  royalty-free non-embedded license.
- [go-librespot licensing](https://github.com/devgianlu/go-librespot/blob/v0.10.5/LICENSE)
  is GPLv3. It remains a separate service; its upstream license and any
  redistribution obligations must be preserved.

Distributing binaries or an appliance requires satisfying applicable GPL source,
notice and installation-information obligations, along with third-party
dependency licenses. Spotify service/API terms are separate from software
licensing.
