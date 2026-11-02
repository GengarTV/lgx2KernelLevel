# Project Roadmap & Technical TODO (`gc550.ko`)

This file tracks planned features, technical design notes, and future experiments for the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module.

---

## Current Milestone: Field Testing `v0.1.0` (1080p60 Baseline)
- [x] Verify probe, cold bootstrap, V4L2 video (`1080p60 YUYV`), and ALSA audio (`48 kHz S16_LE`) in QEMU/KVM CachyOS VM (`07ca:4710`).
- [ ] Field-test bare-metal performance, frame pacing, and audio sync on a secondary physical Linux machine (laptop) over USB 3.0 Micro-B SuperSpeed.
- [ ] Field-test on primary production gaming rig.

---

## Planned Feature: Multi-Resolution / Low-Resolution Support (`720p`, `576p`, `480p`, `640x480`)

Upstream [`ChrisAJS/lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace) lists `Low resolution(?) support [ ]` in its TODO. Below is the technical investigation and implementation plan for adding lower-resolution support to `gc550.ko` once `v0.1.0` bare-metal field testing is complete.

### 1. Why Lower Resolutions Currently Drop in the Parser
1. **HDMI Receiver (`ADV7604`) Already Locks to Lower Resolutions:**
   - The EDID programmed into `ADV7604` (`0x36`) already advertises `1080p60/50`, `720p60/50`, `576p`, `480p`, and `640x480`.
   - When an HDMI source outputs `720p` or `480p`, the `ADV7604` locks onto the signal automatically and reports the active dimensions in `HDMI 0x07..0x0A` (already decoded by `gc550_log_signal_status()` in [`kernel/gc550-cmd.c`](kernel/gc550-cmd.c)).
2. **Hardcoded Frame Size Gate in `gc550-video.c`:**
   - Currently, `gc550_complete_video_frame()` validates that the byte count (`p->video_pos`) between `SAV` (`00 FF FF C0`) and `EAV` (`00 FF FF C1`) is within `±16 KiB` of `GC550_FRAME_SIZE` (`1920 × 1080 × 2 = 4,147,200` bytes).
   - A `1280 × 720` frame is `1,843,200` bytes (`720 × 480` is `691,200` bytes), so the 1080p size gate rejects every lower-resolution frame as a "startup partial".
3. **EAV Header Line Count Clue (`PROTOCOL.md`):**
   - In `PROTOCOL.md`, the 5-byte `EAV` header (`c0 c1 c2 c3 c4`) carries `c2..c3 = 0x04 0x38` (`1080` in decimal) on 1080p streams. At 720p, `c2..c3` should report `0x02 0xD0` (`720`).

### 2. Implementation Steps (Post Field-Testing)
- **Step 1 — Diagnostic Span & Header Logging:**
  - Log `p->video_pos`, `SAV` (`b0..b3`), and `EAV` (`c0..c4`) via `dev_dbg_ratelimited()` when a non-1080p `SAV → EAV` span completes while connected to a `720p` or `480p` HDMI source.
  - Verify whether the Lattice ECP3 FPGA (`I2C 0x10`) passes `720p`/`480p` BT.1120 active lines straight through or requires register updates (`0x01`, `0x02`, `0x05..0x07`).
- **Step 2 — Expose Discrete Frame Sizes in V4L2 (`kernel/gc550-video.c`):**
  - Update `gc550_vidioc_enum_framesizes()`, `gc550_vidioc_try_fmt_vid_cap()`, and `gc550_vidioc_s_fmt_vid_cap()` to support:
    - `1920 × 1080` (`4,147,200` bytes)
    - `1280 × 720` (`1,843,200` bytes)
    - `720 × 576` (`829,440` bytes)
    - `720 × 480` (`691,200` bytes)
    - `640 × 480` (`614,400` bytes)
- **Step 3 — Dynamic Parser Frame Size Gate:**
  - Compare `p->video_pos` against `dev->pix_fmt.sizeimage` instead of the constant `GC550_FRAME_SIZE`.
  - Optionally support `VIDIOC_QUERY_DV_TIMINGS` / `VIDIOC_G_DV_TIMINGS` by reading `ADV7604` `HDMI 0x07..0x0A` so V4L2 applications can auto-detect source resolution changes.

---

## Other Future Enhancements
- [ ] **Periodic Background Status Polling (`struct delayed_work`)**:
  - Poll `ADV7604` status registers every ~200–500 ms when idle/streaming to detect HDMI hot-plug, resolution changes, or HDCP state changes dynamically.
- [ ] **Synthesized "No Signal" / "HDCP Protected" Frame Generator**:
  - When `ADV7604` reports no HDMI cable or HDCP encryption, emit a solid Rec. 709 black/status frame at 60 Hz into `vb2` buffers so V4L2 consumers (like OBS) don't stall waiting for `DQBUF`.
