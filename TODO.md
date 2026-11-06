# Project Roadmap & Technical TODO (`gc550.ko`)

This file tracks planned features, technical design notes, and future experiments for the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module.

---

## `v0.1.2` Stability Hardening (1080p60 Baseline)
- [x] Verify probe, cold bootstrap, V4L2 video (`1080p60 YUYV`), and ALSA audio (`48 kHz S16_LE`) in QEMU/KVM CachyOS VM (`07ca:4710`).
- [x] Field-test bare-metal performance, frame pacing (`60.02 fps`, `0` desyncs), and audio sync on a secondary physical Linux machine (laptop) (When I can find my god damn extra USB 3.0 A to Micro-B SuperSpeed cable! I REALLY don't want to untangle my cable management from my primary production rig — *Update: Cable found! Verified on `niri` laptop with Raspberry Pi 3B & Nintendo Switch: `776,295` continuous 1080p60 frames with `0` desyncs + `48 kHz` stereo ALSA audio!*).
- [x] **Fix USB Hot-Unplug Use-After-Free While Streaming (`struct kref` + `v4l2_dev.release`)**:
  - **Issue Discovered on `niri` (`2026-11-03`)**: Hot-unplugging the USB 3.0 cable while `mpv` had `/dev/video2` actively open and streaming triggered a `NULL pointer dereference` (`RIP: v4l2_ioctl+0x24/0x60`) in `mpv`'s `demux` thread (`PID 36588`), leaving `/dev/video2` zombie-locked and registering `/dev/video3` on replug.
  - **Root Cause**: [`gc550_disconnect()`](kernel/gc550-core.c) currently calls `kfree(dev)` immediately after `gc550_video_unregister(dev)` while userspace file descriptors (`/dev/videoX`, `/dev/snd/pcmC*`) are still open and mid-`ioctl`.
  - **Implemented and verified in `v0.1.1` on niri:** Device kref release is deferred through V4L2 and ALSA lifetimes; disconnect closes the URB submission gate, drains URBs and in-flight ALSA callbacks, and makes VB2 stop cleanup idempotent.
  - **Unplug test:** OBS was capturing video and audio. Disconnect completed after `29,629` frames with `0` desyncs and no kernel faults; replug registered the device and completed a cold bootstrap.
- [ ] Field-test on primary production gaming rig.

## Stability Hardening Follow-up
- [x] Fill the uncopied tail of accepted short V4L2 frames with neutral YUYV black to prevent stale buffer contents (implemented in `v0.1.2`; no naturally accepted short span was observed, so this path was not exercised).
- [x] Collect a real-GC550 SAV→EAV span histogram across sustained 1080p60 capture and a physical reconnect on Niri (CachyOS `7.3.1-1`). Before reconnect, all 28,189 candidates fell in the `4,147,200..4,147,455` byte bin. After reconnect, 27,465 of 27,466 candidates fell in that bin; one was below the `4,130,816` byte acceptance limit and was rejected. Both sessions ended with zero desyncs. No accepted short span was observed, so the neutral-tail fill was not exercised. Keep the protocol-derived ±16 KiB acceptance window.
- [ ] In a disposable VM or USB test setup, exercise command replies with exact, short, oversized, and absent payloads and verify each fails or succeeds with the expected error. Do not use the daily-use laptop for fault injection.
- [ ] Recheck parser callback serialization on each newly supported HCD/kernel combination. Niri's CachyOS 7.3.1-1 xHCI trace is documented in [`CONTRIBUTING.md`](CONTRIBUTING.md), but generic USB completion ordering is not guaranteed across all HCDs.

---

## Planned Feature: Dynamic ALSA Audio Sample Rate (`44.1 kHz` / `48 kHz` / `32 kHz`)

Currently, [`kernel/gc550-audio.c`](kernel/gc550-audio.c) hardcodes `SNDRV_PCM_RATE_48000`. However, HDMI sources (such as CD players, PS1/PS2/PS3 consoles playing Red Book CD audio, retro scalers, or PCs configured for `44.1 kHz`) can output `44.1 kHz`, `48 kHz`, or `32 kHz` LPCM audio over HDMI.

### 1. Hardware Evidence (`ADV7604` RX + `ADV7511` TX)
1. **EDID Audio Capabilities:**
   - The `ADV7604` EDID advertises 2-channel LPCM at `32 kHz`, `44.1 kHz`, and `48 kHz`.
2. **Incoming Audio Rate Detection (`ADV7604` HDMI Map `0x34`):**
   - The `ADV7604` extracts the HDMI Audio Clock Regeneration (`N` / `CTS` in `HDMI 0x5B..0x5F`) and IEC 60958 Channel Status bits (`HDMI 0x36..0x3A`, where `0x39` bits `3:0` encode the exact sample frequency: `0x0 = 44.1 kHz`, `0x2 = 48 kHz`, `0x3 = 32 kHz`, `0x8 = 88.2 kHz`, `0xA = 96 kHz`).
   - Additionally, the USB stream's per-frame audio payload length (`[00 FF FF 58] [BE32 len]`) directly reflects the sample rate relative to frame rate (e.g., at `60 fps`: `3,200 bytes` = `48 kHz`, `2,940 bytes` = `44.1 kHz`).
3. **HDMI Passthrough Transmitter (`ADV7511` `0x3D`):**
   - As noted in [`PROTOCOL.md`](PROTOCOL.md), the bootstrap programs the `ADV7511` Audio Clock Regeneration `N` register (`0x01..0x03`) with `0x001880` (`6272`, the HDMI standard `N` for `44.1 kHz`) and then `0x001800` (`6144`, the HDMI standard `N` for `48 kHz`).
   - When the input sample rate changes, `ADV7511` registers `0x01..0x03` must also be updated so the HDMI passthrough monitor doesn't lose audio sync.

### 2. Implementation Steps (Post Field-Testing)
- **Step 1 — Read Active HDMI Audio Rate in `gc550-cmd.c`:**
  - Read `ADV7604` `HDMI 0x39` (IEC 60958 Channel Status sample rate bits) and `HDMI 0x5B..0x5D` (`N` value) during `gc550_log_signal_status()` (and periodic status polling), storing `dev->audio_rate` (`32000`, `44100`, or `48000`, defaulting to `48000` when unlocked).
- **Step 2 — Update `ADV7511` Passthrough `N` Register on Rate Change:**
  - When `dev->audio_rate` changes between `44.1 kHz`, `48 kHz`, and `32 kHz`, write the matching 20-bit `N` value (`6272` for `44.1 kHz`, `6144` for `48 kHz`, `4096` for `32 kHz`) to `ADV7511` (`0x3D`) registers `0x01..0x03`.
- **Step 3 — Dynamic ALSA Rate Constraints in `gc550-audio.c`:**
  - Expand `gc550_pcm_hw.rates` to `SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000` (`rate_min = 32000`, `rate_max = 48000`).
  - In `gc550_pcm_open()`, constrain `runtime->hw.rate_min = runtime->hw.rate_max = dev->audio_rate` (and `runtime->hw.rates = snd_pcm_rate_to_rate_bit(dev->audio_rate)`) so PipeWire / PulseAudio / OBS automatically negotiates the exact sample rate of the connected HDMI source.
  - If the HDMI source changes audio sample rate while an ALSA stream is already running, call `snd_pcm_stop_xrun(substream)` so PipeWire/OBS cleanly re-opens and re-negotiates the new sample rate instead of playing pitch-shifted audio (the "chipmunk" or "slow-mo" effect).

---

## `v0.1.3` Multi-Resolution Capture

- [x] Expose YUYV sizes `1920x1080`, `1280x720`, `720x576`, `720x480`, and `640x480` in V4L2.
- [x] Use the selected V4L2 `sizeimage` for parser acceptance, copy bounds, short-tail fill, and buffer payload size. Preserve the protocol-derived ±16 KiB span window.
- [x] Test on Niri with a Raspberry Pi 3B. Isolated V4L2 captures delivered exact expected spans: 1080p60 `4,147,200` bytes (120 frames), 720p60 `1,843,200` (120), 576p50 `829,440` (100), 480p59.94 `691,200` (100), and 640x480p60 `614,400` (100). All reported zero parser desyncs.
- [x] Verify OBS capture at each lower size after manually matching the source format to the input resolution.
- [ ] Report the input's actual frame interval through V4L2. The current interval enumeration reports 60 fps for every size; a 576p50 source was observed at about 50 fps while OBS was configured for 60 fps.
- [ ] Detect HDMI timing changes and notify V4L2 consumers so applications that support source-change events can update their format automatically. OBS did not change its configured capture size when the Pi's HDMI mode changed; the source had to be updated manually.

---

## Other Future Enhancements
- [ ] **URB Chunk Size Latency Tuning (`GC550_URB_BUF_SIZE`: `512 KiB` → `128 KiB`)**:
  - A 1080p frame period (`4,161,536` bytes = `254 × 16 KiB`) is not an exact multiple of `512 KiB` (`32 × 16 KiB`), so the `512 KiB` URB containing a frame's `EAV` marker can wait up to `~1.5–2.5 ms` into the next frame before completing. Reducing `GC550_URB_BUF_SIZE` to `128 KiB` (with `16` or `32` URBs) can shave `~2 ms` off `vb2_buffer_done()` delivery to catch the current 60 Hz display VSync (`~33 ms` / 2-frame glass-to-glass latency).
- [ ] **Periodic Background Status Polling (`struct delayed_work`)**:
  - Poll `ADV7604` status registers every ~200–500 ms when idle/streaming to detect HDMI hot-plug, resolution changes, or HDCP state changes dynamically.
- [ ] **Synthesized "No Signal" / "HDCP Protected" Frame Generator**:
  - When `ADV7604` reports no HDMI cable or HDCP encryption, emit a solid Rec. 709 black/status frame at 60 Hz into `vb2` buffers so V4L2 consumers (like OBS) don't stall waiting for `DQBUF`.

---

## Long-Term Goal: Distribution & Mainline Linux Kernel (`linux-media`) Upstreaming

### Phase 1: Arch User Repository (AUR) Release
- [ ] **Publish `avermedia-gc550-dkms` to the AUR**:
  - Adapt the existing [`PKGBUILD`](PKGBUILD) to fetch tagged releases (or `-git` HEAD) from `https://github.com/GengarTV/lgx2KernelLevel` so Arch Linux, CachyOS, EndeavourOS, and Manjaro users can install the driver directly via `paru -S avermedia-gc550-dkms` or `yay -S avermedia-gc550-dkms`.

### Phase 2: Mainline Linux Kernel Submission (`drivers/media/usb/gc550/`)
Target subsystem: **Linux Media Subsystem (`linux-media@vger.kernel.org`)**, maintained by Hans Verkuil (V4L2) and Mauro Carvalho Chehab.

- [ ] **1. License Coordination (`GPL-3.0` → `GPL-2.0-or-later`)**:
  - The mainline Linux kernel requires `GPL-2.0` compatibility. Coordinate with upstream author **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** to dual-license/re-license the reverse-engineered GC550 hardware initialization tables under `GPL-2.0-or-later` and invite him to co-sign (`Signed-off-by` / `Co-developed-by`) the upstream patch series.
- [ ] **2. Pass `v4l2-compliance -s` with Zero Failures**:
  - Run `v4l2-compliance -s -d /dev/avermedia-gc550` (from `v4l-utils`) after implementing the `struct kref` / `v4l2_dev.release` disconnect fix and resolve any flagged V4L2 ioctl edge cases.
- [ ] **3. Trim Redundant Polling Loops from `gc550-regtables.h`**:
  - As documented in [`PROTOCOL.md`](PROTOCOL.md), only the first ~1,730 steps of the 6,576-command capture are actual chip initialization; the remaining ~4,800 steps are repeated status-poll reads from the Windows USB capture. Trimming the redundant polling reads will reduce [`kernel/gc550-regtables.h`](kernel/gc550-regtables.h) by ~75% and cut cold bootstrap time from `~3.3 s` to **`< 0.9 s`**.
- [ ] **4. Pass `scripts/checkpatch.pl --strict`**:
  - Verify zero style warnings against the target `media_stage` kernel tree before submitting the `[PATCH 0/2] media: usb: gc550: Add AVerMedia Live Gamer Extreme (GC550) driver` series to `linux-media@vger.kernel.org`.
