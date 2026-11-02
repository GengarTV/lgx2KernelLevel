# AVerMedia Live Gamer Extreme (GC550) Linux Kernel Module (`gc550.ko`)

> **Fork Notice & Credits:** This repository is a Linux kernel-level fork of [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) by **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors. Building directly upon their USB 3.0 protocol reverse-engineering, [`PROTOCOL.md`](PROTOCOL.md) specification, and hardware bootstrap research, this project implements a **native Linux kernel driver (`V4L2` + `ALSA`)** for the **AVerMedia Live Gamer Extreme (GC550, `USB ID 07ca:4710` / `07ca:0550`)**.

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
  - Automatically configures the ADV7604 input color space (`IO 0x02`) to match RGB or YCbCr HDMI sources and resets picture brightness/contrast adjustments to neutral.
  - **AT88SC CryptoMemory Protection**: Completely omits and blocks proprietary anti-clone authentication replays (`opcodes 0x05..0x07`), ensuring hardware attempt counters remain untouched (`0xFF`).
- **Stream Resilience**:
  - **Cross-URB Marker Straddle Reassembly**: Tracks partial `[00 FF FF]` SAV/EAV/Audio markers across USB transfer boundaries so frames never drop or misalign.
  - **16 KiB FX3 DMA Swap Reorderer**: Automatically detects and un-swaps 16 KiB ping-pong DMA buffers if the FX3 flips buffer phase on warm restarts.

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

### 2. Clone & Run the Installer
```bash
git clone https://github.com/GengarTV/lgx2KernelLevel.git
cd lgx2KernelLevel
./install.sh
```

What `./install.sh` does:
1. Registers and compiles `avermedia-gc550` (`0.1.0`) via **DKMS** (automatically detects whether your kernel was built with **GCC** or **Clang/LLVM**, and automatically rebuilds the module whenever your kernel updates).
2. Installs the udev rule ([`99-avermedia-gc550.rules`](99-avermedia-gc550.rules)) creating `/dev/avermedia-gc550` and granting desktop user access.
3. Loads `videodev`, `videobuf2`, `snd-pcm`, and `gc550.ko` immediately.

### Other Installer Commands
```bash
./install.sh --direct     # Compile & install for the current kernel only (without DKMS)
./install.sh --uninstall  # Unload and remove driver, DKMS package, and udev rules
```

### Verify Installation
Plug your AVerMedia GC550 into a **USB 3.0 (SuperSpeed)** port and check:
```bash
sudo dmesg | grep -i gc550
v4l2-ctl --list-devices
arecord -l
```
In **OBS Studio**:
- Add a **Video Capture Device (V4L2)** → select **AVerMedia Live Gamer Extreme (GC550)** (`/dev/video0` or `/dev/avermedia-gc550`).
- Add an **Audio Input Capture (ALSA / PipeWire)** → select **AVerMedia GC550**.

---

## Repository Structure

```text
├── 99-avermedia-gc550.rules # udev permissions & /dev/avermedia-gc550 symlink
├── Kbuild                   # Kernel build object definitions
├── Makefile                 # Out-of-tree kernel module Makefile (GCC & Clang/LLVM auto-detect)
├── PROTOCOL.md              # USB 3.0 protocol & hardware specification (by @ChrisAJS)
├── dkms.conf                # DKMS configuration
├── install.sh               # Automated DKMS / direct installer & uninstaller script
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
  - Reverse-engineered the USB-to-I2C bridge, the GC550/GC551 initialization sequences, the BT.1120 SAV/EAV framing checksums, the ADV7604 automatic color-space fix, the 16 KiB FX3 DMA swap behavior, and the AT88SC CryptoMemory anti-clone protection documented in [`PROTOCOL.md`](PROTOCOL.md).
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
