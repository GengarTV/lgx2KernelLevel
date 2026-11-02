# AVerMedia Live Gamer Extreme (GC550) Linux Kernel Module (`gc550.ko`)

> **Fork Notice & Credits:** This repository is a Linux kernel-level fork of [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) by **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors. Building directly upon their USB 3.0 protocol reverse-engineering, [`PROTOCOL.md`](PROTOCOL.md) specification, and hardware bootstrap research, this project implements a **native Linux kernel driver (`V4L2` + `ALSA`)** for the **AVerMedia Live Gamer Extreme (GC550, `USB ID 07ca:4710` / `07ca:0550`)**.

PLEASE also note that this kernel module is under HEAVY development (so heavy, in fact, I have not tried it outside of a VM yet, but I CAN confirm it works in QEMU running a CachyOS VM) and, like MANY projects on GitHub, comes with NO WARRANTY and NO SUPPORT!

Q: Why make a kernel module if a userspace driver exists?

A; Well, good question. The long answer is that I had Google Antigravity make me a kernel module; it based its work on Chris Sawczuk's work without my prior knowledge, resulting in two potentially equal things. Of course, I had Antigravity properly credit everyone involved in the project, but on further reflection, that wasn't enough, so I forked his repo and moved my kernel module to his fork, properly crediting his work.

Q: If it was a "mistake", why keep it at all?

A: I spent tokens on this, and logically there should be performance benefits of having the module in the kernel instead of userspace. Poor kernel modules CAN start race conditions, leading to an unstable kernel, more or less breaking your system, so as I said above **PROCEED AT YOUR OWN RISK**

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
├── PKGBUILD                 # Arch Linux / CachyOS pacman package build script (avermedia-gc550-dkms)
├── PROTOCOL.md              # USB 3.0 protocol & hardware specification (by @ChrisAJS)
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
