# AVerMedia Live Gamer Extreme (GC550) Linux Kernel Module (`gc550.ko`)

> **Fork Notice & Credits:** This repository is a Linux kernel-level fork of [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) by **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors. Building directly upon their USB 3.0 protocol reverse-engineering, [`PROTOCOL.md`](PROTOCOL.md) specification, and hardware bootstrap research, this project implements a **native Linux kernel driver (`V4L2` + `ALSA`)** for the **AVerMedia Live Gamer Extreme (GC550, `USB ID 07ca:4710` / `07ca:0550`)**.

PLEASE also note that this kernel module is under HEAVY development (so heavy, in fact, I have not tried it outside of a VM yet, but I CAN confirm it works in QEMU running a CachyOS VM) and, like MANY projects on GitHub, comes with NO WARRANTY and NO SUPPORT!

Q: Why make a kernel module if a userspace driver exists?

A; Well, good question. The long answer is that I had Google Antigravity make me a kernel module; it based its work on Chris Sawczuk's work without my prior knowledge, resulting in two potentially equal things. Of course, I had Antigravity properly credit everyone involved in the project, but on further reflection, that wasn't enough, so I forked his repo and moved my kernel module to his fork, properly crediting his work.

Q: If it was a "mistake", why keep it at all?

A: I spent tokens on this, and logically there should be performance benefits of having the module in the kernel instead of userspace. Badly written kernel modules CAN start race conditions, leading to an unstable kernel, more or less breaking your system, so as I said above **PROCEED AT YOUR OWN RISK**

---

## Hardware Compatibility Matrix

| Device Model | USB ID (`VID:PID`) | Driver Status | Notes |
|---|---|---|---|
| **AVerMedia Live Gamer Extreme (GC550 / LGX)** | `07ca:4710` | **Supported & Bare-Metal Verified** | Primary GC550 hardware revision (`bcdDevice 2.07`). Verified on bare-metal CachyOS (`7.3.1-cachyos` Clang & `6.18.55-lts` GCC) for 3.6+ hours continuous `1080p60` (`776,295` frames, `0` desyncs) + `48 kHz` ALSA audio. |
| **AVerMedia Live Gamer Extreme (GC550 / LGX)** | `07ca:0550` | **Supported (Alias Included)** | Alternate USB Product ID documented for GC550 hardware. Uses identical USB endpoints and I2C map. |
| **AVerMedia Live Gamer Extreme 2 (GC551 / LGX2)** | `07ca:0551` | **Not Supported in `gc550.ko`** | Uses an ITE IT6664 HDMI 2.0 4K front-end splitter (`0x2C`/`0x58`). Use upstream [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) for GC551. |
| **AVerMedia Live Gamer Ultra (GC553)** | `07ca:0553` | **N/A (Standard UVC)** | Works out-of-the-box with the mainline Linux `uvcvideo` driver. |

### HDMI Input Source Compatibility

| Source Signal Type | Status | Details |
|---|---|---|
| **1920×1080p @ 60 Hz / 59.94 Hz / 50 Hz (SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2 (Rec. 709 limited range) + 48 kHz 16-bit stereo LPCM audio. Bare-metal verified with **Nintendo Switch** (video + audio) and **Raspberry Pi 3B**. |
| **RGB & YCbCr 4:4:4 / 4:2:2 Sources (SDR)** | **Supported** | `ADV7604 IO[0x02]` is automatically set to `0xF` (auto color-space selection from HDMI AVI InfoFrame). |
| **HDR10 / HLG / Dolby Vision (BT.2020)** | **Unsupported (Hardware Limit)** | The GC550 uses an HDMI 1.4a 8-bit SDR receiver (`ADV7604`). Turn **HDR OFF** on your source console/PC. |
| **HDCP-Protected HDMI Sources** | **Unsupported (By Design)** | Encrypted HDCP streams (e.g. PS4/PS5 with HDCP enabled, MacBooks) cannot be captured. Disable HDCP in console settings. |

---

## Features & Architecture

Instead of requiring a background `libusb` daemon and `v4l2loopback`, `gc550.ko` registers the AVerMedia Live Gamer Extreme (GC550) directly with the Linux kernel media and sound subsystems as soon as you plug it into a USB 3.0 port:

- **Native V4L2 Video Capture (`/dev/videoX` & `/dev/avermedia-gc550`)**:
  - Exposes a standard `videobuf2-vmalloc` V4L2 capture node compatible out-of-the-box with **OBS Studio**, **FFmpeg**, **mpv**, and **GStreamer**.
  - Streams uncompressed `1920x1080 @ 60fps` (`YUYV` / `YUY2` 4:2:2, Rec. 709 limited range).
- **Native ALSA Audio Capture (`hw:GC550Audio,0`)**:
  - Demultiplexes embedded HDMI LPCM stereo audio (`48 kHz`, `16-bit Little-Endian`) directly from USB Bulk Endpoint `0x83 IN` into an ALSA capture card (automatically picked up by PipeWire / PulseAudio).
- **Complete Hardware Wake-Up & Passthrough Support**:
  - Programs the internal **ADV7604** HDMI receiver (including 1080p60 EDID RAM), **ADV7511** HDMI passthrough transmitter, **TLV320AIC3x** analog audio codec, **Lattice ECP3** FPGA, and **PCA9539A** GPIO expander over the FX3 USB-to-I2C bridge (`EP 0x01 OUT` / `EP 0x81 IN`).
  - Automatically configures the ADV7604 input colour space (`IO 0x02`) to match RGB or YCbCr HDMI sources and resets picture brightness/contrast adjustments to neutral.
  - **AT88SC CryptoMemory Protection**: Completely omits and blocks proprietary anti-clone authentication replays (`opcodes 0x05..0x07`), ensuring hardware attempt counters remain untouched (`0xFF`).
- **Stream Resilience**:
  - **Cross-URB Marker Straddle Reassembly**: Tracks partial `[00 FF FF]` SAV/EAV/Audio markers across USB transfer boundaries so frames never drop or misalign.
  - **16 KiB FX3 DMA Swap Reorderer**: Automatically detects and un-swaps 16 KiB ping-pong DMA buffers if the FX3 flips buffer phase on warm restarts.

---

## Known Issues & Limitations

1. **SDR Only (No HDR):** The internal Analog Devices `ADV7604` (HDMI RX) and `ADV7511` (HDMI TX) are HDMI 1.4a 8-bit chips. They do not support HDMI 2.0a HDR metadata or 10-bit BT.2020 colorimetry.
2. **USB 3.0 SuperSpeed (5 Gbps) Required:** Uncompressed 1080p60 YUYV video requires ~250 MB/s of USB bandwidth. Connecting the card to a USB 2.0 port or cable (`480M`) will log a warning in `dmesg` and will not stream video.
3. **Audio Requires Active Video Capture:** Because the GC550 hardware multiplexes HDMI audio packets (`00 FF FF 58`) inside the USB 3.0 video bulk stream (`EP 0x83 IN`), `/dev/videoX` must be actively streaming (e.g. open in OBS) for audio samples to flow into the ALSA capture device.
4. **Fixed 1920×1080p60 V4L2 Format:** The driver currently exposes a fixed `1920×1080` `YUYV` format (`4,147,200` bytes/frame), matching the preferred EDID mode advertised by the card.
5. **Cold Bootstrap Delay (~3.3 Seconds):** The very first time `/dev/videoX` is opened after plugging in the USB cable, the driver sends 6,576 I2C setup commands over USB (~3.3 seconds). Subsequent opens skip the bootstrap and start instantaneously.
6. **Close Capture Apps Before Unplugging USB (`v0.1.0`):** Hot-unplugging the **HDMI** cable while streaming works cleanly, but hot-unplugging the **USB 3.0** cable while an application (`OBS`, `mpv`) still holds `/dev/videoX` open can trigger a use-after-free in `v4l2_ioctl` (tracked in [`TODO.md`](TODO.md) for reference-counted `v4l2_dev.release` teardown). Always close `OBS` / `mpv` before unplugging the USB cable.

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
   - `pacman` automatically installs `dkms` if missing, tracks ownership of all files (`/usr/src/avermedia-gc550-0.1.0` and `/usr/lib/udev/rules.d/99-avermedia-gc550.rules`), compiles `gc550.ko` via its DKMS hook, and loads the module immediately.
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
- **[`TODO.md`](TODO.md)** — Project roadmap and technical plan for multi-resolution (`720p` / `480p` / `576p`) support.
- **[`PROTOCOL.md`](PROTOCOL.md)** — Deep-dive USB 3.0 bulk protocol, BT.1120 SAV/EAV markers, and I2C register documentation (by `@ChrisAJS`).
- **[`CONTRIBUTING.md`](CONTRIBUTING.md)** — Driver architecture, hardware safety rules (`AT88SC`), VM testing workflow, and register table format.
- **[`CHANGELOG.md`](CHANGELOG.md)** — Release history and notable changes.

```text
├── 99-avermedia-gc550.rules # udev permissions & /dev/avermedia-gc550 symlink
├── CHANGELOG.md             # Version history and release notes
├── CONTRIBUTING.md          # Developer & maintainer guide
├── Kbuild                   # Kernel build object definitions
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

## License

Licensed under the **GNU General Public License v3.0** ([`LICENCE.md`](LICENCE.md)).
