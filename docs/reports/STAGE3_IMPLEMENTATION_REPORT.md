# E1.9 Stage 3 – Implementation Report: FX3 Hardware Reset Feasibility

**Project:** `lgx2KernelLevel`  
**Target Hardware:** AVerMedia Live Gamer Extreme GC550 (`07ca:4710`)  
**Host Kernel:** Linux `7.3.3-1-cachyos` (x86_64, Clang/LLVM)  
**Author:** Antigravity / Maintainer: Øystein Lie  
**Baseline:** E1.9 Stage 2B (Passive Telemetry & Hardware Validation Completed)  
**Date:** 2026-11-13  
**Overall Status:** **`BLOCKED – HARDWARE CAPABILITY NOT ESTABLISHED`**  

---

## 1. Executive Summary

In accordance with the maintainer's authorization and strict implementation gate for **Stage 3 Phase 1**, an exhaustive investigation into the GC550 hardware architecture, USB command protocol, Cypress FX3 firmware capabilities, and reverse-engineering records was conducted.

The objective was to determine whether a supported hardware mechanism exists to:
1. Reset the Cypress FX3 GPIF II DMA channel.
2. Flush residual data from Bulk IN Endpoint `0x83`.
3. Re-establish a known DMA buffer-pair boundary prior to host URB submission.

### Implementation Gate Verdict:
**No supported hardware reset or endpoint FIFO flush mechanism exists on the AVerMedia GC550.**  
In strict compliance with the maintainer's directive:
- Speculative vendor commands and undocumented register operations were **not invented**.
- Marker-based physical phase calculations (proved underdetermined in Revision 4) were **not implemented**.
- Unilateral hardcoding of `swap_mode = true` was **not applied**.
- The previously rejected mid-stream dynamic voting and unaligned URB-boundary workarounds were **not implemented**.
- The validated Stage 2B driver source code was **preserved without speculative alterations**.

---

## 2. Technical Investigation of FX3 Hardware Reset Feasibility

### 2.1 Hardware Topology Audit
The capture pipeline on the GC550 physical board follows this signal path:
```
[HDMI Input] ──► [Analog Devices ADV7604] (TMDS Receiver & Audio Decoder)
                         │ (Parallel 8-bit BT.1120 Video + Audio)
                         ▼
                 [Lattice ECP3 FPGA] (Packetizer & Stream Marker Injector)
                         │ (Parallel GPIF II Interface)
                         ▼
                 [Cypress EZ-USB FX3] (USB 3.0 Controller: ARM9 + GPIF II)
                         │
                         ▼
                 [USB 3.0 Bulk IN Endpoint 0x83] ──► [Host Linux Driver]
```

1. **The Lattice ECP3 FPGA (I2C `0x10`):** Acts as the stream packetizer, inserting the ITU-R BT.1120 SAV (`00 FF FF C0`), EAV (`00 FF FF C1`), and Audio (`00 FF FF 58`) preambles before feeding the Cypress FX3 parallel GPIF II bus. Its internal register map is closed and undocumented.
2. **The Cypress EZ-USB FX3 Controller:** Bridges the parallel GPIF II bus into internal 16 KiB dual-buffer SRAM sockets (Buffer 0 and Buffer 1) and transfers them over USB SuperSpeed Bulk IN Endpoint `0x83`.
3. **The Command Bridge (Bulk OUT `0x01` / Bulk IN `0x81`):** Serves exclusively as an I2C and memory bridge managed by the FX3 ARM9 firmware.

---

### 2.2 USB Command Protocol Audit
An audit of the reverse-engineered Windows driver (`avmu3_x64.sys`) by Chris Sawczuk ([PROTOCOL.md](file:///home/gengartv/lgx2KernelLevel/PROTOCOL.md)), the project reverse-engineering reference ([docs/GC550-REVERSE-ENGINEERING.md](file:///home/gengartv/lgx2KernelLevel/docs/GC550-REVERSE-ENGINEERING.md)), and the kernel driver command implementation ([kernel/gc550-cmd.c](file:///home/gengartv/lgx2KernelLevel/kernel/gc550-cmd.c)) established all supported opcodes:

| Opcode | Payload Format | Functional Purpose | Can Reset FX3 DMA / Flush FIFO? |
|:---:|---|---|:---:|
| **`0x01`** | `[01] [slave] [01] [reg] [data...]` | I2C Write (ADV7604, ADV7511, PCA9539A, Codec, FPGA) | **NO** (I2C bus only) |
| **`0x02`** | `[02] [slave] [01] [len] [reg]` | I2C Read (ADV7604, ADV7511, PCA9539A, Codec, FPGA) | **NO** (I2C bus only) |
| **`0x0D`** | `[0D] [addr_h] [addr_l] [len] [data...]` | EEPROM Configuration Write | **NO** (Non-volatile memory) |
| **`0x0E`** | `[0E] [addr_h] [addr_l] [len]` | EEPROM Configuration Read | **NO** (Non-volatile memory) |
| **`0x15`** | `[15] [addr_h] [addr_l] [val]` | Single-byte Configuration Write | **NO** (Non-volatile memory) |
| **`0x16`** | `[16] [addr_h] [addr_l]` | Single-byte Configuration Read | **NO** (Non-volatile memory) |
| **`0x05..0x09`**| `[05..09] ...` | Atmel AT88SC CryptoMemory authentication (safety blocked) | **NO** (EEPROM crypto auth) |
| **`0x08`, `0x14`**| None | Untraced RTTI virtual methods (no arguments) | **NO** (Uncharacterized / unused) |

**Conclusion:** The GC550 command bridge protocol provides **no command to query FX3 internal DMA pointers, reset the GPIF II state machine, or flush Endpoint 0x83**.

---

### 2.3 USB Core and Endpoint Semantics
1. **`usb_clear_halt()`:** In standard USB 3.0 architectures, issuing `CLEAR_FEATURE(ENDPOINT_HALT)` resets endpoint stall status in the USB device core. However, in Cypress FX3 firmware, clearing endpoint halt does not reinitialize internal GPIF II DMA channel buffers or reset ping-pong phase pointers without explicit firmware event handling.
2. **`usb_kill_urb()`:** Only cancels pending host-side URBs in the host xHCI controller. It sends no packet across the USB wire and has zero effect on the physical FX3 FIFO.
3. **Commit History Confirmation (`cba5ee3`):**  
   Commit `cba5ee3` ("Restore swapped 16 KiB DMA buffer order before parsing") confirmed that after an unclean stop, the device delivers 16 KiB buffers with every pair swapped, persisting until physical power-off. Neither upstream `lgx2userspace` nor the official Windows driver possessed a hardware command to reset the FX3 DMA channel.

---

## 3. Implementation Gate Decision

Because no documented or supported hardware reset capability exists:
1. **Implementation Status:** **BLOCKED**
2. **Preservation of Baseline:** The working Stage 2B kernel driver source code is preserved verbatim.
3. **Prevention of Regressions:** Speculative, unverified vendor commands were avoided, ensuring zero risk of bricking or wedging device firmware.

---

## 4. Modified Files Summary

| File | Status | Reason |
|---|:---:|---|
| `kernel/gc550-video.c` | **Unmodified** | Preserved validated Stage 2B capture baseline. |
| `kernel/gc550-core.c` | **Unmodified** | Preserved validated Stage 2B lifecycle baseline. |
| `kernel/gc550-cmd.c` | **Unmodified** | Preserved validated Stage 2B command bridge. |
| `kernel/gc550.h` | **Unmodified** | Preserved validated Stage 2B structures. |
| `Kbuild` | **Unmodified** | Preserved validated dual-configuration build support. |

---

*Phase 1 Complete. Progression to Phase 2 (Build Verification) and Phase 3 (Hardware Validation) reported in accompanying documents.*
