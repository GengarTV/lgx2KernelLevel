# Project Roadmap & Technical TODO (`gc550.ko`)

This file tracks planned features, technical design notes, and future experiments for the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module.

---

## `v0.1.2` Stability Hardening (1080p60 Baseline)
- [x] Verify probe, cold bootstrap, V4L2 video (`1080p60 YUYV`), and ALSA audio (`48 kHz S16_LE`) in QEMU/KVM CachyOS VM (`07ca:4710`).
- [x] Field-test bare-metal performance, frame pacing (`60.02 fps`, `0` desyncs), and audio sync on a secondary physical Linux machine (laptop) (When I can find my god damn extra USB 3.0 A to Micro-B SuperSpeed cable! I REALLY don't want to untangle my cable management from my primary production rig — *Update: Cable found! Verified on `niri` laptop with Raspberry Pi 3B & Nintendo Switch: `776,295` continuous 1080p60 frames with `0` desyncs + `48 kHz` stereo ALSA audio!*).
- [x] **Fix USB Hot-Unplug Use-After-Free While Streaming (`struct kref` + `v4l2_dev.release`)**:
  - **Issue Discovered on `niri` (`2026-11-03`)**: Hot-unplugging the USB 3.0 cable while `mpv` had `/dev/video2` actively open and streaming triggered a `NULL pointer dereference` (`RIP: v4l2_ioctl+0x24/0x60`) in `mpv`'s `demux` thread (`PID 36588`), leaving `/dev/video2` zombie-locked and registering `/dev/video3` on replug.
  - **Historical Root Cause (`v0.1.0`)**: In `v0.1.0`, [`gc550_disconnect()`](kernel/gc550-core.c) called `kfree(dev)` immediately after `gc550_video_unregister(dev)` while userspace file descriptors (`/dev/videoX`, `/dev/snd/pcmC*`) were still open and mid-`ioctl`.
  - **Implemented and verified in `v0.1.1` on niri:** Device kref release is deferred through V4L2 and ALSA lifetimes; disconnect closes the URB submission gate, drains URBs and in-flight ALSA callbacks, and makes VB2 stop cleanup idempotent.
  - **Unplug test:** OBS was capturing video and audio. Disconnect completed after `29,629` frames with `0` desyncs and no kernel faults; replug registered the device and completed a cold bootstrap.
- [x] Field-test on primary production gaming rig (`Titan`):
  - Verified on Titan (CachyOS `7.3.3-1`, AMD Ryzen 9 5900X, Radeon RX 9070 XT) via DKMS package `avermedia-gc550-dkms` (`0.1.3`).
  - Sustained continuous 1080p60 capture in OBS Studio (`177,241` continuous frames, ~49.2 minutes) with `0` driver desyncs (13 frames dropped downstream purely due to OBS video encoder load).
  - ALSA audio capture verified with continuous `44.1 kHz` FLAC audio (*"In the Hall of the Mountain King"*) streamed over HDMI from a Raspberry Pi 3B into the GC550 `48 kHz` stereo capture pipeline with zero glitches or sync drift.

## Stability Hardening Follow-up
- [x] Fill the uncopied tail of accepted short V4L2 frames with neutral YUYV black to prevent stale buffer contents (implemented in `v0.1.2`; no naturally accepted short span was observed, so this path was not exercised).
- [x] Collect a real-GC550 SAV→EAV span histogram across sustained 1080p60 capture and a physical reconnect on Niri (CachyOS `7.3.1-1`). Before reconnect, all 28,189 candidates fell in the `4,147,200..4,147,455` byte bin. After reconnect, 27,465 of 27,466 candidates fell in that bin; one was below the `4,130,816` byte acceptance limit and was rejected. Both sessions ended with zero desyncs. No accepted short span was observed, so the neutral-tail fill was not exercised. Keep the protocol-derived ±16 KiB acceptance window.
- [ ] In a disposable VM or USB test setup, exercise command replies with exact, short, oversized, and absent payloads and verify each fails or succeeds with the expected error. Do not use the daily-use laptop for fault injection.
- [ ] Recheck parser callback serialization on each newly supported HCD/kernel combination. Niri's CachyOS 7.3.1-1 xHCI trace is documented in [`CONTRIBUTING.md`](CONTRIBUTING.md), but generic USB completion ordering is not guaranteed across all HCDs.
- [ ] Long-duration endurance testing under active V4L2 streaming.
- [ ] Compliance and capture testing across all five supported resolutions (720p, 576p, 480p, 640x480 beyond 1080p).
- [ ] Hot unplug/replug verification under active streaming.
- [ ] Simultaneous video streaming and ALSA audio capture stability under load.
- [ ] DMABUF streaming verification with `--expbuf-device`.

---

## Audio Transport & Sample-Rate Investigation (`44.1 kHz` / `48 kHz` / `32 kHz`) — Complete

Hardware investigation (Phases B2, C1–C3, D1, D2.1, and D2.2.7) concluded. Confirmed that [`kernel/gc550-audio.c`](kernel/gc550-audio.c) correctly implements a fixed `48 kHz` stereo `S16_LE` (`SNDRV_PCM_RATE_48000`) ALSA capture interface. The GC550 hardware delivers a continuous `48 kHz` audio stream over USB bulk EP 0x83 regardless of whether the incoming HDMI audio is `48 kHz` or `44.1 kHz`. Dynamic ALSA sample-rate switching is therefore unnecessary and unsupported by the hardware capture stream. See [`docs/GC550-REVERSE-ENGINEERING.md`](docs/GC550-REVERSE-ENGINEERING.md) (Sections F.4, F.5, F.6, and G) for complete documentation.

### 1. Verified Hardware Observations (Phases B2, C1, C2, and C3 on Titan; Analyzed in Phase D1)
- [x] **ADV7604 Channel-Status Verification (`IO[0x65]` & `HDMI[0x39]`)**:
  - Verified on Titan (`1080p60` from Raspberry Pi 3B): `ADV7604 IO[0x65]` bit 7 (`CS_DATA_VALID_RAW`) and `HDMI[0x39]` bits `3:0` (`CS_DATA` sampling frequency) reported invalid/idle during no-audio capture (`B2`: `IO[0x65]=0x40`, `HDMI[0x39]=0x00`), valid `48,000 Hz` in `C1` (`IO[0x65]=0x82`, `HDMI[0x39]=0x02`), and valid `44,100 Hz` in `C3` when the Pi hardware ran at `44,100 Hz` with explicit `AES3=0x00` channel-status metadata (`IO[0x65]=0x82`, `HDMI[0x39]=0x00`). (`C2` ran at `44,100 Hz` with incorrect default `AES3=0x02` metadata and is not a valid `44.1 kHz` receiver verification.)
- [x] **Fixed USB Audio Payload Byte Rate (`~3,200` B/frame / `~192,000` B/s at `1080p60`)**:
  - Measured `parser.audio_bytes` during Phases B2 and C1–C3 (with the GC550 ALSA PCM device closed; `parser.audio_bytes` counts only the payload bytes declared by the 4-byte big-endian length header after `00 FF FF 58`, excluding the marker, length field, and trailing `AA 55 55 AA` filler):
    - `B2` (no active HDMI audio, `300` frames): `959,984` bytes
    - `C1` (`48 kHz`, valid channel status, `3,600` frames): `11,523,052` bytes
    - `C2` (`44.1 kHz` source, incorrect channel-status metadata, `3,600` frames): `11,523,044` bytes
    - `C3` (`44.1 kHz`, valid channel status, `3,600` frames): `11,523,036` bytes
  - Both `C1` (`48 kHz`) and `C3` (`44.1 kHz`) produced practically identical USB audio payload totals (`16` bytes difference over `3,600` frames), and `B2` demonstrated continued `~3,200` B/frame payload generation even without valid HDMI audio.

### 2. Verified Hardware Capture & Signal Analysis (Phases D2.1 & D2.2.7 on Titan)
- [x] **Fixed 48 kHz Stereo S16_LE Capture Under Both HDMI Input Conditions**:
  - Controlled 30-second ALSA captures (`arecord -D hw:3,0 -f S16_LE -c 2 -r 48000`) produced exactly `5,760,000` bytes each (`1,440,000` audio frames) with exit code 0 and zero parser desyncs:
    - **D2 Condition 1 (`48 kHz` HDMI input, `AES3=0x02`)**: Captured to `/tmp/gc550_d2_48k_raw_retry.pcm` (`5,760,000` bytes).
    - **D2 Condition 2 (`44.1 kHz` HDMI input, `AES3=0x00`)**: Captured to `/tmp/gc550_d2_44k1_raw.pcm` (`5,760,000` bytes).
- [x] **ADV7604 Input Rate Reporting Preserved & Verified**:
  - `gc550_log_signal_status()` correctly read and reported the incoming HDMI rate via `IO[0x65]` and `HDMI[0x39]` (`48000 Hz` in D2 Condition 1, `44100 Hz` in D2 Condition 2) while the ALSA capture interface operated at fixed `48,000 Hz`.
- [x] **Audio Transport Continuity & Duplicate Characterization**:
  - **Continuity**: No audio discontinuities or transport repetitions were detected in the analyzed 30-second captures (`0` cycle length anomalies `> 0.5 samples` across all `30,000` sine cycles in both conditions).
  - **Duplicates**: Duplicate samples occurred exclusively at peak apex turning points (`|s| >= 22,900 LSB`), matching the integer quantization of the scaled continuous sine model (`180/180` matches in D2 Condition 1, `165/165` matches in D2 Condition 2). Zero slope repetitions and zero zero-crossing duplicates were observed.
- [x] **Observed Upstream Rate Conversion**:
  - The GC550 capture pipeline delivers a continuous `48 kHz` stereo `S16_LE` stream when fed a `44.1 kHz` HDMI source. The driver performs no host-side resampling and passes demultiplexed payload bytes directly to ALSA ring buffers. Rate conversion upstream of the host ALSA interface is empirically supported by the preserved recordings, while the exact internal silicon component and conversion location (ADV7604 internal processing, an on-board DSP, or Cypress FX3 firmware) remain unknown.
- [x] **Separation of Observations from Unproven Hypotheses**:
  - In D2 Condition 1, continuous sine-model residual is `5.460 LSB RMS` with residual spectral peaks at `996.00`, `998.40`, and `1003.20 Hz` (`~2.95 LSB`, `-77.82 dBc`). In D2 Condition 2, continuous residual is `0.511 LSB RMS` with residual peaks suppressed to `0.08 LSB` (`-109.18 dBc`).
  - Hypotheses attributing D2 Condition 1 residual spectral peaks to ADV7604 HDMI ACR PLL tracking jitter or D2 Condition 2 residual suppression to rate-converter FIFO decoupling are **unproven**.
- [x] **Driver Architecture Decision**:
  - Because the hardware capture stream is consistently `48 kHz` stereo `S16_LE` across both `48 kHz` and `44.1 kHz` HDMI inputs, dynamic ALSA rate switching logic is unnecessary and unsupported by the hardware stream. Fixed `48 kHz` ALSA capture in [`kernel/gc550-audio.c`](kernel/gc550-audio.c) is the permanent hardware abstraction.


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
- [x] **2. Pass `v4l2-compliance -s` with Zero Failures**:
  - Verified on Titan (`7.3.3-1-cachyos`, Clang/LLVM `W=1`, `srcversion: 27D8CAC60C62239C91B8BFC`):
    - Non-streaming: `48/48` succeeded (`0` failures).
    - Streaming (`-s 1`): `57/57` succeeded (`0` failures, `0` warnings).
    - Passed: `read/write`, `MMAP (no poll / select / epoll, REQBUFS & CREATE_BUFS)`, `USERPTR (no poll & select)`.
    - DMABUF was not tested because no `--expbuf-device` was specified.
    - Centralized buffer completion helper (`gc550_finish_buffer`) ensures monotonic timestamps (`ktime_get_ns()`) and frame sequence numbering across both DONE and ERROR states.
    - Buffer reservation locked to SAV boundary (`video_pos == 0`); mid-frame buffer attachment eliminated.
    - Validated `vb2_plane_vaddr` check in `gc550_buf_prepare()`.
- [ ] **3. Trim Redundant Polling Loops from `gc550-regtables.h`**:
  - As documented in [`PROTOCOL.md`](PROTOCOL.md), only the first ~1,730 steps of the 6,576-command capture are actual chip initialization; the remaining ~4,800 steps are repeated status-poll reads from the Windows USB capture. Trimming the redundant polling reads will reduce [`kernel/gc550-regtables.h`](kernel/gc550-regtables.h) by ~75% and cut cold bootstrap time from `~3.3 s` to **`< 0.9 s`**.
- [ ] **4. Pass `scripts/checkpatch.pl --strict`**:
  - Verify zero style warnings against the target `media_stage` kernel tree before submitting the `[PATCH 0/2] media: usb: gc550: Add AVerMedia Live Gamer Extreme (GC550) driver` series to `linux-media@vger.kernel.org`.
