# AVerMedia Live Gamer Extreme (GC550) Linux Kernel Module (`gc550.ko`)

> **Fork Notice & Credits:** This repository is a Linux kernel-level fork of [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) by **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors. Building directly upon their USB 3.0 protocol reverse-engineering, [`PROTOCOL.md`](PROTOCOL.md) specification, and hardware bootstrap research, this project implements a **native Linux kernel driver (`V4L2` + `ALSA`)** for the **AVerMedia Live Gamer Extreme (GC550, `USB ID 07ca:4710` / `07ca:0550`)**.

PLEASE also note that this kernel module is under HEAVY development (I CAN confirm it works in QEMU running a CachyOS VM and on bare metal running the same versions, more on that in the TODO.md file) and, like MANY projects on GitHub, comes with NO WARRANTY and NO SUPPORT!

Q: Why make a kernel module if a userspace driver exists?

A; Well, good question. The long answer is that I had Google Antigravity make me a kernel module; it based its work on Chris Sawczuk's work without my prior knowledge, resulting in two potentially equal things. Of course, I had Antigravity properly credit everyone involved in the project, but on further reflection, that wasn't enough, so I forked his repo and moved my kernel module to his fork, properly crediting his work.

Q: If it was a "mistake", why keep it at all?

A: I spent tokens on this, and logically there should be performance benefits of having the module in the kernel instead of userspace (Spoiler warning: there is). Badly written kernel modules CAN start race conditions, leading to an unstable kernel, more or less breaking your system, so as I said above **PROCEED AT YOUR OWN RISK**

Q: Wait—if this driver is for the original LGX (GC550), why is the repo named `lgx2KernelLevel`?

A: Think of the `2` as **"to"** — **`LGX-to-KernelLevel`** (playing off upstream's `lgx2userspace`), not `LGX2-KernelLevel`! (For the LGX2 / GC551, use `@ChrisAJS`'s upstream [`lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace)).

---

## Verified Baseline & Hardware Test Environment

The active verified baseline for this driver is **Phase E1.5.2.2 / E1.6**:
- **Baseline Git Commit**: `8744e8628d2822e2419f5326795bf046d2e94160` (branch `main`)
- **Module `srcversion`**: `27D8CAC60C62239C91B8BFC`
- **Module SHA256**: `b795b207d573eaadbe23f553d25dca40a29ba5d03898b148d38c689fffa7bb97`
- **Primary Test Platform (`Titan`)**: Bare-metal AMD Ryzen 9 5900X (12C/24T), Radeon RX 9070 XT, 32 GB DDR4, CachyOS kernel `7.3.3-1-cachyos` (x86_64), Clang/LLVM 19.
- **Secondary Test Platform (`Niri`)**: Bare-metal Intel Core i7 laptop, CachyOS kernel `7.3.1-1-cachyos` (x86_64), Clang/LLVM 19.
- **V4L2 Compliance (`v4l2-compliance 1.32.0`)**:
  - Non-streaming tests: **48 passed**, 0 failed.
  - Streaming tests (MMAP, USERPTR): **57 passed**, 0 failed, 0 warnings.
  - DMABUF export (`--expbuf-device`): **Unverified**.
- **Phase E1.6 Hardware Test Results (Titan)**:
  - **30-Minute Endurance Capture (TE-01)**: **108,000 frames dequeued**, 108,001 driver-observed, 0 dropped, 0 mid-stream desyncs over 1,799.985 s at 60.001 fps (345,598,308 audio bytes parsed).
  - **Multi-Resolution Functional Checks (TR-01..TR-05)**: Short functional smoke tests across all five supported resolutions (1080p60 [300 frames], 720p60 [62 frames], 576p50 [51 frames], 480p60 [60 frames], 640x480p60 [62 frames]) with dynamically matched Raspberry Pi HDMI source modes; all captured cleanly without mid-stream errors (the four shorter captures are functional smoke tests, not full 300-frame endurance tests).
  - **Simultaneous Video & Audio Capture (TS-01)**: 1,800 video frames and 5,750,784 PCM bytes over 30.0 seconds, demonstrating **99.84% audio/video duration correspondence** during simultaneous capture (measures duration alignment; lip-sync / AV sync accuracy has not been independently measured).
  - **Physical Hot-Unplug / Reconnect**: **NOT TESTED** under active streaming (software interface unbind/bind evaluated; physical cable unplug during active streaming remains unverified).

---

## Hardware Compatibility Matrix

| Device Model | USB ID (`VID:PID`) | Driver Status | Notes |
|---|---|---|---|
| **AVerMedia Live Gamer Extreme (GC550 / LGX)** | `07ca:4710` | **Supported & Bare-Metal Verified** | Primary GC550 hardware revision (`bcdDevice 2.07`). Runtime-verified on the Niri test laptop (CachyOS `7.3.1-1-cachyos` Clang) for 3.6+ hours continuous `1080p60` (`776,295` frames, `0` desyncs) + `48 kHz` ALSA audio; DKMS build also verified against `6.18.55-2-cachyos-lts` (GCC). |
| **AVerMedia Live Gamer Extreme (GC550 / LGX)** | `07ca:0550` | **Supported (Alias Included)** | Alternate USB Product ID documented for GC550 hardware. Uses identical USB endpoints and I2C map. |
| **AVerMedia Live Gamer Extreme 2 (GC551 / LGX2)** | `07ca:0551` | **Not Supported in `gc550.ko`** | Uses an ITE IT6664 HDMI 2.0 4K front-end splitter (`0x2C`/`0x58`). Use upstream [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) for GC551. |
| **AVerMedia Live Gamer Ultra (GC553)** | `07ca:0553` | **N/A (Standard UVC)** | Works out-of-the-box with the mainline Linux `uvcvideo` driver. |

### HDMI Input Source Compatibility

| Source Signal Type | Status | Details |
|---|---|---|
| **1920×1080p, 1280×720p, 720×576p, 720×480p, 640×480p (SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2 (Rec. 709 limited range). Short functional captures (100–120 frames per mode + manual OBS format matching) verified on Niri (`7.3.1-1-cachyos`) with a Raspberry Pi 3B across 1080p60, 720p60, 576p50, 480p59.94, and 640×480p60; long-duration (3.6+ hr) endurance testing has only been performed at 1080p60 on Niri. |
| **RGB & YCbCr 4:4:4 / 4:2:2 Sources (SDR)** | **Supported** | `ADV7604 IO[0x02]` is automatically set to `0xF` (auto color-space selection from HDMI AVI InfoFrame). |
| **HDR10 / HLG / Dolby Vision (BT.2020)** | **Unsupported (Hardware Limit)** | The GC550 uses an HDMI 1.4a 8-bit SDR receiver (`ADV7604`). Turn **HDR OFF** on your source console/PC. |
| **HDCP-Protected HDMI Sources** | **Unsupported (By Design)** | Encrypted HDCP streams (e.g. PS4/PS5 with HDCP enabled, MacBooks) cannot be captured. Disable HDCP in console settings. |

---

## Features & Architecture

Instead of requiring a background `libusb` daemon and `v4l2loopback`, `gc550.ko` registers the AVerMedia Live Gamer Extreme (GC550) directly with the Linux kernel media and sound subsystems as soon as you plug it into a USB 3.0 port:

- **Native V4L2 Video Capture (`/dev/videoX` & `/dev/avermedia-gc550`)**:
  - Exposes a standard `videobuf2-vmalloc` V4L2 capture node compatible out-of-the-box with **OBS Studio**, **FFmpeg**, **mpv**, and **GStreamer**.
  - Exposes uncompressed `YUYV` / `YUY2` 4:2:2 capture at `1920x1080`, `1280x720`, `720x576`, `720x480`, and `640x480` (Rec. 709 limited range). Select the V4L2/OBS source size to match the active HDMI input.
- **Native ALSA Audio Capture (`hw:GC550Audio,0`)**:
  - Demultiplexes embedded HDMI LPCM stereo audio (`48 kHz`, `16-bit Little-Endian`) directly from USB Bulk Endpoint `0x83 IN` into an ALSA capture card (automatically picked up by PipeWire / PulseAudio).
- **Complete Hardware Wake-Up & Passthrough Support**:
  - Programs the internal **ADV7604** HDMI receiver (including 1080p60 EDID RAM), **ADV7511** HDMI passthrough transmitter, **TLV320AIC3x** analog audio codec, **Lattice ECP3** FPGA, and **PCA9539A** GPIO expander over the FX3 USB-to-I2C bridge (`EP 0x01 OUT` / `EP 0x81 IN`).
  - Automatically configures the ADV7604 input colour space (`IO 0x02`) to match RGB or YCbCr HDMI sources and resets picture brightness/contrast adjustments to neutral.
  - **AT88SC CryptoMemory Protection**: Completely omits proprietary anti-clone authentication replays from the initialization table and enforces a runtime filter (`-EPERM`) in `gc550_cmd_send()` (`kernel/gc550-cmd.c`) blocking bridge opcodes `0x05..0x07` and `0x09` so hardware attempt counters remain untouched (`0xFF`).
- **Stream Resilience**:
  - **Cross-URB Marker Straddle Reassembly**: Tracks partial `[00 FF FF]` SAV/EAV/Audio markers across USB transfer boundaries so frames never drop or misalign.
  - **16 KiB FX3 DMA Swap Reorderer**: Automatically detects and un-swaps 16 KiB ping-pong DMA buffers if the FX3 flips buffer phase on warm restarts.

---

## Performance, Latency & Architectural Payoff

While upstream [`ChrisAJS/lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace) provides a great cross-platform SDL3/`libusb` application, moving the GC550 stream parser into a Linux kernel driver (`gc550.ko`) provides measurable latency, memory-bandwidth, and integration benefits when capturing into V4L2/PipeWire applications like **OBS Studio**:

### 1. Glass-to-Glass Latency Target (`<= 50 ms`) & Historical Niri Measurement (`50 ms` / 3 Frames @ 60 Hz)
The project target is **no more than `50 ms` end-to-end glass-to-glass latency** (and lower where possible without sacrificing stability). In a point-in-time test on the **Niri** test laptop (CachyOS `7.3.1-1-cachyos`, KWin/Wayland) using a millisecond count-up timer comparing the GC550's `HDMI OUT` passthrough on a TV (`50.283 s`) against a live `mpv` V4L2 preview window flushed to the live edge (`50.233 s`):
- **Observed Glass-to-Glass Delta on Niri:** **`50 ms`** (`3 × 16.67 ms` frames at 60 fps; single-configuration observation on Niri, not a guaranteed universal maximum).
- **Analytical Three-Frame Pipeline Breakdown (Estimated, Not Independently Measured Per Stage):**
  1. **Frame 1 (`~16.7 ms` — Hardware Scanout + USB 3.0 DMA):** The GC550 scans out 1080 lines of HDMI video (`4.14 MB`) over USB 3.0 Bulk `EP 0x83 IN` until the `EAV` marker triggers `vb2_buffer_done()` in `gc550.ko`.
  2. **Frame 2 (`~16.7 ms` — GPU Shader & Wayland Compositor):** `mpv` uploads the `YUYV` frame, converts `YUYV → RGB` on the GPU, and submits the surface to KWin/Wayland.
  3. **Frame 3 (`~16.7 ms` — Laptop Display VSync & Panel Scanout):** KWin flips on the next 60 Hz VSync and the laptop LCD scans out the frame.
- **Analytical Driver Capture Latency Estimate:** **`~16–20 ms` (~1 frame at 60 Hz)**, an analytical estimate based on the `16.67 ms` 60 Hz frame scanout plus the `~1.5–2.5 ms` `512 KiB` URB completion interval (not an independently measured value).
- **Historical Niri Endurance Verification (`v0.1.0`):** Runtime-tested on **Niri** (`7.3.1-1-cachyos` Clang) over **3.6+ hours continuous `1080p60` streaming** (`776,295` frames captured with **`0` frame desyncs** and **`0` ALSA audio XRUNs**).

### 2. Kernel (`gc550.ko`) vs. Userspace (`libusb` + `v4l2loopback`) Pipeline Comparison

| Metric / Capability | Userspace Daemon (`libusb` → `v4l2loopback`) | Native Kernel Module (`gc550.ko`) |
|---|---|---|
| **USB Transfer Granularity** | `2,080,768 B` (`~2 MiB` = half a 1080p frame per transfer) | **`512 KiB`** (`1/8th` of a frame per URB) |
| **`EAV` Frame-Completion Delay** | Up to `~8.3 ms` (waits for `2 MiB` buffer to fill into next frame) | **`~1.5–2.5 ms`** (completes immediately on `512 KiB` URB boundary) |
| **Memory Copies (for OBS / V4L2)** | **3–4 copies per frame** (`libusb` → `BufferReorderer` → `FrameBuilder` → `v4l2loopback` → OBS; ~1 GB/s RAM traffic) | **1 copy** (Kernel URB buffer → `vb2` `mmap` buffer shared directly with OBS; ~250 MB/s) |
| **Context Switches per Frame** | Multiple Kernel ↔ Userspace hops (`xhci` → daemon → `v4l2loopback` → OBS) | **Zero** intermediate userspace daemon hops |
| **Audio Integration** | Internal SDL3 audio queue (requires virtual sinks to route into OBS) | **Native ALSA Capture Card (`hw:GC550Audio,0`)** visible in PipeWire & OBS |
| **Plug-and-Play UX** | Manual CLI launch; claims raw USB interface via `libusb` | **100% Plug-and-Play** (`udev` auto-loads on USB plug-in) |
| **Fault Isolation** | **Userspace isolated** (process crash only closes the app) | **Kernel ring-0** (driver bugs can cause a kernel `Oops`) |

---

## Known Issues & Limitations

1. **SDR Only (No HDR):** The internal Analog Devices `ADV7604` (HDMI RX) and `ADV7511` (HDMI TX) are HDMI 1.4a 8-bit chips. They do not support HDMI 2.0a HDR metadata or 10-bit BT.2020 colorimetry.
2. **USB 3.0 SuperSpeed (5 Gbps) Required:** Uncompressed 1080p60 YUYV video requires ~250 MB/s of USB bandwidth. Connecting the card to a USB 2.0 port or cable (`480M`) will log a warning in `dmesg` and will not stream video.
3. **Audio Requires Active Video Capture:** Because the GC550 hardware multiplexes HDMI audio packets (`00 FF FF 58`) inside the USB 3.0 video bulk stream (`EP 0x83 IN`), `/dev/videoX` must be actively streaming (e.g. open in OBS) for audio samples to flow into the ALSA capture device.
4. **Manual Resolution Selection and Frame-Rate Reporting:** The driver exposes five discrete `YUYV` sizes, but does not automatically change the V4L2 format when HDMI timing changes. Set the OBS/V4L2 source size to match the input. Frame-interval enumeration currently reports 60 fps for all sizes; a `576p50` input was observed at about 50 fps, so frame-rate reporting needs follow-up work.
5. **Cold Bootstrap Delay (~3.3 Seconds):** The very first time `/dev/videoX` is opened after plugging in the USB cable, the driver sends 6,576 I2C setup commands over USB (~3.3 seconds). Subsequent opens skip the bootstrap and start instantaneously.
6. **USB Hot-Unplug Testing Status:** Device lifetime is retained across open V4L2 and ALSA handles, and stream URBs are stopped before their resources are released. Software interface unbind/bind was evaluated, but physical cable hot-unplug during active video streaming remains unverified on the Titan hardware baseline. Reopen or reselect the capture devices after reconnecting the USB cable.
7. **Warm Stream Restart & Swap-State Trapping:** The driver retains `swap_mode` across STREAMOFF. Under rapid stream stop/start cycles (e.g. Phase E1.7 testing), if incoming 16 KiB blocks arrive out-of-phase, the driver's pair accumulation grid is unanchored to SAV headers and the detector cannot toggle back from SWAPPED to NORMAL on full frames, leading to desynchronization storms until the driver is rebound. This behavior was investigated in Phase E1.8, with diagnostic verification planned for Phase E1.9.
8. **Not Upstream-Ready Yet:** This driver is under active development and is **not ready for mainline Linux kernel upstream submission**. It requires further correctness hardening (SAV-anchored block pairing), multi-resolution phase drift validation, portability testing across non-xHCI host controllers, licensing review, and full media-subsystem API/checkpatch review.

---

## Installation

### 1. Install Prerequisites (Kernel Headers & DKMS)
Make sure you have `git`, `make`, `dkms`, and your distribution's kernel headers installed:

- **Arch Linux / CachyOS**:
  ```bash
  sudo pacman -S base-devel git dkms linux-headers
  # (If using CachyOS kernel: sudo pacman -S linux-cachyos-headers)
  ```
- **Debian / Ubuntu**:
  ```bash
  sudo apt install build-essential git dkms linux-headers-$(uname -r)
  ```
- **Fedora**:
  ```bash
  sudo dnf install kernel-devel kernel-headers make gcc git dkms
  ```

### 2. Clone & Run the Smart Installer
```bash
git clone https://github.com/GengarTV/lgx2KernelLevel.git
cd lgx2KernelLevel
./install.sh
```

`./install.sh` automatically detects your Linux distribution and selects the best installation method:
1. **On Arch Linux / CachyOS / EndeavourOS / Manjaro (Default: `PKGBUILD` + `pacman` + `DKMS`)**:
   - Runs `makepkg -si` using the included [`PKGBUILD`](PKGBUILD) to build and install the native **`avermedia-gc550-dkms`** package.
   - `pacman` automatically installs `dkms` if missing, tracks ownership of all files (`/usr/src/avermedia-gc550-0.1.3` and `/usr/lib/udev/rules.d/99-avermedia-gc550.rules`), compiles `gc550.ko` via its DKMS hook, and loads the module immediately.
2. **On Other Linux Distributions (Ubuntu, Debian, Fedora, etc. — Default: Direct Kernel Build)**:
   - Compiles `gc550.ko` against `/lib/modules/$(uname -r)/build` (`make modules && make install`), installs the udev rules, and loads the module immediately.

### Persistence Across Reboots & Kernel Updates (Rolling-Release Friendly)
- **Zero-Touch Kernel Updates (DKMS)**: On Arch/CachyOS (or when installed with `--dkms`), `pacman` owns the `avermedia-gc550-dkms` package and automatically runs its DKMS hook (`70-dkms-install.hook`) whenever a new kernel is installed. It recompiles and installs `gc550.ko` for the new kernel automatically in the background (~2 seconds)—you never have to manually recompile after a system update.
- **Automatic Plug-and-Play Loading**: The module embeds the USB device alias (`usb:v07CAp4710...` and `usb:v07CAp0550...`). Across reboots, the Linux kernel and `udev` automatically load `gc550.ko` on demand the moment the GC550 is plugged into a USB port.

### Other Installer Commands
```bash
./install.sh --arch       # Force Arch/CachyOS PKGBUILD (makepkg -si) installation
./install.sh --dkms       # Force standalone DKMS installation (for non-Arch distros)
./install.sh --direct     # Force direct compile & install for current kernel only
./install.sh --uninstall  # Unload and remove driver, pacman/DKMS package, and udev rules
```

---

## Operational Verification Checklist

After running `./install.sh` and plugging the GC550 into a **USB 3.0 (SuperSpeed)** port, run through this checklist to confirm everything is working:

1. **Confirm USB 3.0 SuperSpeed (`5000M`) & Driver Binding:**
   ```bash
   lsusb -t | grep -i gc550
   ```
   *Expected:* `|__ Port X: Dev Y, If 0, Class=Vendor Specific Class, Driver=gc550, 5000M`
2. **Confirm V4L2 Video Node & Symlink Exist:**
   ```bash
   ls -l /dev/avermedia-gc550 /dev/video*
   v4l2-ctl --list-devices
   ```
   *Expected:* `/dev/avermedia-gc550 -> video0` (or `videoX`) under `AVerMedia Live Gamer Extreme (GC550)`.
3. **Confirm ALSA Audio Capture Card Exists:**
   ```bash
   arecord -l | grep -i gc550
   ```
   *Expected:* `card X: GC550Audio [AVerMedia GC550], device 0: GC550 HDMI Audio [AVerMedia GC550 Capture]`
4. **Test 60 Frames of Video Capture & Check HDMI Signal Lock:**
   ```bash
   v4l2-ctl -d /dev/avermedia-gc550 --stream-mmap --stream-count=60
   sudo dmesg | grep -i gc550 | tail -n 10
   ```
   *Expected `dmesg` output:*
   ```text
   gc550 2-4:1.0: AVerMedia Live Gamer Extreme (GC550) [07ca:4710] detected (USB speed: super-speed)
   gc550 2-4:1.0: Registered gc550 as /dev/video0 and ALSA capture card
   gc550 2-4:1.0: HDMI Status: cable=YES, signal=LOCKED (1920x1080p, HDMI, HDCP=NO, TMDS=148MHz)
   gc550 2-4:1.0: Stream stopped: frames=60, desyncs=0, swap_mode=0
   ```
5. **Configure in OBS Studio:**
   - Add a **Video Capture Device (V4L2)** → select **AVerMedia Live Gamer Extreme (GC550)**.
   - Add an **Audio Input Capture (ALSA / PipeWire)** → select **AVerMedia GC550**.

If any step above fails, see **[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)** for solutions.

---

## Documentation Index & Repository Structure

- **[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)** — Solutions for missing kernel headers, Clang/GCC errors, Secure Boot, USB 2.0 fallback, HDCP black screens, and OBS audio.
- **[`TODO.md`](TODO.md)** — Project roadmap, completed multi-resolution test results, and remaining timing-detection work.
- **[`PROTOCOL.md`](PROTOCOL.md)** — Deep-dive USB 3.0 bulk protocol, BT.1120 SAV/EAV markers, and I2C register documentation (by `@ChrisAJS`).
- **[`CONTRIBUTING.md`](CONTRIBUTING.md)** — Driver architecture, hardware safety rules (`AT88SC`), VM testing workflow, and register table format.
- **[`CHANGELOG.md`](CHANGELOG.md)** — Release history and notable changes.

```text
├── 99-avermedia-gc550.rules # udev permissions & /dev/avermedia-gc550 symlink
├── CHANGELOG.md             # Version history and release notes
├── CONTRIBUTING.md          # Developer & maintainer guide
├── Kbuild                   # Kernel build object definitions
├── LICENCE.md               # Upstream MIT license (Chris Sawczuk)
├── LICENSE-GPL-2.0          # GNU General Public License v2 text
├── LICENSES.md              # Project licensing policy & provenance guide
├── Makefile                 # Out-of-tree kernel module Makefile (GCC & Clang/LLVM auto-detect)
├── PKGBUILD                 # Arch Linux / CachyOS pacman package build script (avermedia-gc550-dkms)
├── PROTOCOL.md              # USB 3.0 protocol & hardware specification (by @ChrisAJS)
├── TODO.md                  # Roadmap & technical design notes (including low-resolution support)
├── TROUBLESHOOTING.md       # Common build, USB, video, and audio troubleshooting guide
├── dkms.conf                # DKMS configuration
├── install.sh               # Smart distro-aware installer & uninstaller script
└── kernel/
    ├── gc550.h              # Driver structures, constants, and register definitions
    ├── gc550-core.c         # USB probe/disconnect & lifecycle management
    ├── gc550-cmd.c          # FX3 USB Bulk (0x01/0x81) to I2C bridge & safe bootstrap
    ├── gc550-regtables.h    # Complete 6,576-step GC550 hardware wake-up table (Crypto-stripped)
    ├── gc550-video.c        # V4L2 capture device, straddle parser & 16 KiB DMA swap reorderer
    └── gc550-audio.c        # ALSA PCM capture device for demuxed HDMI audio
```

---

## Credits & Upstream Links

- **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors to [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace):
  - Reverse-engineered the USB-to-I2C bridge, the GC550/GC551 initialisation sequences, the BT.1120 SAV/EAV framing checksums, the ADV7604 automatic colour-space fix, the 16 KiB FX3 DMA swap behaviour, and the AT88SC CryptoMemory anti-clone protection documented in [`PROTOCOL.md`](PROTOCOL.md).
- **Linux Kernel Media Maintainers**:
  - Authors of the upstream `adv7604` and `adv7511` drivers.

---

## Note for Windows, macOS, and Userspace Driver Users

This fork contains **only the native Linux kernel module (`gc550.ko`)**. All Windows, macOS, and cross-platform `libusb` / SDL3 userspace application files have been removed from this repository.

If you are on **Windows**, **macOS**, or want the **userspace driver (`lgx2userspace` / `lgx2d`)** or **LGX2 (GC551)** userspace support, please visit **Chris Sawczuk's (@ChrisAJS) upstream repository**:
👉 [**https://github.com/ChrisAJS/lgx2userspace**](https://github.com/ChrisAJS/lgx2userspace)

---

## Licensing & Provenance

This project contains original Linux kernel driver code and incorporates research, protocol definitions, and hardware initialization data derived from the upstream [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) project.

### 1. Original Project Code (`GPL-2.0-only OR MIT`)
All original Linux kernel driver code, headers, build scripts, and packaging authored for `lgx2KernelLevel` (Copyright (C) 2026 Øystein Lie) are dual-licensed under:

- **GNU General Public License version 2 only (`GPL-2.0-only`)** ([`LICENSE-GPL-2.0`](LICENSE-GPL-2.0)); **OR**
- **The MIT License (`MIT`)** ([`LICENCE.md`](LICENCE.md)), at your option.

This dual-licensing arrangement allows permissive MIT reuse while ensuring full compatibility with the Linux kernel's GPL-2.0 licensing requirements. The kernel module binary declares `MODULE_LICENSE("Dual MIT/GPL")`.

### 2. Upstream lgx2userspace Material (`MIT License`)
Foundational USB 3.0 protocol reverse-engineering, hardware initialization tables, BT.1120 SAV/EAV checksum validation formulas, and AT88SC CryptoMemory research are derived from [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace):

- **Copyright 2021 Chris Sawczuk <chris@sawcz.uk> and contributors**
- Licensed under the **MIT License** ([`LICENCE.md`](LICENCE.md)).

All upstream copyright notices, permissions, and conditions are preserved in full. Upstream authors have not granted additional licensing rights beyond the MIT License.

### 3. Upstream Protocol Specification
The upstream specification [`PROTOCOL.md`](PROTOCOL.md) is preserved verbatim as historical documentation under its original author's copyright and MIT licensing.

For complete details and file-by-file classifications, see [`LICENSES.md`](LICENSES.md).
