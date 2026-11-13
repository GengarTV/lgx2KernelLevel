# E1.9 Stage 2B – Passive Parser Telemetry Implementation Report

**Project:** `lgx2KernelLevel`  
**Target Hardware:** AVerMedia Live Gamer Extreme GC550 (`07ca:4710`)  
**Specification:** `STAGE2B_ARCHITECTURE_SPECIFICATION_REV3.md`  
**Implementation Date:** 2026-11-12  
**Maintainer:** Øystein Lie  
**Status:** **STAGE 2B IMPLEMENTED – BOTH BUILDS PASSED – READY FOR HARDWARE VALIDATION**

---

## 1. Executive Summary

E1.9 Stage 2B has been implemented in strict adherence to `STAGE2B_ARCHITECTURE_SPECIFICATION_REV3.md`. The implementation introduces passive, read-only instrumentation across the BT.1120 video parser, marker validation, FX3 16 KiB DMA ping-pong swap heuristic, frame-terminal buffer lifecycle, and desynchronization paths without changing any driver capture algorithms, state transitions, scheduling, or buffer management.

Both the diagnostic-enabled (`CONFIG_GC550_DIAG=1`) and diagnostic-disabled (`CONFIG_GC550_DIAG=0`) build configurations were successfully verified using Clang/LLVM against host kernel `7.3.3-1-cachyos` with zero compiler warnings or errors. Pre-existing Stage 2A module binaries in the project directory were preserved untouched by performing isolated builds in dedicated build directories.

---

## 2. Modified Source Files and Scope

| File | Status in Git | Role in Stage 2B | Lines Changed |
|---|---|---|---|
| `kernel/gc550-diag.h` | Untracked (new) | Ring buffer capacity (262,144 records), 80-byte ABI, parser flag bitfields, dispatch context definition, Stage 2B emitter declarations and inline no-op stubs. | +318 lines |
| `kernel/gc550-diag.c` | Untracked (new) | Stage 2B emitter implementations, stack-local record population, debugfs `diag_records` `seq_file` formatting updates. | +1099 lines |
| `kernel/gc550.h` | Tracked (modified) | Span limits (`GC550_SWAP_SPAN_LIMIT`, `GC550_FRAME_SPAN_TOLERANCE`), dispatch context prototype updates for `gc550_parse_stream_chunk`. | +16 lines |
| `kernel/gc550-video.c` | Tracked (modified) | Synchronous dispatch context propagation, SAV/EAV/Audio marker hooks, swap candidate telemetry, D1/D2/D3 desync hooks, and P1–P17 frame-terminal telemetry. | +128 lines |
| `Kbuild` | Tracked (modified) | Unaltered Stage 2A conditional build logic (`CONFIG_GC550_DIAG` gating). | 0 lines changed in this task |
| `kernel/gc550-core.c` | Tracked (modified) | Unaltered Stage 2A diagnostic ring initialization, debugfs registration, and teardown. | 0 lines changed in this task |

### Git Working Tree Status
```
 M Kbuild
 M kernel/gc550-core.c
 M kernel/gc550-video.c
 M kernel/gc550.h
?? kernel/gc550-diag.c
?? kernel/gc550-diag.h
```
*HEAD commit:* `cfa2473087c114f12d25c8320ebef797d9545a99`  
*Integrity:* Zero commits, resets, stashes, checkouts, or discards performed.

---

## 3. New Event Types and Telemetry Emitters

Stage 2B introduces eight new diagnostic telemetry emitter functions in `kernel/gc550-diag.c`:

1. `gc550_diag_emit_marker_sav`: Logs SAV marker detection, sequence byte `sav_b0`, validation result, opening checksum byte `h[3]`, and expected checksum.
2. `gc550_diag_emit_marker_eav`: Logs EAV marker detection, sequence byte `eav_c0`, sequence match/mismatch with `sav_b0`, and checksum comparison.
3. `gc550_diag_emit_audio_header`: Logs BT.1120 audio packet headers and decoded payload remaining length (`p->audio_rem`).
4. `gc550_diag_emit_swap_vote`: Logs evaluation of candidate 16 KiB FX3 swap condition (`video_pos <= 32 KiB && c0 != b0`) and incremental vote counter.
5. `gc550_diag_emit_swap_toggle`: Logs active transition between normal and swapped FX3 buffer order upon two consecutive votes (`swap_votes >= 2`).
6. `gc550_diag_emit_desync`: Independently instruments each of the three parser desynchronization branches (D1, D2, D3), recording the trigger reason, marker type, sequence byte, and current `desync_count`.
7. `gc550_diag_emit_frame_complete`: Emits terminal buffer outcomes (`VB2_BUF_STATE_DONE` or `VB2_BUF_STATE_ERROR`) across all six terminal outcome categories (`GC550_TERM_*`).
8. `gc550_diag_emit_parser_reset`: Emits stream lifecycle and exception resets (P1 `STREAMON` and P2 USB transfer gap).

When `CONFIG_GC550_DIAG=0`, all eight emitters evaluate to static inline no-ops in `kernel/gc550-diag.h`, introducing zero runtime overhead.

---

## 4. Dispatch-Context Propagation

To eliminate shared mutable state, `struct gc550_dispatch_ctx` is allocated strictly on the execution stack of `gc550_urb_complete()`:

```c
struct gc550_dispatch_ctx {
	u64 submission_generation;
	u64 completion_seq;
	u16 slot_index;
	u16 cpu_id;
	u8  active_callbacks;
};
```

### Propagation Path
```
gc550_urb_complete()
  │  struct gc550_dispatch_ctx dctx = { ... }; (stack allocated)
  ▼
gc550_parse_stream_chunk(dev, data, len, &dctx)
  │
  ├──► !swap_mode:
  │        gc550_parse_ordered_bytes(dev, data, len, dctx, false)
  │
  └──► swap_mode == 1:
           Accumulate into dev->parser.pair_buf (32 KiB)
           │ (On 32 KiB boundary)
           ▼
           gc550_parse_ordered_bytes(dev, pair_buf + 16384, 16384, dctx, true)
           gc550_parse_ordered_bytes(dev, pair_buf,         16384, dctx, true)
             │
             ├──► gc550_reserve_video_frame(dev, dctx, is_pair_buf)
             ├──► gc550_complete_video_frame(dev, eav_c0, dctx, is_pair_buf)
             └──► gc550_abort_video_frame(dev, dctx, reason, is_pair_buf)
```

- **Lifetime:** Exists exclusively during synchronous callback execution; never assigned to `struct gc550_dev` or `struct gc550_stream_parser`.
- **Out-of-Callback Sentinels:** Where events occur outside URB callbacks (e.g. `STREAMON` reset or `STREAMOFF` teardown), `dctx` is passed as `NULL`. Telemetry emitters record documented sentinels: `submission_generation = 0`, `completion_seq = 0`, `slot_index = 0xFF`, `active_callbacks = 0`.

---

## 5. Byte-Provenance Classifications and Limitations

Stage 2B maintains a strict distinction between **storage dispatch origin** and **physical byte provenance**:

- **Storage Origin (Parser Input Path):**
  - `GC550_PFLAG_PATH_DIRECT_URB (BIT(14))`: Data passed into `gc550_parse_ordered_bytes()` originates directly from `urb->transfer_buffer`.
  - `GC550_PFLAG_PATH_PAIR_BUF (BIT(13))`: Data passed into `gc550_parse_ordered_bytes()` originates from `dev->parser.pair_buf`.
- **Physical Byte Provenance:**
  - Because the existing driver parses markers across chunk boundaries via `p->hdr_buf` without tracking per-byte source coordinates, Stage 2B strictly enforces `urb_rel_offset = U32_MAX (0xFFFFFFFF)` and `GC550_PFLAG_PROV_UNKNOWN (BIT(15))` for all parser-level events.
  - No synthetic or speculative stream coordinates are fabricated.

---

## 6. 80-Byte Record Layout and Pahole ABI Verification

The diagnostic record layout was verified using `pahole` on the compiled kernel object `/tmp/gc550_stage2b_diag1/gc550.ko`:

```
struct gc550_diag_record {
	u64                        timestamp_ns;            /*     0     8 */
	u64                        endpoint_payload_offset; /*     8     8 */
	u64                        secondary_payload_offset;/*    16     8 */
	u64                        submission_generation;   /*    24     8 */
	u64                        completion_seq;          /*    32     8 */
	u32                        urb_rel_offset;          /*    40     4 */
	u32                        distance_from_prev_sav;  /*    44     4 */
	u32                        segment_id;              /*    48     4 */
	u32                        payload_len;             /*    52     4 */
	u16                        pair_pos;                /*    56     2 */
	u16                        cpu_id;                  /*    58     2 */
	s16                        urb_status;              /*    60     2 */
	u8                         event_type;              /*    62     1 */
	u8                         slot_index;              /*    63     1 */
	/* --- cacheline 1 boundary (64 bytes) --- */
	u8                         marker_type;             /*    64     1 */
	u8                         marker_seq;              /*    65     1 */
	u8                         parser_state;            /*    66     1 */
	u8                         swap_mode;               /*    67     1 */
	u8                         swap_votes;              /*    68     1 */
	u8                         active_callbacks;        /*    69     1 */
	u16                        flags;                   /*    70     2 */
	u16                        parser_flags;            /*    72     2 */
	u8                         reserved[6];             /*    74     6 */

	/* size: 80, cachelines: 2, members: 23 */
	/* last cacheline: 16 bytes */
} __attribute__((__aligned__(8)));
```

### Static Compile-Time Layout Assertions
The following assertions are compiled into `kernel/gc550-diag.h`:
```c
static_assert(sizeof(struct gc550_diag_record) == 80,
	      "struct gc550_diag_record ABI size must be exactly 80 bytes");
static_assert(offsetof(struct gc550_diag_record, parser_flags) == 0x48,
	      "parser_flags must begin at byte offset 0x48");
static_assert(offsetof(struct gc550_diag_record, reserved) == 0x4A,
	      "reserved must begin at byte offset 0x4A");
```

---

## 7. Parser Control-Flow and Frame-Terminal Coverage (P1–P17)

All 17 paths defined in Section 10 of `STAGE2B_ARCHITECTURE_SPECIFICATION_REV3.md` are fully instrumented:

| Path | Source Function | Trigger Condition | Diagnostic Event | Outcome / Meaning |
|---|---|---|---|---|
| **P1** | `gc550_start_streaming` | STREAMON stream start | `GC550_EVT_PARSER_RESET` | Reason = `GC550_RESET_STREAMON` |
| **P2** | `gc550_urb_complete` | USB gap / transfer error (`urb->status != 0`) | `GC550_EVT_PARSER_RESET` | Reason = `GC550_RESET_USB_TRANSFER_ERROR` |
| **P3** | `gc550_reserve_video_frame` | Incoming SAV while `dev->cur_buf != NULL` | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_ERROR`, `GC550_TERM_UNCLOSED_SAV_ABORT (4)` |
| **P4** | `gc550_reserve_video_frame` | Valid SAV header accepted | `GC550_EVT_SAV_DETECTED` | `GC550_PFLAG_SAV_CHECKSUM_VALID` |
| **P5** | `gc550_parse_ordered_bytes`| Invalid SAV checksum | `GC550_EVT_PARSER_DESYNC` | **Desync Branch D1** (`GC550_DESYNC_SAV_CHECKSUM`) |
| **P6** | `gc550_abort_video_frame` | SAV checksum failure abort | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_ERROR`, `GC550_TERM_PARSER_ABORT_ERROR (5)` |
| **P7** | `gc550_parse_ordered_bytes`| Valid EAV checksum | `GC550_EVT_EAV_DETECTED` | `GC550_PFLAG_EAV_CHECKSUM_VALID` |
| **P8** | `gc550_parse_ordered_bytes`| EAV/SAV sequence mismatch (`c0 != b0`) | `GC550_EVT_PARSER_DESYNC` | **Desync Branch D2** (`GC550_DESYNC_SEQ_MISMATCH`) |
| **P9** | `gc550_parse_ordered_bytes`| Invalid EAV checksum | `GC550_EVT_PARSER_DESYNC` | **Desync Branch D3** (`GC550_DESYNC_EAV_CHECKSUM`) |
| **P10**| `gc550_abort_video_frame` | EAV checksum failure abort | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_ERROR`, `GC550_TERM_PARSER_ABORT_ERROR (5)` |
| **P11**| `gc550_parse_ordered_bytes`| Audio header marker (`00 FF FF 58`) | `GC550_EVT_AUDIO_HEADER` | Payload length `p->audio_rem` |
| **P12**| `gc550_complete_video_frame`| `span <= 32K && c0 != b0` | `GC550_EVT_SWAP_VOTE` | Increment vote candidate |
| **P13**| `gc550_complete_video_frame`| `swap_votes >= 2` | `GC550_EVT_SWAP_TOGGLE` | Toggle `p->swap_mode` |
| **P14**| `gc550_complete_video_frame`| Swap candidate abort | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_ERROR`, `GC550_TERM_SWAP_CANDIDATE_ERROR (3)` |
| **P15**| `gc550_complete_video_frame`| Span outside tolerance | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_ERROR`, `GC550_TERM_INVALID_SPAN_ERROR (2)` |
| **P16**| `gc550_complete_video_frame`| Valid span accepted | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_DONE`, `GC550_TERM_NORMAL_DONE (1)` |
| **P17**| `gc550_return_all_buffers` | STREAMOFF / disconnect active buf teardown | `GC550_EVT_FRAME_COMPLETE` | `VB2_BUF_STATE_ERROR`, `GC550_TERM_STREAMOFF_TEARDOWN (6)` |

### Unclosed-Frame Abort Handling (Path P3)
In `gc550_reserve_video_frame()`:
```c
spin_lock_irqsave(&dev->queue_lock, flags);
if (dev->cur_buf) {
	abort_buf = dev->cur_buf;
	dev->cur_buf = NULL;
}
spin_unlock_irqrestore(&dev->queue_lock, flags);

if (abort_buf) {
#ifdef CONFIG_GC550_DIAG
	gc550_diag_emit_frame_complete(dev, dctx, abort_buf,
				       VB2_BUF_STATE_ERROR,
				       GC550_TERM_UNCLOSED_SAV_ABORT,
				       is_pair_buf);
#endif
	gc550_finish_buffer(dev, abort_buf, VB2_BUF_STATE_ERROR);
}

p->current_frame_sequence = p->frame_sequence++;
p->sav_b0 = p->hdr_buf[0];
...
```
The unclosed frame's sequence number and span are captured and logged prior to opening the new frame, preventing any loss of frame telemetry.

---

## 8. Desync Branch Instrumentation Mapping

The three source-verified `p->desync_count++` locations in `kernel/gc550-video.c` are independently instrumented, preserving full distinction from USB transfer gaps:

- **Branch D1 (Line 384, Path P5):** SAV checksum failure. Emits `GC550_EVT_PARSER_DESYNC` with `GC550_DESYNC_SAV_CHECKSUM (1)`.
- **Branch D2 (Line 402, Path P8):** EAV/SAV sequence mismatch (`eav_c0 != sav_b0`). Emits `GC550_EVT_PARSER_DESYNC` with `GC550_DESYNC_SEQ_MISMATCH (2)`. (Frame completes normally afterwards).
- **Branch D3 (Line 416, Path P9):** EAV checksum failure. Emits `GC550_EVT_PARSER_DESYNC` with `GC550_DESYNC_EAV_CHECKSUM (3)`.
- **Branch D4 (Line 683, Path P2):** USB transfer error gap (`urb->status != 0`). Calls `gc550_abort_video_frame()` and `gc550_parser_reset()`, emitting `GC550_EVT_PARSER_RESET` with `GC550_RESET_USB_TRANSFER_ERROR (2)`. **Does not increment `desync_count`**, maintaining 100% fidelity to the source code.

---

## 9. Ring-Buffer Capacity and Memory Footprint

- **Record ABI Size:** 80 bytes (fixed)
- **Ring Capacity:** 262,144 records ($2^{18}$)
- **Total Kernel Memory:** $262,144 \times 80 = 20,971,520\text{ bytes} = \mathbf{20.00\text{ MiB}}$ (allocated via `vzalloc` at probe)
- **Retention at 1080p60 (Worst-Case ~777 events/sec):** 337.3 seconds (5.62 minutes), fully retaining a standard 5-minute capture session without ring overwrite.

---

## 10. Build Verification Results

### Build 1: Diagnostics Enabled (`CONFIG_GC550_DIAG=1`)
- **Directory:** `/tmp/gc550_stage2b_diag1`
- **Command:** `make -C /tmp/gc550_stage2b_diag1 CONFIG_GC550_DIAG=1 LLVM=1 -j$(nproc)`
- **Result:** **PASS (Exit code 0)**
- **Compiler Warnings:** 0
- **Generated Module:** `/tmp/gc550_stage2b_diag1/gc550.ko` (Size: 2,001,760 bytes)
- **Exported Symbols:** Confirmed all 8 Stage 2B diagnostic functions present via `llvm-nm`.

### Build 2: Diagnostics Disabled (`CONFIG_GC550_DIAG=0`)
- **Directory:** `/tmp/gc550_stage2b_diag0`
- **Command:** `make -C /tmp/gc550_stage2b_diag0 CONFIG_GC550_DIAG=0 LLVM=1 -j$(nproc)`
- **Result:** **PASS (Exit code 0)**
- **Compiler Warnings:** 0
- **Generated Module:** `/tmp/gc550_stage2b_diag0/gc550.ko` (Size: 1,939,816 bytes)
- **Symbol Cleanliness:** Confirmed **zero** diagnostic symbols in the compiled module (`llvm-nm | grep gc550_diag` returned empty).

---

## 11. Post-Implementation Source Audit

1. **No parser algorithm changes:** All byte scanning, marker state tracking, and copy operations remain verbatim.
2. **No altered `swap_mode` or `swap_votes` behavior:** Evaluated strictly under identical original conditions.
3. **No change to parser state transition conditions:** Marker recognition, parsing states, and validation logic are unchanged.
4. **No change to USB or URB behavior:** Bulk IN endpoint scheduling, URB count (8), and size (512 KiB) are unchanged.
5. **No change to video/audio payload handling:** YUYV unpacking, frame sizing, and audio sample delivery are unchanged.
6. **No parser-wide locks:** No locks added around byte parsing or memcpy.
7. **No shared mutable callback context:** All context is stack-local and passed via `const`.
8. **No fabricated stream coordinates:** `urb_rel_offset = U32_MAX` for all parser events.
9. **Independent desync telemetry:** D1, D2, and D3 log independent reason codes.
10. **USB gaps separated from desync count:** D4 logs parser reset without touching `desync_count`.
11. **Single terminal event per buffer:** Every buffer transition emits exactly one `GC550_EVT_FRAME_COMPLETE` event.
12. **Unclosed-frame abort instrumented before reset:** Path P3 logs old frame metadata before resetting frame state.
13. **80-byte record layout preserved:** Validated with `pahole` and compile-time assertions.
14. **Existing Stage 2A diagnostics preserved:** `gc550_diag_emit_urb_complete`, submission failure, and lifecycle hooks remain fully functional.
15. **Zero overhead on disabled builds:** Proved with clean build and empty symbol table under `CONFIG_GC550_DIAG=0`.

---

## 12. Deviations from Specification

**None.** The implementation adheres strictly to `STAGE2B_ARCHITECTURE_SPECIFICATION_REV3.md`.

---

## 13. Operational Boundaries and Constraints Confirmation

- **Hardware Testing:** **NOT PERFORMED.** The GC550 module was not loaded or reloaded. No V4L2 capture, `mpv` execution, or physical testing was initiated.
- **Repository Safety:** The working tree in `/home/gengartv/lgx2KernelLevel` was kept clean of build artifacts. The Stage 2A module artifact remains intact.
- **Git State:** No commits, checkouts, or branch mutations were executed.

---

## Completion Status

**STAGE 2B IMPLEMENTED – BOTH BUILDS PASSED – READY FOR HARDWARE VALIDATION**
