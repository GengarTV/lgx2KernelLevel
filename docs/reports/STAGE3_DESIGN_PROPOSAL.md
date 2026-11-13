# E1.9 Stage 3 – Technical Design Proposal (Revision 4): Marker-Referenced Phase Proof and Architectural Feasibility Audit

**Project:** `lgx2KernelLevel`  
**Target Hardware:** AVerMedia Live Gamer Extreme GC550 (`07ca:4710`)  
**Host Kernel:** Linux `7.3.3-1-cachyos` (x86_64, Clang/LLVM)  
**Author:** Antigravity / Maintainer: Øystein Lie  
**Baseline:** E1.9 Stage 2B (Passive Telemetry & Hardware Validation Completed)  
**Date:** 2026-11-13  
**Status:** Revision 4 Proposal – Final Technical Feasibility Audit  

---

## 1. Executive Summary and Scope

Following the maintainer review of Revision 3, this document provides the formal mathematical derivations, source-code audits, and physical data-path analyses required to resolve the remaining inconsistencies in **Investigation Area 1 (FX3 DMA Buffer Ordering and Swap Calibration)** and **Investigation Area 2 (HDMI Resolution Transitions and Runt-Frame Handling)**.

### Core Architectural Findings of Revision 4:
1. **Mathematical Underdetermination of Physical DMA Phase ($H$):**  
   A rigorous derivation proves that the observed distance between SAV and EAV ($\Delta = P_{EAV} - P_{SAV}$) contains **zero information regarding the absolute physical DMA phase $H$**, because $H$ cancels out identically. Furthermore, because the Analog Devices ADV7604 raster clock and the Cypress FX3 DMA channel are asynchronous, the physical coordinate of SAV within the FX3 buffer ($x_{SAV}$) is an independent unknown. Therefore, observing marker positions relative to the host stream origin cannot determine $H$ analytically.
2. **Invalidity of Non-Zero `pair_pos` Initialization:**  
   Source-code inspection confirms that `p->pair_pos` is the write cursor for `p->pair_buf`. Setting `pair_pos` to a non-zero offset before or after `gc550_parser_reset()` is either immediately wiped out by `gc550_parser_reset()` (which sets `pair_pos = 0`), or causes the driver to dispatch **uninitialized kernel memory** from the head of `pair_buf` directly into userspace video frames.
3. **Mid-URB Transition Hazards:**  
   Switching swap modes mid-URB or discarding remaining URB bytes severs the cumulative byte count modulo 32,768, displacing the physical phase of all subsequent URBs and inducing persistent desynchronization.
4. **Absence of Active HDMI Signal Monitoring:**  
   `gc550_log_signal_status()` returns `void`, is invoked strictly once at `STREAMON`, and does not export state. The existing driver contains **no interrupt handler, polling timer, or workqueue** to monitor HDMI TMDS lock during streaming. Signal recovery cannot occur mid-stream without userspace initiating a `STREAMOFF` $\rightarrow$ `STREAMON` cycle.
5. **Final Engineering Recommendations:**  
   - **Investigation Area 1:** **`DEFER`**  
   - **Investigation Area 2:** **`DEFER`**

In accordance with strict operational constraints, this is a **design and analysis document only**. No driver source files, build systems, or runtime parameters are modified.

---

## 2. Issue 1: Physical Phase Derivation and Mathematical Proof

### 2.1 Formal Coordinate Definitions

To analyze the physical stream rigorously, define three distinct coordinate systems:

```
[System C: Chronological Video Raster]
  Continuous digital pixel raster clocked by ADV7604:
  Line 0, Line 1, ... Line N (SAV at start of active video, EAV at end)
  Coordinate: t in [0, inf)

             │ (Asynchronous bridging into FX3 GPIF II)
             ▼
[System X: Physical Cypress FX3 DMA Channel]
  Dual 16 KiB ping-pong SRAM buffers committed to USB Bulk IN FIFO:
  Buffer 0: size S = 16,384 bytes
  Buffer 1: size S = 16,384 bytes
  Pair quantum: 2S = 32,768 bytes
  Coordinate: x in [0, inf)
  Physical 16 KiB buffer boundary: x = 0 (mod 16,384)
  Physical 32 KiB pair boundary:   x = 0 (mod 32,768)

             │ (USB Bulk IN transfer across Endpoint 0x83)
             ▼
[System Y: Host-Received Byte Stream]
  Sequential byte stream delivered to gc550_urb_complete():
  URB 0 (512 KiB), URB 1 (512 KiB), ...
  Coordinate: y in [0, inf), where y = 0 is byte 0 of URB 0 after STREAMON
```

#### The Relationship Between Host Stream $y$ and Physical FX3 Space $x$:
Let $H \in [0, 32,767]$ be the **unknown physical DMA phase** in the FX3 FIFO at the instant the host begins receiving byte $y = 0$:
$$x(y) = (H + y) \pmod{32,768}$$

The physical 16 KiB buffer boundaries in host coordinates occur at:
$$y \equiv (16,384 - (H \pmod{16,384})) \pmod{16,384}$$

The physical 32 KiB pair boundaries in host coordinates occur at:
$$y \equiv (32,768 - H) \pmod{32,768}$$

---

### 2.2 Proof that Marker Distance $\Delta$ Contains Zero Information About $H$

Let:
- $P_{SAV} \ge 0$ be the observed byte offset of the SAV header (`00 FF FF C0`) in the host stream.
- $P_{EAV} > P_{SAV}$ be the observed byte offset of the subsequent EAV header (`00 FF FF C1`) in the host stream.

In physical FX3 space, the corresponding coordinates are:
$$x_{SAV} = (H + P_{SAV}) \pmod{32,768}$$
$$x_{EAV} = (H + P_{EAV}) \pmod{32,768}$$

Now consider the observed distance $\Delta$ between the markers:
$$\Delta = P_{EAV} - P_{SAV}$$

Expressing $P_{SAV}$ and $P_{EAV}$ in terms of physical coordinates:
$$P_{SAV} = x_{SAV} - H$$
$$P_{EAV} = x_{EAV} - H$$

Subtracting the two equations:
$$\Delta = (x_{EAV} - H) - (x_{SAV} - H) = x_{EAV} - x_{SAV}$$

#### Mathematical Conclusion:
**The parameter $H$ cancels out identically.**  
The observable distance $\Delta$ is completely invariant to $H$. It depends exclusively on the physical separation of the markers in the FX3 DMA stream.

---

### 2.3 Proof that Marker Positions Cannot Determine $H$ Analytically

Can the absolute observed position $P_{SAV}$ determine $H$?

From the definition of $x_{SAV}$:
$$H \equiv (x_{SAV} - P_{SAV}) \pmod{16,384}$$

In this congruence:
- $P_{SAV}$ is known (measured by the host parser).
- $H$ is the unknown physical FIFO phase at $y = 0$.
- $x_{SAV}$ is the physical offset within the FX3 16 KiB buffer where the ADV7604 clocked the first active video pixel.

#### Why $x_{SAV}$ is an Independent Unknown:
The Analog Devices ADV7604 and the Cypress FX3 operate in **separate clock and reset domains**:
1. The ADV7604 pixel clock is driven by the HDMI TMDS clock from the external HDMI source.
2. The Cypress FX3 GPIF II state machine and internal DMA buffer pointers are initialized by the FX3 firmware upon USB enumeration.
3. There is no synchronization signal or genlock between the ADV7604 vertical sync and the FX3 DMA buffer start.
4. When video streaming starts, the first SAV can occur at **any arbitrary byte offset** $x_{SAV} \in [0, 16,383]$ within an FX3 buffer.

Because $x_{SAV}$ is an independent, unmeasured random variable:
$$\text{Number of Unknowns} = 2 \quad (H, \, x_{SAV})$$
$$\text{Number of Independent Equations} = 1 \quad (H + P_{SAV} \equiv x_{SAV})$$

#### Rigorous Verdict:
**$H$ is mathematically underdetermined.**  
The marker positions relative to the host stream origin $y = 0$ contain insufficient information to calculate the physical DMA phase $H$. Knowing the raster dimensions ($W \times H \times 2 = 4,147,200$) only defines distances between raster elements; it does not anchor the raster to the FX3 DMA buffer pointers.

---

### 2.4 Distinguishing Swap Detection from Phase Determination

It is vital to distinguish between two completely different properties of the stream:

| Property | Definition | Observable Evidence | Can Be Determined? |
|---|---|---|:---:|
| **Buffer Ordering (Swap Mode)** | Whether the FX3 emits Buffer 0 then Buffer 1 (`NORMAL`), or Buffer 1 then Buffer 0 (`SWAPPED`). | $\Delta \le 32\text{ KiB}$ with $eav\_c0 \ne sav\_b0$ vs $\Delta \approx frame\_size$ with $eav\_c0 == sav\_b0$. | **YES** (100% Deterministic) |
| **Physical DMA Phase ($H$)** | The exact byte offset $H \in [0, 32767]$ from host byte $y=0$ to the true physical FX3 32 KiB pair boundary. | Absolute seam location where physical 16 KiB blocks were spliced by the hardware DMA engine. | **NO** (Analytically Underdetermined) |

While the driver can detect with 100% mathematical certainty whether the stream requires 16 KiB swapping, it **cannot** deduce the phase offset $H$ from marker headers alone.

---

### 2.5 Data-Driven Phase Discovery and Why It Is Prohibitive in Kernel Space

If $H$ cannot be deduced analytically, could the driver discover $H$ empirically by evaluating candidate alignments?

#### The Candidate Evaluation Architecture:
1. Retain $N$ raw URBs (e.g. 4–8 MB) in a kernel staging buffer upon `STREAMON`.
2. Evaluate candidate phase alignments:
   - In USB SuperSpeed, transfers are quantized into 1024-byte packets. If residual FIFO data is packet-aligned, there are:
     $$M = \frac{32,768}{1,024} = 32 \text{ candidate alignments}$$
   - If residual data can be byte-arbitrary, there are:
     $$M = 32,768 \text{ candidate alignments}$$
3. For each candidate $\hat{H} \in \{0, 1024, \dots, 31744\}$:
   - Slices the retained buffer into 32 KiB blocks starting at $\hat{H}$.
   - Dispatches Half 1 then Half 0 through a trial parser.
   - Evaluates whether:
     a) `gc550_sav_hdr_valid()` passes.  
     b) `gc550_eav_hdr_valid()` passes.  
     c) $eav\_c0 == sav\_b0$.  
     d) $video\_pos \in [frame\_size - 16384, \, frame\_size + 16384]$.  
4. **Candidate Ambiguity and Rejection:**
   Because video payloads can contain random byte sequences that coincidentally match marker prefixes (`00 FF FF C0`), a candidate cannot be validated by a single marker. Demonstrating that a candidate is unique requires parsing at least **2 full consecutive frame spans** (~8.3 MB for 1080p).
5. **Why This Is Prohibitive in Kernel Space:**
   - Testing 32 candidates $\times$ 8.3 MB requires parsing up to **265 megabytes of byte-by-byte data** in kernel execution context.
   - Executing this computational loop inside softirq or workqueue context would stall CPU cores for hundreds of milliseconds, triggering Linux kernel RCU stall warnings (`rcu_sched detected stalls on CPUs/tasks`) and system watchdog timeouts.
   - Retaining 8.3 MB of raw transport data requires significant contiguous memory allocation in kernel space (`vmalloc`), introducing allocation latency and potential out-of-memory failures under high memory fragmentation.

---

## 3. Issue 2: Pair Buffer Semantics and Initialization Audit

### 3.1 Exact Semantics of `pair_pos` in Existing Source Code

In [kernel/gc550.h](file:///home/gengartv/lgx2KernelLevel/kernel/gc550.h#L135-L136):
```c
u8 *pair_buf;      /* 32 KiB staging buffer when swap_mode is active */
u32 pair_pos;      /* Current write cursor in pair_buf */
```

In [kernel/gc550-video.c](file:///home/gengartv/lgx2KernelLevel/kernel/gc550-video.c#L580-L616):
```c
while (len > 0) {
    size_t space = GC550_PAIR_BUF_SIZE - p->pair_pos;
    size_t copy = min(len, space);

    memcpy(p->pair_buf + p->pair_pos, data, copy);
    p->pair_pos += copy;
    data += copy;
    len -= copy;

    if (p->pair_pos == GC550_PAIR_BUF_SIZE) {
        p->pair_pos = 0;

        /* Dispatch Half 1 first */
        gc550_parse_ordered_bytes(dev,
                                  p->pair_buf + GC550_FX3_DMA_BUF_SIZE,
                                  GC550_FX3_DMA_BUF_SIZE);

        /* Dispatch Half 0 second */
        gc550_parse_ordered_bytes(dev,
                                  p->pair_buf,
                                  GC550_FX3_DMA_BUF_SIZE);
    }
}
```

#### Established Semantics:
`p->pair_pos` is strictly an **accumulation write index**. It indicates how many bytes have been written into `p->pair_buf` from the current incoming stream chunk. It is **not** a phase-shift register or circular buffer pointer.

---

### 3.2 Contradiction in Revision 3: The `gc550_parser_reset()` Conflict

Revision 3 proposed:
```c
p->pair_pos = (32768 - R) % 32768;
gc550_parser_reset(p);
```

Now inspect [kernel/gc550-video.c](file:///home/gengartv/lgx2KernelLevel/kernel/gc550-video.c#L64-L73):
```c
void gc550_parser_reset(struct gc550_stream_parser *parser)
{
    parser->state = GC550_STATE_IDLE;
    parser->sav_b0 = 0;
    parser->hdr_pos = 0;
    parser->audio_rem = 0;
    parser->video_pos = 0;
    parser->marker_stage = 0;
    parser->pair_pos = 0;  /* Line 72: Unconditionally resets pair_pos to 0 */
}
```

#### The Conflict:
`gc550_parser_reset()` unconditionally overwrites `pair_pos = 0`. Any calculated non-zero value is instantly erased.

---

### 3.3 Stale / Uninitialized Kernel Memory Dispatch

Even if `pair_pos` were assigned *after* `gc550_parser_reset()`:
Suppose `p->pair_pos` is initialized to non-zero offset $K = 10,000$:
1. When incoming data arrives, `memcpy(p->pair_buf + 10000, data, copy)` writes starting at byte 10,000.
2. Bytes `p->pair_buf[0 .. 9999]` are **never written**. They contain whatever stale bytes existed from previous streams, or uninitialized heap memory from `kmalloc()`.
3. When `pair_pos` reaches 32,768, the driver executes:
   - Dispatch Half 1: `pair_buf[16384 .. 32767]`.
   - Dispatch Half 0: `pair_buf[0 .. 16383]`.
4. Half 0 contains `p->pair_buf[0 .. 9999]`—**10,000 bytes of stale kernel data**.
5. This stale memory is dispatched directly to `gc550_parse_ordered_bytes()` and written into userspace video buffers via `gc550_append_video_bytes()`.

#### Critical Security and Functional Flaw:
Assigning a non-zero `pair_pos` does **not** align the stream. It creates severe buffer corruption and leaks uninitialized kernel memory to userspace.

#### The Only Valid Stream Alignment Technique:
Physical phase alignment can **only** be achieved by **skipping $K$ residual bytes in the incoming stream**:
```c
data += K;
len  -= K;
p->pair_pos = 0;
```
`pair_buf` must always be filled cleanly from index 0 to 32,767.

---

## 4. Issue 3: Calibration Transition and Remaining URB Data

When calibration criteria are met (e.g. vote 2 detected at byte 18,432 of an active 512 KiB URB):

### 4.1 The Mid-URB Remainder Hazard
- Total URB length: 524,288 bytes.
- Offset where vote 2 occurs: $M = 18,432$ bytes.
- Remaining unprocessed bytes in the current URB:
  $$524,288 - 18,432 = 505,856\text{ bytes}$$

### 4.2 Why Discarding the Remainder Fails:
If the driver discards the remaining 505,856 bytes and attempts to restart pair buffering at byte 0 of the next URB:
$$505,856 \pmod{32,768} = 14,336 \ne 0$$
Discarding 505,856 bytes drops a non-multiple of 32 KiB from the continuous hardware stream. This shifts the physical phase of the next URB by 14,336 bytes, destroying all subsequent pairing alignment.

### 4.3 Why Immediate Processing Fails:
If the driver switches `swap_mode = true` immediately at byte 18,432, `pair_buf` starts slicing at an unaligned coordinate ($18,432 \pmod{32768} = 18,432$), creating the exact $+32\text{ KiB}$ displacement and permanent desync proved in Stage 2B.

### 4.4 Transition Requirement:
The only mathematically valid transition is to continue processing bytes linearly under the initial hypothesis until an exact physical 32 KiB boundary ($offset \equiv 0 \pmod{32,768}$) is reached, while discarding completed video buffers via `VB2_BUF_STATE_ERROR`.

---

## 5. Issue 4: HDMI Signal Lock and Monitoring Audit

### 5.1 Source Code Audit of `gc550_log_signal_status()`

In [kernel/gc550-cmd.c](file:///home/gengartv/lgx2KernelLevel/kernel/gc550-cmd.c#L412-L460):
```c
/**
 * gc550_log_signal_status - Query ADV7604 HDMI cable, TMDS clock, and measured timings
 */
void gc550_log_signal_status(struct gc550_dev *dev)
{
    ...
    dev_info(&dev->intf->dev,
             "HDMI Status: cable=%s, signal=%s (%ux%u%c, %s, HDCP=%s, TMDS=%uMHz)\n",
             cable_5v ? "YES" : "NO",
             (!pwr_down && stdi_valid && tmds_lock && tmds_clk) ? "LOCKED" : "NONE",
             width, height, (io_12 & 0x10) ? 'i' : 'p', ...);
}
```

#### Established Facts:
1. `gc550_log_signal_status()` returns `void`.
2. It does not store or export `tmds_lock` or `tmds_clk` to `struct gc550_dev`.
3. It is called **only once**, during `gc550_start_streaming()` ([gc550-video.c#L960](file:///home/gengartv/lgx2KernelLevel/kernel/gc550-video.c#L960)).

### 5.2 Complete Absence of Continuous Active Monitoring
- The ADV7604 interrupt pin is not wired or polled.
- The driver possesses **no background timer, no workqueue, and no interrupt handler** monitoring HDMI signal integrity during active streaming.
- **Architectural Conclusion:**  
  The driver has no mechanism to detect when an HDMI cable is unplugged or when TMDS lock is lost during active capture. Revision 3's proposal to transition to `UNCALIBRATED` on TMDS unlock is impossible without implementing a polling workqueue or interrupt mechanism, which is explicitly forbidden under the current project constraints.
- Recovery after a signal-loss event only occurs if userspace detects frame starvation and restarts capture via `STREAMOFF` $\rightarrow$ `STREAMON`.

---

## 6. Investigation Area 2: HDMI Resolution Transitions and Runt-Frame Handling

*(Preserved without modification from Revision 1 and Revision 3.)*

### 6.1 Status Quo Evaluation
- During an HDMI resolution transition, the first incoming frame delivered over USB Bulk IN can be a partial runt frame ($dst < frame\_size$).
- The driver parser currently detects this runt frame via `valid_span == false`, marks it with `VB2_BUF_STATE_ERROR`, resets `video_pos = 0`, and delivers it to userspace via `vb2_buffer_done()`.
- The parser recovers instantly on the very next frame, achieving 100% steady-state stability.
- Visual inspection across all four resolutions (1080p, 720p, 576p, 480p) confirmed clean 1:1 image delivery.
- Modifying videobuf2 buffer completion lifecycles without a proven defect introduces unwarranted regression risks.

**Recommendation for Area 2: `DEFER`**

---

## 7. Final Recommendations

### Investigation Area 1 (FX3 DMA Buffer Ordering & Swap Calibration)
**Recommendation: `DEFER`**

**Technical Justification:**
1. **Mathematical Block:** The physical phase $H$ is mathematically underdetermined from marker observations alone ($H$ cancels out of $\Delta$, and $x_{SAV}$ is an independent unknown).
2. **Implementation Block:** Modifying `pair_pos` to a non-zero offset is wiped out by `gc550_parser_reset()` or injects uninitialized kernel memory into userspace video buffers.
3. **Transport Block:** Transitioning swap state mid-URB severs cumulative modulo 32,768 tracking, causing permanent desynchronization.
4. **Monitoring Block:** The driver possesses no active monitoring mechanism to detect TMDS signal loss during streaming.
5. **Stability of Existing Baseline:** In all physical hardware tests with established `swap_mode = true`, the driver achieved **100% flawless steady-state capture across >750,000 URBs and 54,000 frames with zero desyncs**. Introducing an incomplete or unproven calibration mechanism risks compromising this validated baseline.

---

### Investigation Area 2 (HDMI Resolution Transition & Runt-Frame Handling)
**Recommendation: `DEFER`**

**Technical Justification:**  
Current driver behavior strictly adheres to V4L2/videobuf2 conventions (`VB2_BUF_STATE_ERROR`) and recovered cleanly in interactive visual testing.

---

## 8. Requirements for Future Resolution of Area 1

Before Investigation Area 1 can be safely approved for implementation, future engineering stages must establish:

1. **Hardware-Level FIFO Reset:**  
   Investigation of Cypress FX3 USB vendor commands or standard control requests (e.g., vendor-specific EP reset) to determine if a command exists that flushes the FX3 Bulk IN 0x83 FIFO and resets the internal GPIF II DMA channel pointers on `STREAMON`.
2. **Phase Invariance Verification:**  
   Empirical hardware testing to establish whether $H$ is consistently $0 \pmod{32768}$ across all cold-boot cycles when the device is un-halted, which would prove that the FX3 FIFO is naturally packet-aligned upon stream start.
3. **Active Signal Monitoring Architecture:**  
   An approved architectural specification for a periodic status polling workqueue or interrupt handler to monitor ADV7604 TMDS lock during streaming without incurring lock-contention penalties on the video streaming pipeline.

---

*Awaiting maintainer approval before taking any further action.*
