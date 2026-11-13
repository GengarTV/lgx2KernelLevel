# E1.9 Stage 2B – Final Technical Review (Read-Only Audit)

**Project:** `lgx2KernelLevel`  
**Hardware:** AVerMedia Live Gamer Extreme GC550 (USB ID `07ca:4710`)  
**Video Endpoint:** Bulk IN `0x83`  
**Baseline:** E1.9 Revision 4  
**Audit Target:** `STAGE2B_ARCHITECTURE_SPECIFICATION_REV2.md`  
**Maintainer:** Øystein Lie  
**Auditor:** Independent Technical Auditor  
**Date:** 2026-11-12  

---

## 1. Executive Verdict

### Verdict: **NOT READY FOR IMPLEMENTATION APPROVAL**

While Revision 2 made significant progress by removing the unproven mathematical claim regarding Test E's desyncs, preserving the observational boundaries of the `swap_mode` investigation, and properly estimating the 20.00 MiB ring buffer retention window, a rigorous source-level audit against `kernel/gc550-video.c`, `kernel/gc550-diag.h`, and `kernel/gc550-diag.c` has identified **four concrete architectural blockers** that prevent Stage 2B implementation from being approved:

1. **Missing Dispatch Context in Parser Call Chain:**  
   The specification mandates that every parser event record must populate `submission_generation`, `completion_seq`, and `slot_index`. However, source inspection reveals that `gc550_urb_complete()` invokes `gc550_parse_stream_chunk(dev, urb->transfer_buffer, urb->actual_length)` without passing `ctx`, `urb`, or callback sequence IDs. These parameters are stack-local in `gc550_urb_complete()` and are **completely inaccessible** inside `gc550_parse_ordered_bytes()` and `gc550_complete_video_frame()`.
2. **Indistinguishable Byte Provenance at Marker Call Sites:**  
   The proposed classification `GC550_PROV_DIRECT` vs `GC550_PROV_COMPOSITE_STRADDLE` cannot be assigned truthfully at lines 320 (`gc550_sav_hdr_valid`) or 333 (`gc550_eav_hdr_valid`) using existing parser state. The parser does not track function-entry values of `marker_stage` or `hdr_pos`, making it impossible to determine whether a marker prefix or header was carried over from a prior URB without introducing additional runtime tracking.
3. **Bitfield Exhaustion in the 16-bit `u16 flags` Record Field:**  
   In `kernel/gc550-diag.h`, existing transport flags occupy 14 of the 16 available bits (`BIT(0)` through `BIT(13)`). Revision 2 specifies more than 10 new diagnostic and provenance flags for parser events, which mathematically cannot fit into the remaining 2 bits (`BIT(14)` and `BIT(15)`) without either semantic overloading or extending into `reserved[]`.
4. **Missing Terminal Frame Outcome in `gc550_reserve_video_frame()`:**  
   Revision 2's control-flow checklist omitted the critical frame-abort path in `gc550_reserve_video_frame()` (lines 153–161), where an unclosed video frame is aborted with `VB2_BUF_STATE_ERROR` upon the arrival of a new SAV marker when a previous EAV was dropped.

As instructed by the maintainer, a technically justified **NOT READY** result is mandatory. Approving an architecture that cannot reliably provide the context or provenance it claims to diagnose would compromise the integrity of E1.9.

---

## 2. Marker and Byte Provenance Findings

This audit traced the complete execution path of Bulk IN video data from USB transfer completion to byte parsing:

```
[xHCI Host Controller]
         │
         ▼ (524,288-byte URB transfer buffer)
gc550_urb_complete(urb) ──► ctx, sub_id, entry_seq, slot_index on local stack
         │
         ▼ Line 546: gc550_parse_stream_chunk(dev, urb->transfer_buffer, urb->actual_length)
         │           [NOTE: urb, ctx, sub_id, entry_seq are NOT passed!]
         │
         ├──► IF !swap_mode:
         │        gc550_parse_ordered_bytes(dev, data, len)
         │        [Direct mode: data points into urb->transfer_buffer]
         │
         └──► IF swap_mode == 1:
                  memcpy(dev->parser.pair_buf + pair_pos, data, copy)
                  │ (accumulates across 32 KiB pair boundaries)
                  ▼
                  gc550_parse_ordered_bytes(dev, pair_buf + 16384, 16384)  [Half 1 first]
                  gc550_parse_ordered_bytes(dev, pair_buf,         16384)  [Half 0 second]
                  [Swapped mode: data points into heap pair_buf, NOT urb->transfer_buffer!]
```

### Detailed Answers to Audit Questions

#### 1. Can a marker prefix or complete marker header span two separate parser input chunks?
**YES.** *Verified from source (`gc550-video.c:317–328, 377–415`).*
- `dev->parser.marker_stage` tracks candidate prefix scanning across calls:
  - If chunk $K$ ends with byte `0x00`, `marker_stage` is set to 1.
  - If chunk $K+1$ begins with `0xFF 0xFF 0xC0`, lines 385–404 consume those bytes from chunk $K+1$ and transition to `GC550_STATE_SAV_HEADER`.
- In `GC550_STATE_SAV_HEADER`:
  - `dev->parser.hdr_pos` persists across calls. If chunk $K$ supplies 2 header bytes (`hdr_pos = 2`), chunk $K+1$ supplies the remaining 2 bytes (`hdr_pos = 4`), completing the SAV header across two separate chunks.

#### 2. Can a marker header span two separate URB completions?
**YES.** *Verified from source (`gc550-video.c:453–489, 538–548`).*
- In direct mode (`swap_mode == 0`), each URB completion invokes `gc550_parse_ordered_bytes()` directly with `urb->transfer_buffer`. A marker prefix or header starting within the final 8 bytes of URB $N$ completes in URB $N+1$.
- In swapped mode (`swap_mode == 1`), a 32 KiB pair may straddle the 512 KiB boundary between URB $N$ and URB $N+1$.

#### 3. Can a marker header be assembled from bytes originating in different URBs?
**YES.** *Verified from source.*
Bytes 0 and 1 of `hdr_buf` can originate from URB $N$, while bytes 2 and 3 originate from URB $N+1$.

#### 4. In swapped mode, can a marker header span different 16 KiB halves or 32 KiB pair-buffer boundaries?
**YES.** *Verified from source (`gc550-video.c:475–487`).*
- In swapped mode, `gc550_parse_ordered_bytes()` is called sequentially for Half 1 (`pair_buf + 16384`), then Half 0 (`pair_buf`).
- If Half 1 ends with `00 FF FF`, `marker_stage` is 3. The first byte of Half 0 (`pair_buf[0]`) provides the marker type byte `0xC0`.
- Furthermore, if Half 0 ends with a partial prefix or header, it completes in Half 1 of the *next* 32 KiB pair buffer.

#### 5. Does the parser retain partial marker/header state between calls?
**YES.** *Verified from source (`kernel/gc550.h:111–127`).*
`struct gc550_stream_parser` explicitly retains:
- `marker_stage` (prefix progress 0..3)
- `state` (`SAV_HEADER`, `EAV_HEADER`, `AUDIO_HEADER`, etc.)
- `hdr_pos` (header byte index 0..5)
- `hdr_buf[5]` (partially assembled header bytes)
- `pair_pos` (pair buffer accumulation index 0..32,767)

#### 6. Can Stage 2B accurately identify the actual origin of every byte in a marker without modifying parser behavior?
**NO.** *Verified from source.*
When line 320 (`gc550_sav_hdr_valid`) or line 333 (`gc550_eav_hdr_valid`) executes:
- The parser inspects `p->hdr_buf[0..3]` or `p->hdr_buf[0..4]`.
- The parser does **not** record which URBs or buffer offsets contributed each individual byte of `hdr_buf`.
- If a marker straddled a boundary, the physical origin of the individual bytes is lost. Stage 2B cannot reconstruct this without adding runtime provenance sidecars, which violates the read-only / no-parser-modification constraint.

#### 7. Can the proposed provenance classifications be assigned truthfully at actual call sites?
**NO, not as specified in Revision 2.**
- Revision 2 assumed `GC550_PROV_DIRECT` could be assigned whenever `swap_mode == 0`. This is false: direct-mode markers can straddle URB completions.
- Inside `gc550_parse_ordered_bytes()`, the code has no access to the calling URB context, making it impossible to record `submission_generation` or `completion_seq` without modifying function signatures.

---

## 3. Provenance Classification Validation

| Provenance Classification | Specification Definition (Rev 2) | Audit Feasibility Status | Source Justification & Technical Reality |
|---|---|---|---|
| **`GC550_PROV_DIRECT`** | Event occurred in direct mode without straddling; `urb_rel_offset` is verified URB byte index. | **Conditionally Implementable** (Requires Entry Tracking) | At line 320, the parser only knows `i` (current chunk index). If `i < 8`, the marker certainly straddled. If $i \ge 8$, the marker *may* have been wholly contained in `data`, but only if `marker_stage` and `hdr_pos` were 0 at entry to `gc550_parse_ordered_bytes()`. Currently, entry state is not saved. |
| **`GC550_PROV_PAIR_BUF`** | Event occurred while parsing `pair_buf`; `urb_rel_offset = U32_MAX`. | **Verified and Implementable** | Implementable by checking `p->swap_mode && p->pair_buf`. `urb_rel_offset` is set to `U32_MAX`, and `pair_pos` accurately records the position in the 32 KiB pair buffer. However, the exact contributing URBs remain composite. |
| **`GC550_PROV_COMPOSITE_STRADDLE`** | Marker bytes crossed a URB or pair boundary; `urb_rel_offset = U32_MAX`. | **Not Reliably Distinguishable** with Current Parser State | While straddling within the first 8 bytes of a chunk is detectable ($i < 8$), distinguishing a URB boundary straddle from a 16 KiB pair half straddle cannot be done without passing dispatch metadata into the parser. |

> [!CRITICAL]
> **Conclusion on Byte Provenance:**  
> In the absence of dispatch context inside `gc550_parse_ordered_bytes()`, all parser events must treat byte provenance as **unknown/composite**. Fabricating a precise `urb_rel_offset` or attributing a straddled marker to the currently executing URB callback is technically invalid.

---

## 4. 80-Byte Record ABI and Bitfield Audit

### Actual Memory Layout of `struct gc550_diag_record`
Audited from `kernel/gc550-diag.h` lines 72–141:

```
[0x00 - 0x07]  u64 timestamp_ns              (8 bytes, offset 0)
[0x08 - 0x0F]  u64 endpoint_payload_offset   (8 bytes, offset 8)
[0x10 - 0x17]  u64 secondary_payload_offset  (8 bytes, offset 16)
[0x18 - 0x1F]  u64 submission_generation     (8 bytes, offset 24)
[0x20 - 0x27]  u64 completion_seq            (8 bytes, offset 32)
[0x28 - 0x2B]  u32 urb_rel_offset            (4 bytes, offset 40)
[0x2C - 0x2F]  u32 distance_from_prev_sav    (4 bytes, offset 44)
[0x30 - 0x33]  u32 segment_id                (4 bytes, offset 48)
[0x34 - 0x37]  u32 payload_len               (4 bytes, offset 52)
[0x38 - 0x39]  u16 pair_pos                  (2 bytes, offset 56)
[0x3A - 0x3B]  u16 cpu_id                    (2 bytes, offset 58)
[0x3C - 0x3D]  s16 urb_status                (2 bytes, offset 60)
[0x3E]         u8  event_type                (1 byte,  offset 62)
[0x3F]         u8  slot_index                (1 byte,  offset 63)
[0x40]         u8  marker_type               (1 byte,  offset 64)
[0x41]         u8  marker_seq                (1 byte,  offset 65)
[0x42]         u8  parser_state              (1 byte,  offset 66)
[0x43]         u8  swap_mode                 (1 byte,  offset 67)
[0x44]         u8  swap_votes                (1 byte,  offset 68)
[0x45]         u8  active_callbacks          (1 byte,  offset 69)
[0x46 - 0x47]  u16 flags                     (2 bytes, offset 70)
[0x48 - 0x4F]  u8  reserved[8]               (8 bytes, offset 72)
Total Size: Exactly 80 bytes (__packed __aligned(8))
```

### The Flag Bitfield Exhaustion Problem

`kernel/gc550-diag.h` lines 48–61 defines:
```c
#define GC550_FLAG_SHORT_TRANSFER             BIT(0)
#define GC550_FLAG_ZERO_LENGTH                BIT(1)
#define GC550_FLAG_TRANSFER_ERROR             BIT(2)
#define GC550_FLAG_PARTIAL_PAYLOAD_DISCARDED  BIT(3)
#define GC550_FLAG_URB_CANCELLED              BIT(4)
#define GC550_FLAG_SUBMISSION_FAILURE         BIT(5)
#define GC550_FLAG_STREAM_DISCONTINUITY       BIT(6)
#define GC550_FLAG_REORDER_STRADDLE           BIT(7)
#define GC550_FLAG_DIST_OVERFLOW              BIT(8)
#define GC550_FLAG_ABNORMAL_ABORT             BIT(9)
#define GC550_FLAG_DEVICE_DISCONNECTED        BIT(10)
#define GC550_FLAG_LIFECYCLE_STOP             BIT(11)
#define GC550_FLAG_PROVENANCE_OVERFLOW        BIT(12)
#define GC550_FLAG_MULTI_URB_SPAN             BIT(13)
```

1. **Available Bits:**
   `u16 flags` has only **two unused bits**: `BIT(14)` and `BIT(15)`.
2. **Revision 2 Over-Allocation:**
   Revision 2 proposed adding all of the following into `flags`:
   - `GC550_FLAG_SAV_HDR_VALID`
   - `GC550_FLAG_SAV_HDR_INVALID`
   - `GC550_FLAG_EAV_HDR_VALID`
   - `GC550_FLAG_EAV_HDR_INVALID`
   - `GC550_FLAG_SEQ_MATCH`
   - `GC550_FLAG_SEQ_MISMATCH`
   - `GC550_FLAG_SPAN_VALID`
   - `GC550_FLAG_SPAN_INVALID`
   - `GC550_FLAG_SWAP_CANDIDATE`
   - `GC550_FLAG_CALLBACK_OVERLAP`
   - `GC550_FLAG_SWAPPED_HALF_DISPATCH`
   - Provenance classification bits
3. **Audit Finding:**
   Fitting 12+ flags into 2 available bits is impossible.
   - *Feasible Architectural Remedy:* Use `reserved[0..1]` as an explicit `u16 parser_flags` field, leaving `flags` strictly for transport-level flags, OR formally specify that `flags` is overloaded contextually when `event_type >= GC550_EVT_SAV_DETECTED`.
   - Until formally specified, this remains an **unresolved ABI blocker**.

---

## 5. Complete Parser Control-Flow and Event Coverage

The table below catalogs every control-flow path across `kernel/gc550-video.c` where parser state changes, markers are recognized, or frame buffers terminate:

| Control-Flow Path | Function | Line(s) | Trigger Condition | Event Type | Nature of Event | Available Fields in Scope | Unreliably Captured / Missing Fields |
|---|---|---|---|---|---|---|---|
| **P1** | `gc550_start_streaming()` | 791 | Stream startup | `GC550_EVT_PARSER_RESET` | Lifecycle / Reset | `epoch`, `cpu` | `sub_id`, `comp_seq`, `slot` |
| **P2** | `gc550_urb_complete()` | 560 | USB gap / transfer error | `GC550_EVT_PARSER_RESET` | Exception / Reset | `ctx`, `urb`, `sub_id`, `comp_seq` | None |
| **P3** | `gc550_reserve_video_frame()` | 158–161 | New SAV while `cur_buf != NULL` | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (ERROR)** | `frame_sequence`, `state` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P4** | `gc550_reserve_video_frame()` | 163–166 | Valid SAV header | `GC550_EVT_SAV_DETECTED` | Observation / State Change| `sav_b0`, `frame_sequence` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P5** | `gc550_parse_ordered_bytes()` | 323–326 | Invalid SAV checksum | `GC550_EVT_PARSER_DESYNC` | **Desync Branch D1** | `hdr_buf[0..3]`, `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P6** | `gc550_abort_video_frame()` | 142 | SAV fail abort | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (ERROR)** | Buffer pointer | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P7** | `gc550_parse_ordered_bytes()` | 333 | Valid EAV checksum | `GC550_EVT_EAV_DETECTED` | Observation | `eav_c0`, `sav_b0`, `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P8** | `gc550_parse_ordered_bytes()` | 334–335 | EAV sequence mismatch | `GC550_EVT_PARSER_DESYNC` | **Desync Branch D2** | `eav_c0`, `sav_b0`, `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P9** | `gc550_parse_ordered_bytes()` | 339–342 | Invalid EAV checksum | `GC550_EVT_PARSER_DESYNC` | **Desync Branch D3** | `hdr_buf[0..4]`, `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P10** | `gc550_abort_video_frame()` | 142 | EAV fail abort | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (ERROR)** | Buffer pointer | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P11** | `gc550_parse_ordered_bytes()` | 348–351 | Audio marker `00 FF FF 58` | `GC550_EVT_AUDIO_HEADER` | Observation | `audio_rem`, `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P12** | `gc550_complete_video_frame()`| 227 | `span <= 32K && c0 != b0` | `GC550_EVT_SWAP_VOTE` | Decision (Swap Candidate)| `swap_votes`, `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P13** | `gc550_complete_video_frame()`| 229 | `swap_votes >= 2` | `GC550_EVT_SWAP_TOGGLE` | State Transition | Old/new `swap_mode`, `pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P14** | `gc550_complete_video_frame()`| 237 | Swap vote abort | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (ERROR)** | `video_pos` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P15** | `gc550_complete_video_frame()`| 254 | `!valid_span` | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (ERROR)** | `video_pos`, `frame_size` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P16** | `gc550_complete_video_frame()`| 267 | `valid_span == true` | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (DONE)** | `video_pos`, `frame_size` | `sub_id`, `comp_seq`, `slot`, `urb_rel_offset` |
| **P17** | `gc550_stop_stream_resources()`| 768 | STREAMOFF teardown | `GC550_EVT_FRAME_COMPLETE` | **Terminal Outcome (ERROR)** | Buffer list | `sub_id`, `comp_seq`, `slot` |

---

## 6. Remaining Risks and Blockers

The following four technical blockers must be resolved in an updated Revision 3 specification before Stage 2B implementation can be approved:

### Blocker 1: Scope Inaccessibility of Dispatch Context
Inside `gc550_parse_ordered_bytes()`, the dispatch context (`submission_generation`, `completion_seq`, `slot_index`) is completely out of scope.
- *Resolution Required in Spec:* Either:
  1. Formally document that `gc550_parse_stream_chunk()` and `gc550_parse_ordered_bytes()` signatures will be updated to accept `const struct gc550_urb_ctx *ctx` and `u64 entry_seq` as read-only arguments, OR
  2. Formally specify that parser events leave `submission_generation = 0`, `completion_seq = 0`, and `slot_index = 0`.

### Blocker 2: Flag Bitfield Overrun
The 16-bit `u16 flags` field cannot accommodate 10+ new parser flags alongside the 14 existing transport flags.
- *Resolution Required in Spec:* Formally allocate `reserved[0..1]` in `struct gc550_diag_record` as `u16 parser_flags` with explicit bit definitions, OR specify contextual bit reuse based on `event_type`.

### Blocker 3: Lack of Boundary Entry Tracking for Byte Provenance
The parser currently does not track `entry_marker_stage` or `entry_hdr_pos` at chunk entry, making it impossible to prove that a marker did not cross a boundary.
- *Resolution Required in Spec:* Formally mandate setting `urb_rel_offset = U32_MAX` for **all** parser events, treating all parser-level byte provenance as composite.

### Blocker 4: Missing Frame-Abort Path in `gc550_reserve_video_frame()`
Path P3 (lines 158–161) finishes an unclosed frame buffer with `VB2_BUF_STATE_ERROR` when an SAV arrives without a closing EAV.
- *Resolution Required in Spec:* Add Path P3 to the Stage 2B control-flow checklist and implementation roadmap.

---

## 7. Final Recommendation

### Status: **NOT READY FOR IMPLEMENTATION APPROVAL**

**Justification:**  
Stage 2B is intended to provide trustworthy diagnostic evidence. Approving an architecture that claims to record URB sequence IDs and precise byte offsets at call sites where those variables do not exist in C scope—and that over-allocates a 16-bit flag field—would result in implementation defects or broken diagnostic records.

A revised **Specification Revision 3** addressing the four actionable blockers above will achieve complete technical coherence and enable clean maintainer approval.

---

**Report Prepared By:**  
Independent Technical Auditor  
*Project lgx2KernelLevel*
