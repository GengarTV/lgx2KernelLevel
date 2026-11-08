# Changelog

All notable changes to the **AVerMedia Live Gamer Extreme (GC550) Linux Kernel Module (`gc550.ko`)** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [0.1.3] - 2026-11-06

### Added
- Expose discrete YUYV capture sizes for `1920x1080`, `1280x720`, `720x576`, `720x480`, and `640x480`.
- Validate frame spans and bound buffer copies using the selected V4L2 `sizeimage`; retain the existing ±16 KiB span tolerance and neutral-black tail fill.

### Verified
- Built with Clang 23 and `W=1` against CachyOS kernel `7.3.1-1-cachyos`; checkpatch and `git diff --check` passed.
- Tested on Niri with the GC550 and a Raspberry Pi 3B. V4L2 captures returned the expected exact frame spans for 1080p60 (120 frames), 720p60 (120), 576p50 (100), 480p59.94 (100), and 640x480p60 (100); all tests reported zero desyncs.
- Tested the modes in OBS after manually selecting the matching source resolution. OBS does not automatically follow HDMI resolution changes. The 576p50 input synchronized while OBS was configured for 60 fps; accurate V4L2 frame-interval reporting remains follow-up work.

## [0.1.2] - 2026-11-04

### Fixed
- Reject USB command replies whose actual length differs from the expected length before copying data to callers; reject invalid I2C data pointers.
- Saturate the video frame-span counter so a missing EAV cannot wrap malformed-stream accounting into an accepted frame size. The existing ±16 KiB span tolerance and streaming parameters are unchanged.
- Fill the uncopied tail of an accepted short V4L2 frame with neutral YUYV black before returning its buffer, preventing bytes from a previously used buffer from appearing in the frame.

### Changed
- Released the stability-hardening branch as DKMS/module version `0.1.2` and pacman package `0.1.2-1`.

### Documented
- Record Niri's tested xHCI callback-ordering evidence and its HCD-specific limits, plus follow-up measurements and fault-injection checks.

### Verified
- Built `0.1.2` with `W=1` against CachyOS kernel `7.3.1-1-cachyos`. The Niri-tested `0.1.1SH` DKMS package built for both `7.3.1-1-cachyos` and `6.18.55-2-cachyos-lts`.
- Tested the stability code on Niri with mpv and OBS. During active OBS video/audio capture, USB disconnect stopped after `17,227` frames with `0` desyncs and no kernel faults; reconnect re-enumerated at SuperSpeed, completed cold bootstrap, and restored 1080p capture. OBS video and GC550 audio capture were confirmed active after reconnect.
- Measured `55,655` SAV→EAV candidates during sustained capture before and after a physical reconnect on Niri (CachyOS `7.3.1-1`): `55,654` fell in the `4,147,200..4,147,455` byte bin; one post-reconnect candidate was below the `4,130,816` byte acceptance limit and was rejected. Both sessions ended with `0` desyncs. No accepted short span occurred, so the neutral-tail fill was not exercised; the ±16 KiB window remains unchanged.

## [0.1.1] - 2026-11-04

### Fixed
- **Safe USB hot-unplug with open V4L2 and ALSA handles**:
  - Added deferred `gc550_dev` lifetime references for V4L2 and ALSA device lifetimes, retaining the USB device and interface until final release.
  - Serialized initial URB submissions against stop/disconnect and gated callback resubmissions without sleeping in completion context.
  - Made stream resource and VB2 buffer teardown idempotent; disconnect now waits for URB callbacks and in-flight ALSA audio callbacks before freeing their resources.
- Bumped the DKMS, installer, driver, and pacman package version to `0.1.1`.

### Verified
- Built with `W=1` against CachyOS kernel `7.3.1-1-cachyos`.
- Tested on niri with OBS capturing video and audio during USB unplug: `29,629` frames, `0` desyncs, and no kernel faults. Replug registered the device and completed cold bootstrap.

## [0.1.0] - 2026-11-02

### Added
- **USB 3.0 Core Driver (`kernel/gc550-core.c`)**:
  - Added USB device table matching AVerMedia Live Gamer Extreme GC550 hardware IDs **`07ca:4710`** and **`07ca:0550`**.
  - Added USB SuperSpeed (5 Gbps) link check on probe.
- **Native V4L2 Video Capture (`kernel/gc550-video.c`)**:
  - Added `videobuf2-vmalloc` V4L2 capture node (`/dev/videoX` and `/dev/avermedia-gc550` symlink) supporting uncompressed `1920x1080 @ 60fps` (`V4L2_PIX_FMT_YUYV`, Rec. 709 limited range).
  - Implemented BT.1120 SAV (`00 FF FF C0`) and EAV (`00 FF FF C1`) checksum-validated stream parser on USB Bulk Endpoint `0x83 IN` with `8 × 512 KiB` URB ring buffers.
  - Added cross-URB marker prefix straddle reassembly (`marker_stage` 0..3).
  - Added automatic 16 KiB FX3 ping-pong DMA swap detector and reorderer (`swap_mode`).
- **Native ALSA Audio Capture (`kernel/gc550-audio.c`)**:
  - Added ALSA PCM capture card (`GC550Audio`, `48 kHz`, `16-bit Little-Endian Stereo`) demultiplexing `00 FF FF 58` audio packets from `EP 0x83 IN`.
- **FX3 USB-to-I2C Command Bridge & Safe Hardware Bootstrap (`kernel/gc550-cmd.c`, `kernel/gc550-regtables.h`)**:
  - Added 6,576-step hardware initialization table (derived from [@ChrisAJS's `lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace)) programming the `ADV7604` HDMI receiver + 1080p60 EDID RAM, `ADV7511` HDMI passthrough transmitter, `TLV320AIC3x` audio codec, `PCA9539A` GPIO expander, and `Lattice ECP3` FPGA.
  - Enforced hard runtime filter (`-EPERM`) blocking `AT88SC CryptoMemory` opcodes (`0x05..0x07`, `0x09`) to prevent hardware lockouts.
  - Added warm board-state detection (`ADV7604 IO[0xFA]` and `PCA9539A CFG`) so already-bootstrapped cards skip cold re-initialization.
  - Added automatic input color-space configuration (`ADV7604 IO[0x02]`) and neutral picture adjustment reset (`CP 0x3A..0x3D`).
  - Added HDMI cable, TMDS lock, resolution, and HDCP status logging (`gc550_log_signal_status`) on stream start.
- **Packaging, Build System & Documentation**:
  - Added `Makefile` and `dkms.conf` with automatic GCC vs. Clang/LLVM (`LLVM=1`) kernel detection (supporting simultaneous DKMS builds across Clang mainline and GCC LTS kernels).
  - Added Arch Linux / CachyOS `PKGBUILD` (`avermedia-gc550-dkms`).
  - Added distro-aware `install.sh` (`--arch`, `--dkms`, `--direct`, `--uninstall`) and `99-avermedia-gc550.rules`.
  - Added `TROUBLESHOOTING.md`, `CONTRIBUTING.md`, `TODO.md`, and `CHANGELOG.md`.
- **Hardware & Endurance Verification**:
  - Runtime-verified on the Niri test laptop running bare-metal CachyOS Linux (`7.3.1-1-cachyos` Clang) over USB 3.0 SuperSpeed (`5000M`); DKMS build also verified against `6.18.55-2-cachyos-lts` (GCC).
  - Verified 3.6+ hours continuous `1080p60` streaming (`776,295` frames captured with `0` desyncs), HDMI hot-plugging, and `48 kHz` stereo ALSA audio capture on Niri with **Nintendo Switch** and **Raspberry Pi 3B** HDMI sources.
