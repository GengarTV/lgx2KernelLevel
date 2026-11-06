# Contributing & Maintainer Guide (`gc550.ko`)

Thank you for your interest in contributing to or maintaining the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module!

---

## 1. Critical Hardware Safety Rule (Read First!)

> [!CAUTION]
> **NEVER send or replay Atmel AT88SC CryptoMemory authentication commands (`Bridge Opcodes 0x05, 0x06, 0x07, 0x09` with `0xB8` / `0xBA`) to a GC550.**
>
> The GC550 contains an Atmel AT88SC CryptoMemory anti-clone chip. Replaying static authentication challenges from USB packet captures will fail verification and decrement the chip's non-volatile attempt counters (`FF -> EE -> CC -> 88 -> 00`). Once a counter reaches `00`, that key set is **permanently locked**, causing the official Windows AVerMedia driver to reject the card.
>
> - The GC550 hardware **does not require** CryptoMemory authentication to stream video or audio.
> - `gc550_cmd_send()` in [`kernel/gc550-cmd.c`](kernel/gc550-cmd.c) enforces a hard runtime filter (`-EPERM`) that blocks opcodes `0x05..0x07` and `0x09`. **Do not remove or bypass this filter.**

---

## 2. Driver Architecture Overview

The module (`gc550.ko`) is split into four C compilation units and two headers in [`kernel/`](kernel/):

| File | Responsibility |
|---|---|
| [`kernel/gc550.h`](kernel/gc550.h) | USB IDs (`07ca:4710`, `07ca:0550`), endpoint addresses (`0x01`, `0x81`, `0x83`), I2C slave addresses (`ADV7604`, `ADV7511`, `PCA9539A`, `TLV320AIC3x`, `ECP3 FPGA`), and `struct gc550_dev`. |
| [`kernel/gc550-core.c`](kernel/gc550-core.c) | `struct usb_driver` lifecycle (`gc550_probe`, `gc550_disconnect`), USB 3.0 speed verification, and V4L2/ALSA registration teardown. |
| [`kernel/gc550-cmd.c`](kernel/gc550-cmd.c) | Cypress FX3 Bulk `0x01 OUT` / `0x81 IN` command & I2C bridge, warm board-state detection (`gc550_detect_board_state`), cold bootstrap replay (`gc550_run_init_sequence`), automatic input color-space config (`gc550_set_auto_input_colorspace`), and HDMI signal status logging (`gc550_log_signal_status`). |
| [`kernel/gc550-regtables.h`](kernel/gc550-regtables.h) | Packed binary array (`gc550_init_blob`) of the 6,576 Crypto-stripped USB-to-I2C initialization steps. |
| [`kernel/gc550-video.c`](kernel/gc550-video.c) | V4L2 `video_device` & `videobuf2-vmalloc` queue, asynchronous USB Bulk `0x83 IN` URB pool (`8 × 512 KiB`), cross-URB marker straddle parser (`gc550_parse_ordered_bytes`), and 16 KiB FX3 DMA swap reorderer (`gc550_parse_stream_chunk`). |
| [`kernel/gc550-audio.c`](kernel/gc550-audio.c) | ALSA `snd_card` & `snd_pcm` capture subdriver (`48 kHz`, `16-bit LE Stereo`). Receives demultiplexed `00 FF FF 58` PCM payloads from `gc550-video.c`. |

For deep-dive protocol details (BT.1120 SAV/EAV markers, checksums, and I2C register maps), read [`PROTOCOL.md`](PROTOCOL.md).

### Stream callback ordering on the tested Niri kernel

On Niri's CachyOS `7.3.1-1` kernel, the xHCI host controller advertises
`HCD_BH`; the USB core gives back these bulk URBs through its single
low-priority work item and drains that giveback queue serially. A short
trace during capture observed 1,399 stream completion callbacks, all on CPU
1. This supports the current parser's single-callback assumption for that
kernel/controller combination. It is not a guarantee made by the generic USB
API for every HCD. Recheck the completion path before changing HCDs or
supporting a controller that can invoke stream completions concurrently; do
not infer system-wide safety from this one machine's trace.

---

## 3. Safe Kernel Development & VM Testing Workflow

Because a bug in a kernel USB/V4L2 driver can cause a kernel panic or soft lockup, **always test changes inside a QEMU/KVM virtual machine with USB 3.0 passthrough first**:

1. **Create a Linux VM in `virt-manager` / QEMU:**
   - Configure the VM's USB controller as **USB 3.0 (`qemu-xhci`)**.
   - Add a **USB Host Device** passthrough for `07ca:4710` (or `07ca:0550`).
   - Enable a serial console (`ttyS0`) so kernel oops stack traces are captured even if the VM display freezes.
2. **Build & Test Locally:**
   ```bash
   make clean && make
   modinfo gc550.ko
   ```
3. **Load & Verify in the VM:**
   ```bash
   sudo modprobe videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm
   sudo rmmod gc550 2>/dev/null || true
   sudo insmod ./gc550.ko
   v4l2-ctl --all -d /dev/video0
   v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=60
   sudo dmesg | grep -i gc550
   ```

---

## 4. How `kernel/gc550-regtables.h` Is Formatted

Each transaction in `gc550_init_blob[]` is packed as:
- `Byte 0`: `tx_len` (`u8`, 1..36 bytes to send on Bulk OUT `0x01`)
- `Bytes 1..2`: `rx_len` (`u16` little-endian, `0` if write-only, `> 0` if a reply of `rx_len` bytes must be read from Bulk IN `0x81`)
- `Bytes 3..(3 + tx_len - 1)`: Raw command payload bytes (`0x01` I2C write, `0x02` I2C read, `0x08` trigger, `0x0E` EEPROM read).

If you ever update or trim the initialization sequence:
1. Verify that only opcodes `0x01`, `0x02`, `0x08`, `0x0E`, or `0x14` are present.
2. Update `GC550_INIT_NUM_STEPS` and `GC550_INIT_BLOB_SIZE` in `kernel/gc550-regtables.h`.

---

## 5. Coding Style & Pull Requests

- Follow the standard **Linux Kernel Coding Style** (8-character tabs, `linux/checkpatch.pl` conventions).
- Ensure `make` succeeds without warnings on both **GCC** and **Clang (`LLVM=1`)**.
- When bumping the driver version, update:
  - `GC550_DRIVER_VERSION` in `kernel/gc550.h`
  - `PACKAGE_VERSION` in `dkms.conf`
  - `pkgver` in `PKGBUILD`
  - `PKG_VER` in `install.sh`
  - Package source path and supported-format details in `README.md`
  - [`CHANGELOG.md`](CHANGELOG.md)
