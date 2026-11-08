# Troubleshooting Guide (`gc550.ko`)

This guide covers common installation, build, hardware detection, and streaming issues with the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module.

---

## 1. Build & Installation Issues

### Missing Kernel Headers (`/lib/modules/$(uname -r)/build` not found)
**Symptom:**
```text
[ERR ] Kernel headers for 7.x.x not found at /lib/modules/7.x.x/build.
```
**Causes & Fixes:**
1. **Headers package is not installed:**
   - **Arch Linux:** `sudo pacman -S linux-headers`
   - **CachyOS:** `sudo pacman -S linux-cachyos-headers` (match your running kernel variant, e.g., `linux-cachyos-bore-headers` or `linux-cachyos-lts-headers`)
   - **Debian / Ubuntu:** `sudo apt install linux-headers-$(uname -r)`
   - **Fedora:** `sudo dnf install kernel-devel-$(uname -r)`
2. **System updated the kernel without rebooting:**
   - On rolling-release distros (Arch/CachyOS), if you ran `pacman -Syu` before installing headers, the installed headers on disk are for the *new* kernel while `uname -r` is still the *old* running kernel. **Reboot your machine** so `uname -r` matches your installed headers, then re-run `./install.sh`.

---

### Compiler Mismatch (`unrecognized command-line option '-mllvm'` or `-fsplit-lto-unit`)
**Symptom:**
GCC fails with unrecognized Clang flags when building against custom kernels like CachyOS or Zen.
**Fix:**
The included `Makefile` and `dkms.conf` automatically inspect `CONFIG_CC_IS_CLANG=y` in `/lib/modules/$(uname -r)/build/.config` and set `LLVM=1`. Make sure you have `clang`, `lld`, and `llvm` installed:
```bash
sudo pacman -S clang lld llvm
```

---

### `modprobe: ERROR: could not insert 'gc550': Operation not permitted` (Secure Boot)
**Symptom:**
The module compiles cleanly, but `modprobe gc550` or `insmod gc550.ko` fails with `Operation not permitted` or `Key was rejected by service`.
**Cause:**
UEFI **Secure Boot** is enabled, which blocks unsigned out-of-tree kernel modules.
**Fix:**
- Either disable Secure Boot in your UEFI/BIOS settings, **or**
- Set up a Machine Owner Key (MOK) with DKMS (`/etc/dkms/framework.conf`) so DKMS automatically signs `gc550.ko` during build.

---

## 2. USB & Device Detection Issues

### Device Plugged In, But No `/dev/videoX` Appears
**Checklist:**
1. **Check if Linux sees the USB device at all:**
   ```bash
   lsusb | grep -i 07ca
   ```
   You should see `ID 07ca:4710 AVerMedia Technologies, Inc. GC550` (or `07ca:0550`). If nothing appears, check the USB 3.0 cable and try another port directly on the motherboard.
2. **Check if the USB port negotiated SuperSpeed (5000M):**
   ```bash
   lsusb -t
   ```
   Look for `Driver=gc550, 5000M`. If it says `480M`, the card is plugged into a USB 2.0 port or using a USB 2.0 cable. The GC550 requires **USB 3.0 SuperSpeed (5 Gbps)** to stream uncompressed 1080p60 video (~250 MB/s).
3. **Check `dmesg` for probe errors:**
   ```bash
   sudo dmesg | grep -i gc550
   ```

---

## 3. Video & Color Space Issues

### Black Screen or "No Signal" in OBS Studio
**Checklist:**
1. **Check the hardware HDMI signal status in `dmesg`:**
   When OBS opens the device, `gc550.ko` queries the internal `ADV7604` HDMI receiver and logs a line in `dmesg`:
   ```bash
   sudo dmesg | grep "HDMI Status"
   ```
   - Expected good output:
     ```text
     HDMI Status: cable=YES, signal=LOCKED (1920x1080p, HDMI, HDCP=NO, TMDS=148MHz)
     ```
   - If **`cable=NO`**: Check the HDMI IN cable from your console/PC/camera.
   - If **`HDCP=YES`**: The HDMI source is encrypting the video with **HDCP** (High-bandwidth Digital Content Protection). Neither the official Windows driver nor this driver captures HDCP-protected streams:
     - **PS4 / PS5:** Go to *Settings → System → HDMI → Enable HDCP* and toggle it **OFF**.
     - **MacBooks / Apple TV:** macOS frequently forces HDCP on HDMI outputs.
   - If **`signal=NONE`**: Check that the source is active, the HDMI cable is connected to **HDMI IN**, and HDR is disabled.
   - If the signal is locked but OBS is black, set the OBS V4L2 source's **Resolution/FPS Type** to **Custom** and choose the size matching the input. Supported sizes are `1920x1080`, `1280x720`, `720x576`, `720x480`, and `640x480`; the driver does not switch the V4L2 format automatically when the source timing changes.

---

### Washed-Out Colors, Green Tint, or Lifted Blacks
- **HDR Enabled on Source:** The GC550 uses an HDMI 1.4a 8-bit SDR receiver (`ADV7604`). It does **not** support HDR10 or BT.2020. Ensure HDR is turned **OFF** on your console or PC.
- **RGB vs. YCbCr Mismatch:** On stream start, `gc550.ko` automatically sets `ADV7604 IO[0x02]` to auto-detect input color space from the HDMI AVI InfoFrame and resets CP contrast/brightness (`0x3A..0x3D`) to neutral. In OBS Studio, leave the V4L2 source's **Color Space** set to `Default` or `709` and **Color Range** set to `Partial` (Limited).

---

### Scrambled / Split / Offset Image After Restarting OBS
**Cause:**
The Cypress FX3 controller inside the GC550 uses two 16 KiB DMA buffers in a ping-pong arrangement. Occasionally, stopping and restarting a stream without power-cycling USB can flip the 16 KiB buffer pair order.
**Fix:**
`gc550.ko` includes an automatic **16 KiB FX3 DMA swap detector**. Within 2 frames (~33 ms), it detects the early EAV header mismatch and logs:
```text
FX3 16 KiB DMA buffer order switched to SWAPPED
```
and automatically un-swaps the stream. If the hardware ever gets wedged into an unrecoverable state, unplugging the USB 3.0 cable for 2 seconds and plugging it back in will cold-reset the FX3 and ADV7604.

---

## 4. Audio Issues in OBS / PipeWire

### No Audio or Cannot Find GC550 Audio in OBS
1. **Verify the ALSA capture card is registered:**
   ```bash
   arecord -l
   ```
   You should see:
   ```text
   card X: GC550Audio [AVerMedia GC550], device 0: GC550 HDMI Audio [AVerMedia GC550 Capture]
   ```
2. **Important Note on Multiplexed Audio:**
   On the GC550 hardware, HDMI audio packets (`00 FF FF 58`) are multiplexed inside the **same USB 3.0 bulk stream (`EP 0x83 IN`) as the video frames**.
   - This means **video capture (`/dev/videoX`) must be active** for the USB stream URBs to run and feed audio samples into the ALSA capture device!
3. **OBS Setup:**
   Add an **Audio Input Capture (ALSA)** or **Audio Input Capture (PipeWire / PulseAudio)** source in OBS and select **AVerMedia GC550 (`GC550 HDMI Audio`)**.

### Mixed Audio or Feedback in OBS
The GC550 may be available through both ALSA and PipeWire / PulseAudio. In OBS, capture it through one path only: use a dedicated GC550 audio source, or select it as the global **Mic/Aux** device, but not both. The device may appear under a PipeWire name such as `GC550 Pro`.

If you only want HDMI capture audio, disable **Desktop Audio** and any laptop microphone input. Desktop Audio captures the computer's playback monitor, which can include audio OBS is monitoring and feed it back into the recording. Set the GC550 source's **Audio Monitoring** to **Monitor Off** unless monitoring is intentional.

### No Audio When Previewing with `mpv`
Because Linux separates V4L2 video (`/dev/avermedia-gc550`) and ALSA audio (`hw:GC550Audio,0`), running `mpv av://v4l2:/dev/avermedia-gc550` only opens the video stream. Furthermore, passing `--audio-file=av://alsa:hw:GC550Audio,0` directly to `mpv` can stall audio playback because V4L2 timestamps use `CLOCK_MONOTONIC` (system uptime) while FFmpeg's `alsa` demuxer uses `CLOCK_REALTIME` (Unix epoch).

**Fix (Low-Latency `mpv` + PipeWire Audio Preview; measured at ~50 ms on Niri):**
Run `mpv` with zero read-ahead probing (or press `Right Arrow` in `mpv` to flush startup frames to the live edge) and bridge the ALSA capture device directly into PipeWire:
```bash
mpv av://v4l2:/dev/avermedia-gc550 --profile=low-latency --demuxer-lavf-analyzeduration=0 --demuxer-readahead-secs=0 &
arecord -D hw:GC550Audio,0 -f S16_LE -c 2 -r 48000 -B 40000 -F 10000 | pw-play --rate=48000 --channels=2 --format=s16 --latency=20ms -
```
