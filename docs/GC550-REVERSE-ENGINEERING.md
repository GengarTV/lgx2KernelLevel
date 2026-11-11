# AVerMedia Live Gamer Extreme GC550 — Hardware Reverse-Engineering Reference

## Purpose and Scope

This document consolidates the hardware, USB, command-protocol, capture-stream, initialization, and audio reverse-engineering knowledge established for the **AVerMedia Live Gamer Extreme (GC550)** into a single long-term technical reference for Linux kernel driver development (`gc550.ko`).

### Attribution and Source Provenance

This reference synthesizes two distinct bodies of work, which are explicitly distinguished throughout this document:

1. **Upstream Protocol & Binary Reverse Engineering (ChrisAJS)** — Foundational analysis of the Windows x64 driver (`avmu3_x64.sys`, `.rdata` RTTI class strings and Ghidra decompilation) and USB captures from the sibling **AVerMedia Live Gamer Extreme 2 (GC551)** (`07ca:0551`) and **Live Gamer Extreme (GC550)** (`07ca:4710` / `07ca:0550`), documented in the upstream [`PROTOCOL.md`](../PROTOCOL.md) reference by Chris Sawczuk (`@ChrisAJS`) and contributors to `lgx2userspace` (licensed under the permissive MIT License; see [`LICENCE.md`](../LICENCE.md)).
2. **GC550 Linux Kernel Driver Implementation & Verification (`lgx2KernelLevel`)** — Source-verified findings from the native V4L2 + ALSA kernel driver ([`kernel/gc550.h`](../kernel/gc550.h), [`kernel/gc550-core.c`](../kernel/gc550-core.c), [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c), [`kernel/gc550-video.c`](../kernel/gc550-video.c), [`kernel/gc550-audio.c`](../kernel/gc550-audio.c), [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h)), hardware runtime testing on GC550 (`07ca:4710`), and offline structural verification of the initialization table and stream parser. Original kernel driver code authored by Øystein Lie is dual-licensed under `GPL-2.0-only OR MIT` (`MODULE_LICENSE("Dual MIT/GPL")`; see [`LICENSES.md`](../LICENSES.md)).

The upstream [`PROTOCOL.md`](../PROTOCOL.md) remains preserved in its original form at the repository root as the historical upstream protocol record under its original MIT license. Note that correcting licensing metadata does not imply the driver is immediately ready for Linux kernel upstream submission, which continues to require correctness hardening, checkpatch review, and multi-resolution validation.

### Evidence Classification Legend

Every major technical claim in this document is classified using one of four evidence levels:

- **`Confirmed`** — Directly verified in the `lgx2KernelLevel` kernel source code, verified USB captures, hardware register readback, or disassembled binary structures with unambiguous execution paths.
- **`Strongly supported`** — Backed by converging evidence from Windows driver decompilation (`avmu3_x64.sys`), standard IC datasheets (Analog Devices ADV7604/ADV7511, NXP PCA9539A, Cypress FX3), and observed capture behaviour, though not every internal hardware state transition has been independently probed in isolation.
- **`Inferred`** — Plausible engineering deduction from RTTI class names, I2C slave addresses, register access patterns, or sibling device behaviour, without direct datasheet or hardware confirmation of the exact circuit wiring.
- **`Unknown`** — Behaviour implemented inside closed-source firmware (Cypress FX3 internal ARM firmware, Lattice ECP3 FPGA bitstream) or uncharacterized I2C slave devices whose internal logic has not been dumped or reverse-engineered.

---

## A. Device Overview

### 1. USB Identity and Board Variants

| Property | GC550 (Primary Hardware Tested) | GC550 (Secondary USB ID) | GC551 (Sibling LGX2 Hardware) | Evidence Level |
|---|---|---|---|---|
| Marketing Name | AVerMedia Live Gamer Extreme (LGX) | AVerMedia Live Gamer Extreme (LGX) | AVerMedia Live Gamer Extreme 2 (LGX2) | `Confirmed` |
| USB Vendor ID (`idVendor`) | `0x07ca` (`USB_VID_AVERMEDIA`) | `0x07ca` | `0x07ca` | `Confirmed` |
| USB Product ID (`idProduct`) | `0x4710` (`USB_PID_GC550`) | `0x0550` (`USB_PID_GC550_ALT`) | `0x0551` | `Confirmed` |
| USB Speed Requirement | SuperSpeed USB 3.x (`bcdUSB 3.00`, 5 Gbps) | SuperSpeed USB 3.x | SuperSpeed USB 3.x | `Confirmed` |
| Analog Component Video In | Present (via multi-pin analog breakout + ADV7604 AFE) | Present | Removed (HDMI-only input) | `Strongly supported` |
| Audio Codec (`0x18`) | Active on I2C bus (`16` writes in `gc550_init_blob`) | Active on I2C bus | Absent (`0x18` not present on GC551) | `Confirmed` |
| ADV7604 InfoFrame Map (`IO[0xF5]`) | Mapped to 7-bit `0x46` (`IO[0xF5] = 0x8C`) | Mapped to 7-bit `0x46` (`IO[0xF5] = 0x8C`) | Mapped to 7-bit `0x3E` (`IO[0xF5] = 0x7C`) | `Confirmed` |
| HDMI Passthrough Architecture | ADV7511 HDMI TX (`0x3D` / `0x38`) re-transmitting from capture path | ADV7511 HDMI TX (`0x3D` / `0x38`) | ITE IT6664 HDMI 2.0 1->4 splitter (`0x2C`/`0x58`/`0x4B`/`0x44..0x47`) | `Confirmed` (I2C traffic) / `Strongly supported` (IC roles) |

Both `07ca:4710` and `07ca:0550` are registered in `gc550_id_table[]` in [`kernel/gc550-core.c`](../kernel/gc550-core.c). Runtime hardware testing in this project was performed on `07ca:4710`.

### 2. Internal Hardware Architecture and Integrated Circuits

The GC550 is an uncompressed USB 3.0 video/audio capture device built around an HDMI/analog receiver, an HDMI passthrough transmitter, an FPGA video/audio packetizer, and a Cypress EZ-USB FX3 SuperSpeed controller.

```
                       +-------------------------------+
 HDMI Input ---------->| Analog Devices ADV7604        |---> Parallel Pixel Bus +
 Component / Analog -->| (HDMI + Analog Receiver)      |     I2S / SPDIF Audio
                       | I2C: 0x20,0x22,0x26,0x32,     |            |
                       |      0x34,0x36,0x46           |            |
                       +-------------------------------+            |
                                       |                            v
                                       |               +-------------------------+
                                       +-------------->| Lattice ECP3 FPGA       |
                                       |               | (Stream Packetizer /    |
                                       v               |  BT.1120 SAV/EAV/AUDIO) |
                       +-------------------------------+ I2C: 0x10               |
 HDMI Passthrough <----| Analog Devices ADV7511        |+-------------------------+
 Output                | (HDMI Transmitter)            |            |
                       | I2C: 0x3D, 0x38               |            | Parallel GPIF II Bus
                       +-------------------------------+            v
                                                       +-------------------------+
 Aux / Mic Analog ---->| TI TLV320AIC3x Audio Codec    | Cypress EZ-USB FX3      |===> USB 3.0 Host
 Audio                 | I2C: 0x18                     | (USB 3.0 Controller &   |     EP 0x01 OUT (Cmd)
                       +-------------------------------+  I2C / 2-Wire Master)   |     EP 0x81 IN  (Reply)
                                                       +-------------------------+     EP 0x83 IN  (Stream)
                                                          |         |         |
                                         +----------------+         |         +----------------+
                                         v                          v                          v
                               +-------------------+      +-------------------+      +-------------------+
                               | NXP PCA9539A      |      | Atmel AT88SC      |      | Other I2C Slaves  |
                               | 16-bit GPIO Exp.  |      | CryptoMemory      |      | 0x4A, 0x65, 0x3F  |
                               | I2C: 0x74         |      | (2-Wire Bridge)   |      | (Uncharacterized) |
                               +-------------------+      +-------------------+      +-------------------+
```

#### Summary of Known and Observed Internal Components

| Component / Bus Target | Bus / Access Path | Role in GC550 | Windows Driver RTTI / Source Evidence | Evidence Level |
|---|---|---|---|---|
| **Cypress EZ-USB FX3** | USB 3.0 Device Controller (ARM9 + GPIF II) | Bridges USB Bulk endpoints to internal I2C/2-wire buses (`EP 0x01`/`0x81`) and streams high-speed GPIF II DMA data from the FPGA over `EP 0x83 IN`. | [`PROTOCOL.md`](../PROTOCOL.md) (`16 KiB` FX3 DMA buffer pair footprint); `GC550_FX3_DMA_BUF_SIZE` in [`kernel/gc550.h`](../kernel/gc550.h) | `Confirmed` |
| **Lattice ECP3 FPGA** | I2C `0x10` (`GC550_I2C_FPGA`) + parallel video/audio bus | Multiplexes raw YUYV 4:2:2 video frames and audio payload blocks with `00 FF FF` markers (`SAV`, `EAV`, `AUDIO`) and feeds FX3 GPIF II FIFO. | `CLFE3::*` RTTI (`"FPGA version = 0x%02X"` in [`PROTOCOL.md`](../PROTOCOL.md)); `18` writes and `6` reads to `0x10` in `gc550_init_blob` | `Strongly supported` |
| **Analog Devices ADV7604** | I2C `0x20` (IO), `0x22` (CP), `0x26` (AFE), `0x32` (REP), `0x34` (HDMI), `0x36` (EDID), `0x46` (InfoFrame) | 4-input HDMI + analog component video/audio receiver. Extracts TMDS video, decodes HDMI audio and InfoFrames, hosts internal 256-byte EDID RAM. | `CRXADV7604::*` (118 methods incl. `ADIAPI_*`/`HAL_*` in [`PROTOCOL.md`](../PROTOCOL.md)); sub-map programming in `IO[0xF3..0xFE]` | `Confirmed` |
| **Analog Devices ADV7511** | I2C `0x3D` (`GC550_I2C_ADV7511_TX`), `0x38` (packet memory) | HDMI transmitter driving the GC550's HDMI passthrough output port. | `493` writes and `685` reads to `0x3D` plus `1` 31-byte write to `0x38` (`reg 0xC0`) in `gc550_init_blob`; matches `adv7511_fixed_registers` in [`PROTOCOL.md`](../PROTOCOL.md) | `Strongly supported` |
| **TI TLV320AIC3x** | I2C `0x18` (`GC550_I2C_TLV320AIC3X`) | Stereo analog audio codec for the 3.5 mm analog audio ports on the GC550. | `16` writes to `0x18` (`steps 52..67` in `gc550_init_blob`: PLL `0x03=0x91, 0x04=0x10`, serial interface `0x08=0xC0, 0x09=0x30`, ADC/line-in routing `0x0F..0x18`) | `Strongly supported` |
| **NXP PCA9539A** | I2C `0x74` (`GC550_I2C_PCA9539A`) | 16-bit I2C GPIO expander controlling board-level chip resets (including bit 6 reset pulse on `0x02`) and power/routing lines. | `CPCA9539A::*` RTTI; output regs `0x02`/`0x03` and direction regs `0x06`/`0x07` (`64` writes, `68` reads in `gc550_init_blob`) | `Confirmed` |
| **Atmel AT88SC CryptoMemory** | FX3 opcodes `0x05`, `0x06`, `0x07`, `0x09` (command bytes `0xB2`, `0xB4`, `0xB6`, `0xB8`, `0xBA`) | Secure EEPROM used for anti-clone authentication (`CryptoCheck`, `GetCryptoSerialNumber`) by the Windows driver. | `CryptoAT88::*` RTTI in [`PROTOCOL.md`](../PROTOCOL.md); stripped from `gc550_init_blob` and blocked in `gc550_cmd_send()` ([`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c)) | `Confirmed` |
| **Unknown Slave `0x4A`** | I2C `0x4A` | Accessed with `125` writes (`regs 0x06..0x5F`, many set to `0x3F`) and `106` reads (`regs 0x00..0x03, 0x05..0x61, 0x7E..0x7F`) in `gc550_init_blob`. | Inferred in [`PROTOCOL.md`](../PROTOCOL.md) to be a probable audio mixer/volume chip (6-bit gain/mute registers); exact IC part number unknown. | `Inferred` / `Unknown` |
| **Unknown Slave `0x65`** | I2C `0x65` | Accessed with `4` writes in `gc550_init_blob` (`reg 0x94 = 0x6D` and `reg 0x18..0x1C = 04 01 20 23 8C` at `steps 18..19` and `1427..1428`). | Noted in [`PROTOCOL.md`](../PROTOCOL.md) as possibly related to `CVX8897`; exact IC identity and role unconfirmed. | `Unknown` |
| **Unknown Slave `0x3F`** | I2C `0x3F` | Read `8` times (`steps 2512..2519`), `32` bytes per read at offsets `0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0` (`256` bytes total). | Purpose and IC identity unknown. | `Unknown` |

---

## B. USB Architecture

### 1. Interface and Endpoint Layout

The GC550 exposes a single USB configuration with one vendor-specific interface (`bInterfaceNumber = 0`, `bAlternateSetting = 0`, `bInterfaceClass = 0xFF`) and six Bulk endpoints (`Confirmed` via [`PROTOCOL.md`](../PROTOCOL.md) line 912 and [`kernel/gc550.h`](../kernel/gc550.h)):

| Endpoint | Direction | Transfer Type | `wMaxPacketSize` (USB 3.0) | Role in GC550 & `gc550.ko` | Evidence Level |
|---|---|---|---|---|---|
| `EP 0x01 OUT` (`GC550_EP_CMD_OUT`) | Host -> Device | Bulk OUT | `1024` bytes (`bMaxBurst = 0`) | FX3 command request pipe (I2C read/write requests, FX3 bridge opcodes). | `Confirmed` |
| `EP 0x81 IN` (`GC550_EP_CMD_IN`) | Device -> Host | Bulk IN | `1024` bytes (`bMaxBurst = 0`) | FX3 command response pipe (returns read payload bytes for read commands). | `Confirmed` |
| `EP 0x02 OUT` | Host -> Device | Bulk OUT | `1024` bytes (`bMaxBurst = 0`) | Unused by the single-interface capture/command path in both Windows (`FUN_1400196e0`) and `gc550.ko`. | `Confirmed` |
| `EP 0x82 IN` | Device -> Host | Bulk IN | `1024` bytes (`bMaxBurst = 15`) | Unused by the single-interface capture/command path in both Windows (`FUN_1400196e0`) and `gc550.ko`. | `Confirmed` |
| `EP 0x03 OUT` | Host -> Device | Bulk OUT | `1024` bytes (`bMaxBurst = 0`) | Unused during normal capture in both Windows and `gc550.ko`. | `Confirmed` |
| `EP 0x83 IN` (`GC550_EP_STREAM_IN`) | Device -> Host | Bulk IN | `1024` bytes (`bMaxBurst = 15`) | High-bandwidth capture stream endpoint carrying interleaved video (`SAV`/`EAV`), audio (`0x58`), and DMA padding (`AA 55 55 AA`). | `Confirmed` |

### 2. Command Channel vs. Capture Stream Channel

The GC550 separates control and capture traffic across two independent USB Bulk paths (`Confirmed`):

1. **Synchronous Command/Reply Pipe (`EP 0x01 OUT` / `EP 0x81 IN`)**:
   - Strictly host-initiated. Every command is sent as a Bulk OUT transfer on `EP 0x01 OUT` via `gc550_cmd_send()` in [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c).
   - Write-only commands (`0x01`, `0x08`, `0x09`, `0x0B`, `0x0D`, `0x14`, `0x15`, and CryptoMemory write opcodes `0x05`/`0x07`) produce **no** response packet on `EP 0x81 IN` (`rx_len == 0`).
   - Read commands (`0x02`, `0x0E`, `0x16`, and CryptoMemory read opcode `0x06`) queue a response payload of length `rx_expected` on `EP 0x81 IN`, which `gc550_cmd_xfer()` reads immediately after `gc550_cmd_send()`.
   - In `gc550.ko` ([`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) and [`kernel/gc550.h`](../kernel/gc550.h)), all `EP 0x01` / `EP 0x81` transactions are serialized under `dev->cmd_lock` using pre-allocated `1024`-byte (`GC550_CMD_MAX_PKT_SIZE`) DMA-safe heap buffers (`dev->cmd_out_buf` and `dev->cmd_in_buf`) with a `5000 ms` timeout (`GC550_CMD_TIMEOUT_MS = 5000`, matching the Windows driver's 5 s `FUN_1400157c4` timeout).
   - `gc550_cmd_xfer()` always submits a `1024`-byte (`GC550_CMD_MAX_PKT_SIZE`) Bulk IN read to avoid USB babble/overflow and then requires `actual_len == rx_expected` (`-EREMOTEIO` if shorter, `-EPROTO` if longer) before copying `rx_expected` bytes to the caller.

2. **Asynchronous Continuous Capture Stream Pipe (`EP 0x83 IN`)**:
   - Once bootstrapped, data flow on `EP 0x83 IN` is entirely device-driven (`Confirmed`). Neither the URB completion callback (`gc550_urb_complete()`) nor the stream parser issues any I2C command per frame.
   - **Transfer Sizes**:
     - The Windows driver (`avmu3_x64.sys`) and upstream userspace captures use `2,080,768`-byte (`0x1FC000` = `127 × 16,384` bytes) Bulk IN transfers, receiving one `1920×1080` frame period (`254 × 16,384 = 4,161,536` bytes) across two transfers.
     - The Linux kernel driver (`gc550.ko`, [`kernel/gc550.h`](../kernel/gc550.h) / [`kernel/gc550-video.c`](../kernel/gc550-video.c)) maintains a pool of `8` asynchronous Bulk IN URBs (`GC550_NUM_URBS = 8`), each sized at `512 KiB` (`GC550_URB_BUF_SIZE = 524,288` bytes = `32 × 16,384` bytes, an exact multiple of the `32 KiB` swap-pair size `GC550_PAIR_BUF_SIZE`).
   - **FX3 `16 KiB` Internal DMA Unit**:
     - Internally, the Cypress FX3 GPIF II engine transfers data in `16,384`-byte (`16 KiB`, `GC550_FX3_DMA_BUF_SIZE = 16 * 1024`) DMA buffers. This `16 KiB` unit governs both the `254 × 16 KiB` 1080p frame period and the `16 KiB` ping-pong DMA swap behaviour described in Section D.5 (`Confirmed`).

### 3. Why Concurrent Userspace Endpoint Access Breaks `gc550.ko`

USB Bulk IN endpoints (`EP 0x81 IN` and `EP 0x83 IN`) are **destructive hardware FIFOs**, not passive monitoring interfaces (`Confirmed`):

- **Command Reply Theft on `EP 0x81 IN`**: Any userspace process that reads `EP 0x81 IN` (for example, to drain responses or issue `0x02` I2C reads) while `gc550.ko` is active can consume a reply belonging to `gc550_cmd_xfer()` or leave an unread reply queued in `EP 0x81 IN`, causing subsequent kernel command reads to fail with length mismatches (`-EREMOTEIO` / `-EPROTO`) or timeouts.
- **Stream Data Theft on `EP 0x83 IN`**: When `gc550.ko` has URBs queued on `EP 0x83 IN`, any concurrent userspace Bulk IN read steals arbitrary byte chunks out of the stream, breaking `SAV`/`EAV` marker reassembly, `32 KiB` swap-pair alignment, and `audio_rem` byte counting.
- **Interface Ownership Conflict (`libusb_claim_interface(0)`)**: Because `EP 0x01`, `EP 0x81`, and `EP 0x83` all belong to USB Interface `0`, claiming Interface `0` from userspace conflicts directly with kernel-driver ownership.

---

## C. FX3 Command Protocol

### 1. Command Dispatcher and Verified Packet Formats

In the Windows driver (`avmu3_x64.sys`), all Bulk command transfers pass through the bridge command dispatcher (`FUN_1400199cc` in [`PROTOCOL.md`](../PROTOCOL.md) line 760), which accepts opcodes `0x01..0x10`, `0x14`, `0x15`, `0x16`, `0x29`, and `0x2A`.

#### Exact Source-Verified Wire Formats for `0x01` (I2C Write) and `0x02` (I2C Read)

Both [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) (`gc550_i2c_write()` at lines 145–166, `gc550_i2c_read()` at lines 177–197), all `6,574` I2C entries in [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h), [`PROTOCOL.md`](../PROTOCOL.md) (lines 415–418), and the Stage 4A-1 offline validator (`validate_init_table()` in `gc550_stage4a_probe.py`) confirm the exact byte layouts below:

1. **Opcode `0x01` — I2C Write (`GC550_OP_I2C_WRITE`)**:
   ```
   Byte 0:        0x01          (GC550_OP_I2C_WRITE)
   Byte 1:        slave_addr    (7-bit I2C slave address)
   Byte 2:        0x01          (reg_addr_len = 1-byte register address; always 0x01)
   Byte 3:        reg           (8-bit register offset)
   Byte 4..3+len: data[0..len-1] (write payload, 1..32 bytes)
   ```
   - **Total `EP 0x01 OUT` Request Length (`tx_len`)**: `4 + len` bytes = **`5..36` bytes** for `1..32` data bytes (`tx_len` values observed in `gc550_init_blob`: `{5, 6, 7, 8, 9, 14, 17, 28, 35, 36}`, corresponding to payload lengths `{1, 2, 3, 4, 5, 10, 13, 24, 31, 32}`). There is **no** 16-bit data-length field in an `0x01` packet; byte `2` (`0x01`) is the register-address width, and the write payload length is `tx_len - 4`.
   - **`EP 0x81 IN` Response Length (`rx_len`)**: `0` bytes (write-only).

2. **Opcode `0x02` — I2C Read (`GC550_OP_I2C_READ`)**:
   ```
   Byte 0:        0x02          (GC550_OP_I2C_READ)
   Byte 1:        slave_addr    (7-bit I2C slave address)
   Byte 2:        0x01          (reg_addr_len = 1-byte register address; always 0x01)
   Byte 3:        read_len      (number of bytes to read, 1..64; equals rx_len)
   Byte 4:        reg           (8-bit register offset)
   ```
   - **Total `EP 0x01 OUT` Request Length (`tx_len`)**: Always **`5` bytes** (`sizeof(pkt) == 5` in `gc550_i2c_read()`). Note that byte `2` is `0x01` (register-address length), byte `3` is the 1-byte `read_len`, and byte `4` is the target register offset `reg`.
   - **`EP 0x81 IN` Response Length (`rx_len`)**: `read_len` bytes (`1..64` supported in `gc550_i2c_read()`; observed `rx_len` values across the `4,721` read steps in `gc550_init_blob`: `{1, 2, 3, 5, 10, 13, 28, 32}`).

#### Complete FX3 Opcode Reference Table

| Opcode | Constant in `kernel/gc550.h` | Request Format (`EP 0x01 OUT`) | Response Format (`EP 0x81 IN`) | Status in `gc550.ko` | Evidence Level |
|---|---|---|---|---|---|
| `0x01` | `GC550_OP_I2C_WRITE` | `[0x01, slave_addr, 0x01, reg, data[0..len-1]]` (`tx_len = 5..36` B for `1..32` data B) | None (`0` B) | Implemented (`gc550_i2c_write` / `gc550_i2c_write_byte`) & replayed (`1,853` steps in `gc550_init_blob`) | `Confirmed` |
| `0x02` | `GC550_OP_I2C_READ` | `[0x02, slave_addr, 0x01, read_len, reg]` (`tx_len = 5` B) | `rx_data[0..read_len-1]` (`1..64` B; `1..32` B in `gc550_init_blob`) | Implemented (`gc550_i2c_read` / `gc550_i2c_read_byte`) & replayed (`4,721` steps in `gc550_init_blob`) | `Confirmed` |
| `0x05` | `GC550_OP_CRYPTO_HDR` | `[0x05, cmd_byte, addr1, addr2, len]` (CryptoMemory 2-wire command header write; `cmd_byte` in `0xB2..0xBA`, e.g. `05 B6 00 <addr> 01`) | None (`0` B) | **Explicitly blocked** in `gc550_cmd_send()` (`-EPERM`) | `Confirmed` |
| `0x06` | `GC550_OP_CRYPTO_READ` | `[0x06, len]` (CryptoMemory 2-wire read request, e.g. `06 01`) | `rx_data[0..len-1]` (`len` B) | **Explicitly blocked** in `gc550_cmd_send()` (`-EPERM`) | `Confirmed` |
| `0x07` | `GC550_OP_CRYPTO_DATA` | `[0x07, data...]` (CryptoMemory 2-wire data write, e.g. challenge/password payload) | None (`0` B) | **Explicitly blocked** in `gc550_cmd_send()` (`-EPERM`) | `Confirmed` |
| `0x08` | `GC550_OP_TRIG_08` | `[0x08]` (`tx_len = 1` B) | None (`0` B) | Replayed once at Step `1` of `gc550_init_blob`; internal FX3 action untraced | `Confirmed` (wire format) / `Unknown` (FX3 internal logic) |
| `0x09` | *(literal `0x09` in `gc550_cmd_send()`)* | `[0x09, val]` (`tx_len = 2` B, e.g. `09 0A` after CryptoMemory writes; probable write-cycle delay) | None (`0` B) | **Explicitly blocked** in `gc550_cmd_send()` (`-EPERM`) | `Confirmed` |
| `0x0B` | *(not defined)* | `[0x0B, a, b, c]` (`tx_len = 4` B short write in `avmu3_x64.sys`) | None (`0` B) | Not used in `gc550.ko` | `Confirmed` (dispatcher) / `Unknown` (function) |
| `0x0D` | `GC550_OP_EEPROM_WRITE` | `[0x0D, a, b, c, len_hi, len_lo, data...]` (memory/EEPROM write matching `0x0E`) | None (`0` B) | Defined in `gc550.h`; not used in `gc550.ko` | `Strongly supported` |
| `0x0E` | `GC550_OP_EEPROM_READ` | `[0x0E, a, b, c, len_hi, len_lo]` (`tx_len = 6` B; reads `len_hi * 256 + len_lo` B) | `rx_data[0..len-1]` (`512` B at Step `0` of `gc550_init_blob`) | Replayed once at Step `0` of `gc550_init_blob` (`0e 02 07 fe 02 00` -> `512` B) | `Confirmed` (wire format) / `Inferred` (board/config EEPROM target) |
| `0x14` | `GC550_OP_STREAM_ARM` | `[0x14]` (`tx_len = 1` B) | None (`0` B) | Issued once mid-stream on GC551 (`cmd #7732` in [`PROTOCOL.md`](../PROTOCOL.md)); absent from `gc550_init_blob` | `Confirmed` (wire format) / `Unknown` (exact FX3 action) |
| `0x15, 0x16` | *(not defined)* | Single-byte variants of `0x0D` / `0x0E` in `avmu3_x64.sys` | `0` B (`0x15`) / `1` B (`0x16`) | Not used in `gc550.ko` | `Confirmed` (dispatcher) / `Unknown` (target) |
| `0x03, 0x04, 0x0A, 0x0C, 0x0F, 0x10, 0x29, 0x2A` | *(not defined)* | Accepted by Windows dispatcher `FUN_1400199cc` | Varies | Not observed in GC550/GC551 USB captures; not used in `gc550.ko` | `Unknown` |

### 2. ADV7604 Sub-Map Address Registers (`IO[0xF3..0xFE]`) and I2C Representation

All I2C slave addresses in opcodes `0x01` and `0x02` use **7-bit notation**, whereas the ADV7604 sub-map address registers (`IO[0xF3..0xFE]` on the main `IO` map at 7-bit `0x20`) store programmable sub-map addresses in **8-bit left-shifted notation** (`addr_8bit = addr_7bit << 1`).

In `gc550_init_blob` ([`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h)), the initialization sequence programs all twelve ADV7604 sub-map address registers twice—first at `steps 78..101` (lines 76–88) after the first `PCA9539A` reset pulse, and identically at `steps 350..373` (lines 228–239) after the second `PCA9539A` reset pulse. Cross-referencing `gc550_init_blob` against the Analog Devices ADV7604 register map, `enum adv76xx_page` in `<media/i2c/adv7604.h>`, and [`PROTOCOL.md`](../PROTOCOL.md) (lines 439–440, 576) yields the complete register-to-map reconstruction (`Confirmed`):

| ADV7604 `IO` (`0x20`) Register | ADV7604 Sub-Map (`enum adv76xx_page`) | Programmed 8-Bit Value in `gc550_init_blob` | 7-Bit FX3 `slave_addr` (`val >> 1`) | Constant in `kernel/gc550.h` | Activity in `gc550_init_blob` | Evidence Level |
|---|---|---|---|---|---|---|
| Hardware strapped (`ALSB`) | Main IO Map (`ADV76XX_PAGE_IO`) | `0x40` (fixed) | `0x20` | `GC550_I2C_ADV7604_IO` | `1,004` writes, `1,393` reads | `Confirmed` |
| `IO[0xF3]` | AVLink Map (`ADV7604_PAGE_AVLINK`) | `0x84` (`steps 101, 373`) | `0x42` | *(not defined)* | `0` writes, `0` reads | `Confirmed` |
| `IO[0xF4]` | CEC Map (`ADV76XX_PAGE_CEC`) | `0x80` (`steps 79, 351`) | `0x40` | *(not defined)* | `0` writes, `0` reads | `Confirmed` |
| `IO[0xF5]` | **InfoFrame Map** (`ADV76XX_PAGE_INFOFRAME`) | **`0x8C`** (`steps 81, 353`) *(vs. `0x7C` on GC551)* | **`0x46`** *(vs. `0x3E` on GC551)* | `GC550_I2C_ADV7604_INFO` | `0` writes, **`1,432` reads** | `Confirmed` |
| `IO[0xF6]` | ESDP Map (`ADV7604_PAGE_ESDP`) | `0x70` (`steps 83, 355`) | `0x38` | *(not defined; ESDP powered down via `IO[0x0B]=0x44`)* | *(see note on `0x38` below)* | `Confirmed` |
| `IO[0xF7]` | DPP Map (`ADV7604_PAGE_DPP`) | `0x78` (`steps 85, 357`) | `0x3C` | *(not defined)* | `0` writes, `0` reads | `Confirmed` |
| `IO[0xF8]` | **AFE / DPLL Map** (`ADV76XX_PAGE_AFE`) | **`0x4C`** (`steps 87, 359`) | **`0x26`** | `GC550_I2C_ADV7604_AFE` | `7` writes, `3` reads | `Confirmed` |
| `IO[0xF9]` | **Repeater / KSV Map** (`ADV76XX_PAGE_REP`) | **`0x64`** (`steps 89, 361`) | **`0x32`** | `GC550_I2C_ADV7604_REP` | `14` writes, `14` reads | `Confirmed` |
| `IO[0xFA]` | **EDID RAM Map** (`ADV76XX_PAGE_EDID`) | **`0x6C`** (`steps 91, 363`) | **`0x36`** | `GC550_I2C_ADV7604_EDID` | `10` writes, `0` reads | `Confirmed` |
| `IO[0xFB]` | **HDMI RX Map** (`ADV76XX_PAGE_HDMI`) | **`0x68`** (`steps 93, 365`) | **`0x34`** | `GC550_I2C_ADV7604_HDMI` | `50` writes, `612` reads | `Confirmed` |
| `IO[0xFC]` | Test Map (`ADV76XX_PAGE_TEST`) | `0x52` (`steps 95, 367`) | `0x29` | *(not defined)* | `0` writes, `0` reads | `Confirmed` |
| `IO[0xFD]` | **Component Processor (CP) Map** (`ADV76XX_PAGE_CP`) | **`0x44`** (`steps 97, 369`) | **`0x22`** | `GC550_I2C_ADV7604_CP` | `47` writes, `394` reads | `Confirmed` |
| `IO[0xFE]` | VDP Map (`ADV7604_PAGE_VDP`) | `0x48` (`steps 99, 371`) | `0x24` | *(not defined; VDP powered down via `IO[0x0C]=0x42`)* | `0` writes, `0` reads | `Confirmed` |

Key verifications from this reconstruction (`Confirmed`):

1. **Verification of `IO[0xF8]`, `IO[0xF9]`, `IO[0xFA]`, and `IO[0xFB]`**:
   - `IO[0xF8]` is the **AFE / DPLL Map** address register, programmed to `0x4C` (7-bit `0x26`, `GC550_I2C_ADV7604_AFE`).
   - `IO[0xF9]` is the **Repeater / KSV Map** address register, programmed to `0x64` (7-bit `0x32`, `GC550_I2C_ADV7604_REP`).
   - `IO[0xFA]` is the **EDID RAM Map** address register, programmed to `0x6C` (7-bit `0x36`, `GC550_I2C_ADV7604_EDID`).
   - `IO[0xFB]` is the **HDMI RX Map** address register, programmed to `0x68` (7-bit `0x34`, `GC550_I2C_ADV7604_HDMI`).
2. **Identity of I2C Target Address `0x46`**:
   - On the GC550, `IO[0xF5]` (`ADV76XX_PAGE_INFOFRAME`) is programmed directly to `0x8C` (`0x46 << 1`) at `steps 81` and `353` (whereas the GC551 programs `IO[0xF5] = 0x7C` = `0x3E << 1`).
   - Thus, 7-bit I2C slave `0x46` (`GC550_I2C_ADV7604_INFO`) is unequivocally the **ADV7604 InfoFrame Map**. The `1,432` read transactions to `0x46` in `gc550_init_blob` poll AVI InfoFrame bytes (`0x00..0x1B`), Audio InfoFrame bytes (`0x1D`, 10 bytes), packet bytes (`0x54`, 28 bytes), and InfoFrame header/version/checksum registers (`0xE0..0xE3`, `0xEC`).
3. **Verification of `gc550_detect_board_state()` in [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c)**:
   - `gc550_detect_board_state()` (lines 212–240 of [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c)) reads `ADV7604 IO[0xFA]` (`gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0xfa, &edid_map)`) and checks `edid_map == 0x6c`, alongside reading `2` bytes from `PCA9539A` (`0x74`) offset `0x06` (`0x06..0x07`) and checking `gpio_cfg[0] == 0x00 && gpio_cfg[1] == 0x09`.
   - Because `IO[0xFA]` is indeed the ADV7604 EDID RAM Map address register (power-on reset default `0x00`, programmed to `0x6C` at `steps 91` and `363`), `gc550_detect_board_state()` checks the **correct** register (`IO[0xFA]`) and **correct** expected value (`0x6C`). (See Section E.3 for the scope and limitations of this warm-state heuristic.)

### 3. CryptoMemory Protection and Scope Limitations

The Atmel AT88SC CryptoMemory IC has a restricted hardware retry counter (`FF -> EE -> CC -> 88 -> 00`) that permanently locks authentication key sets if recorded challenges (`0xB8`/`0xBA`) are replayed (`Confirmed` in [`PROTOCOL.md`](../PROTOCOL.md) lines 525–555).

- In [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) (lines 77–85), `gc550_cmd_send()` enforces a runtime gate rejecting top-level opcodes `GC550_OP_CRYPTO_HDR` (`0x05`), `GC550_OP_CRYPTO_READ` (`0x06`), `GC550_OP_CRYPTO_DATA` (`0x07`), and `0x09` with `-EPERM`, and `gc550_init_blob` has been verified offline to contain zero instances of `0x05`, `0x06`, `0x07`, or `0x09` (`Confirmed`).
- **Important Boundary on This Guarantee (`Unknown` / Unresolved)**: Blocking top-level USB opcodes `0x05`, `0x06`, `0x07`, and `0x09` prevents the host from sending the known direct CryptoMemory 2-wire transport commands used by `CryptoAT88::*`. However, because the Cypress FX3 internal ARM9 firmware binary has not been dumped or disassembled, it is **not** proven that no other FX3 opcode (`0x08`, `0x0E`, `0x0B`, `0x15`, `0x16`, etc.) or internal firmware code path could ever interact with the CryptoMemory bus.

---

## D. Capture Stream Protocol (`EP 0x83 IN`)

### 1. Stream Framing and `00 FF FF` Marker Prefix

All framing markers on `EP 0x83 IN` begin with the 3-byte prefix `00 FF FF` followed by a 1-byte marker type `T` (`Confirmed`):

| Marker Byte Sequence | Type Byte (`T`) | Constant in `kernel/gc550.h` | Meaning | Header Bytes After `00 FF FF T` | Evidence Level |
|---|---|---|---|---|---|
| `00 FF FF C0` | `0xC0` | `GC550_MARKER_SAV` | **Start of Active Video (SAV)** — Opens a raw YUYV video frame. | `4` bytes (`b0, b1, b2, b3`) | `Confirmed` |
| `00 FF FF C1` | `0xC1` | `GC550_MARKER_EAV` | **End of Active Video (EAV)** — Closes the current raw YUYV video frame. | `5` bytes (`c0, c1, c2, c3, c4`) | `Confirmed` |
| `00 FF FF 58` | `0x58` (`'X'`) | `GC550_MARKER_AUDIO` | **Audio Packet** — Opens a length-prefixed audio payload packet (sample format and sample semantics under verification). | `4` bytes (`L3, L2, L1, L0`, big-endian `u32`) | `Confirmed` |

#### Origin of the `00 FF FF` Prefix and Windows 4-Byte vs. 5-Byte Scanner Forms

- **Hardware Origin (`Strongly supported` from [`PROTOCOL.md`](../PROTOCOL.md) lines 24–42, 100–106)**: On the 8-bit dual-channel BT.1120 bus (`ADV7604 IO[0x03] = 0x80`, `IO[0x05] = 0x2C`), each SAV/EAV timing reference preamble begins with `3FF 000 000 XY` (`0xFF` in the upper 8 bits). The FPGA/FX3 packs the dual-channel preamble and packet-type indicator (`0xC0`, `0xC1`, `0x58`) into the `00 FF FF T` wire sequence (`0xTFFFF00` as a little-endian `u32`).
- **Windows Driver (`FUN_1400a6fa4`) 4-Byte and 5-Byte Forms (`Confirmed` from [`PROTOCOL.md`](../PROTOCOL.md) lines 85–94)**:
  - **4-byte form**: `00 FF FF T` where `T` is in `{0xC0, 0xC1, 0x58}`.
  - **5-byte form**: `X 00 FF FF T` where `X` is any leading byte absorbed and consumed along with the marker.
- **Linux Kernel Parser (`gc550_parse_ordered_bytes()` in [`kernel/gc550-video.c`](../kernel/gc550-video.c)) (`Confirmed`)**:
  - Uses a 4-stage byte-level prefix state machine (`p->marker_stage` `0..3`) that tracks partial `[00]`, `[00 FF]`, and `[00 FF FF]` sequences across arbitrary URB boundaries and flushes `prefix_bytes` (`{0x00, 0xFF, 0xFF}`) back into the video buffer if a candidate prefix in `GC550_STATE_VIDEO_DATA` is not followed by a valid marker type (`gc550_is_marker_type()`).

### 2. Header Structures and Checksum Validation

#### SAV Header (`00 FF FF C0` + 4 Bytes: `h[0..3]`)

- **Structure**: `4` bytes immediately following `00 FF FF C0` (`Confirmed`, `FUN_1400a8c60` in [`PROTOCOL.md`](../PROTOCOL.md) line 128 and `gc550_sav_hdr_valid()` in [`kernel/gc550-video.c`](../kernel/gc550-video.c) lines 78–83).
- **Frame Sequence Counter**: `h[0]` (`p->sav_b0`) is an 8-bit frame sequence counter compared against `h[0]` (`eav_c0`) of the closing `EAV` header.
- **Checksum Formula (`Confirmed`)**:
  ```c
  u8 expected = (u8)((h[0] + h[1] + h[2] - 0x40) & 0xff);
  return h[3] == expected;
  ```

#### EAV Header (`00 FF FF C1` + 5 Bytes: `h[0..4]`)

- **Structure**: `5` bytes immediately following `00 FF FF C1` (`Confirmed`, `FUN_1400a8b34` in [`PROTOCOL.md`](../PROTOCOL.md) line 138 and `gc550_eav_hdr_valid()` in [`kernel/gc550-video.c`](../kernel/gc550-video.c) lines 85–90).
- **Frame Sequence Counter Match**: For a properly ordered frame, `h[0]` (`eav_c0`) equals the opening `SAV` header's `sav_b0`. In `gc550_parse_ordered_bytes()` (line 254), `if (p->hdr_buf[0] != p->sav_b0) p->desync_count++;`.
- **Checksum Formula (`Confirmed`)**:
  ```c
  u8 expected = (u8)((h[0] + h[1] + h[2] + h[3] - 0x3f) & 0xff);
  return h[4] == expected;
  ```

#### AUDIO Header (`00 FF FF 58` + 4 Bytes: `h[0..3]`) and Post-Audio DMA Filler

- **Packet Structure (`Confirmed` via [`PROTOCOL.md`](../PROTOCOL.md) lines 141, 178, 848–852 and [`kernel/gc550-video.c`](../kernel/gc550-video.c) lines 214–217, 266–288)**:
  - **Audio Marker**: `00 FF FF 58` (`GC550_MARKER_AUDIO`, 4 bytes).
  - **Payload Length Header**: `4` bytes immediately following `00 FF FF 58`, encoding a **32-bit big-endian** payload byte count (`p->audio_rem = get_unaligned_be32(p->hdr_buf)`).
  - **Audio Payload**: `p->audio_rem` bytes of audio packet payload.
  - **Post-Audio DMA Filler**: `AA 55 55 AA` repeating pattern padding the remainder of the FX3 `16 KiB` DMA buffer window after the audio payload until the next frame's `00 FF FF C0` (`SAV`) marker.
- **Sanity Bounds in `gc550.ko` (`Confirmed`)**:
  - Accepted in [`kernel/gc550-video.c`](../kernel/gc550-video.c) (line 270) iff `p->audio_rem > 0 && p->audio_rem <= 65536`. Otherwise, `p->state` returns to `GC550_STATE_IDLE` (without incrementing `p->desync_count`).
- **Parser Processing and `audio_bytes` Exclusion Rules (`Confirmed` in [`kernel/gc550-video.c`](../kernel/gc550-video.c) lines 214–217, 266–288, 357–365)**:
  - When `gc550_parse_ordered_bytes()` matches `00 FF FF 58`, `gc550_handle_marker()` transitions to `GC550_STATE_AUDIO_HEADER` without counting the 4-byte marker in `p->audio_bytes`.
  - In `GC550_STATE_AUDIO_HEADER`, the 4-byte big-endian length header is collected into `p->hdr_buf[0..3]` and decoded into `p->audio_rem` without counting those 4 bytes in `p->audio_bytes`.
  - In `GC550_STATE_AUDIO_DATA`, `gc550_parse_ordered_bytes()` computes `take = min_t(size_t, len - i, p->audio_rem)`, adds `p->audio_bytes += take`, and passes the exact same byte slice (`&data[i]`, `take`) to `gc550_audio_push_samples(dev, &data[i], take)`. When `p->audio_rem` reaches `0`, the parser transitions to `GC550_STATE_IDLE`, where all trailing `AA 55 55 AA` DMA filler bytes are skipped while scanning for the next `0x00` marker prefix.
  - Consequently, `p->audio_bytes` excludes the 4-byte `00 FF FF 58` marker, the 4-byte big-endian length field, and the trailing `AA 55 55 AA` filler.
- **Observed Payload Sizes — Titan GC550 Measurements vs. Upstream `PROTOCOL.md` (`Confirmed`)**:
  - **Measured on Titan (GC550 `07ca:4710` at `1080p60` in Phases B2 and C1–C3)**: Approximately `3,200` payload bytes per video frame (`~192,000` bytes/s; see Section F.4 below).
  - **Documented by Upstream Reverse Engineering ([`PROTOCOL.md`](../PROTOCOL.md) lines 178, 848, 957 on GC551)**: `3,200` bytes per video frame at `1080p60` and `3,840` bytes per video frame at `1080p50`, both corresponding to the same nominal `192,000` bytes/s (`800` or `960` four-byte units per frame if interpreted as stereo `S16_LE` frames; actual GC550 sample format and sample semantics under verification).

### 3. Video Payload Format and Interleaving Structure

Within each video frame period on `EP 0x83 IN`, packets are interleaved in the following order (`Confirmed`):

```
+---------------------------------------------------------------------------------------------------+
| 00 FF FF C0 | b0 b1 b2 b3 | 00 00 10 80 | Active YUYV Pixels (W * H * 2 B = sizeimage)            |
| (SAV, 4 B)  | (SAV Hdr)   | (1st Word)  | (Limited-Range YUYV 4:2:2, Rec. 709)                    |
+---------------------------------------------------------------------------------------------------+
| 00 FF FF C1 | c0 c1 c2 c3 c4 | 00 FF FF 58 | L3 L2 L1 L0 (BE32) | Audio payload bytes (sample     |
| (EAV, 4 B)  | (EAV Hdr, 5 B) | (AUDIO, 4B) | (Audio Len, 4 B)   | format & semantics under verif.)|
+---------------------------------------------------------------------------------------------------+
| AA 55 55 AA AA 55 55 AA ... (DMA filler pattern padding out the remainder of the 254 x 16 KiB     |
|                              frame period until the next frame's 00 FF FF C0 SAV marker)          |
+---------------------------------------------------------------------------------------------------+
```

Key payload properties (`Confirmed`):

1. **Pixel Format and Colorimetry in `gc550.ko`**:
   - `gc550_vidioc_try_fmt_vid_cap()` and `gc550_video_register()` in [`kernel/gc550-video.c`](../kernel/gc550-video.c) (lines 717–736, 864–874) expose `V4L2_PIX_FMT_YUYV` (`2` bytes/pixel) and unconditionally report `V4L2_COLORSPACE_REC709`, `V4L2_YCBCR_ENC_709`, `V4L2_QUANTIZATION_LIM_RANGE`, and `V4L2_XFER_FUNC_709` for all five discrete frame sizes.
2. **Leading 4-Byte Word (`00 00 10 80` = `0x80100000` LE)**:
   - As documented in [`PROTOCOL.md`](../PROTOCOL.md) (lines 150–165, 1010–1017), the first 4-byte data word immediately after the 4-byte `SAV` header begins with `00 00` (`Y0=0, Cb=0`, below-black vertical blanking/overscan artefact) followed by `10 80` (`Y1=16, Cr=128`), and is copied verbatim into the frame buffer by both the Windows and Linux drivers.
3. **Measured `SAV -> EAV` Video Spans (`Confirmed` via [`PROTOCOL.md`](../PROTOCOL.md), [`CHANGELOG.md`](../CHANGELOG.md), and [`TODO.md`](../TODO.md))**:
   - In the upstream 1080p60 capture ([`PROTOCOL.md`](../PROTOCOL.md) lines 177, 846, 878), each un-swapped `SAV -> EAV` span is `4,147,200` bytes (`1920 × 1080 × 2`).
   - In a real-hardware GC550 `SAV -> EAV` span histogram on **Niri** (`7.3.1-1-cachyos`) across `55,655` candidates before and after a physical USB reconnect ([`CHANGELOG.md`](../CHANGELOG.md) line 36; [`TODO.md`](../TODO.md) line 19), `55,654` candidates fell in the `4,147,200..4,147,455` byte bin and `1` post-reconnect candidate fell below the `4,130,816`-byte lower acceptance limit and was rejected.
   - Across isolated multi-resolution V4L2 tests on **Niri** with a Raspberry Pi 3B ([`CHANGELOG.md`](../CHANGELOG.md) line 17; [`TODO.md`](../TODO.md) line 57), the observed `SAV -> EAV` spans matched the exact active `sizeimage` (`W × H × 2`) for all five resolutions: `4,147,200` B (`1920×1080p60`), `1,843,200` B (`1280×720p60`), `829,440` B (`720×576p50`), `691,200` B (`720×480p59.94`), and `614,400` B (`640×480p60`).
4. **Role of `AA 55 55 AA` (`Confirmed` in [`PROTOCOL.md`](../PROTOCOL.md) lines 95–98, 849–852)**:
   - `AA 55 55 AA` is never scanned for by either `avmu3_x64.sys` or `gc550.ko`; it is post-audio DMA padding (`11,108` bytes at 1080p60) up to the next `16 KiB` buffer boundary / `SAV` marker.

### 4. Supported Video Frame Sizes and Runtime Validation Status in `gc550.ko`

In [`kernel/gc550-video.c`](../kernel/gc550-video.c) (lines 45–51), `gc550_frame_sizes[]` defines five discrete `V4L2_PIX_FMT_YUYV` capture resolutions registered by `gc550_video_register()`. Note that `gc550_vidioc_enum_frameintervals()` and `gc550_vidioc_g_parm()` (lines 764–811) currently report a fixed `1/60` s (`60 fps`) frame interval for every frame size (`Confirmed`):

| `gc550_frame_sizes[]` Entry | Active Frame Size (`sizeimage` = `W × H × 2`) | V4L2 Reported Interval | Recorded Runtime Validation Status (`CHANGELOG.md`, `README.md`, `TODO.md`) | Evidence Level |
|---|---|---|---|---|
| `1920 × 1080` (`GC550_DEFAULT_WIDTH × GC550_DEFAULT_HEIGHT`) | `4,147,200` B | `1/60` s (`60 fps`) | **Endurance & functional verified on Niri** (`7.3.1-1-cachyos` Clang): **3.6+ hours** continuous `1080p60` (`776,295` frames, `0` desyncs, Nintendo Switch & Raspberry Pi 3B); `55,655`-frame span histogram across reconnect (`0` desyncs); `29,629`-frame (`v0.1.1`) and `17,227`-frame (`v0.1.2`) active OBS hot-unplug tests (`0` desyncs); `120`-frame `v0.1.3` test (`0` desyncs). | `Confirmed` |
| `1280 × 720` | `1,843,200` B | `1/60` s (`60 fps`) | **Short functional test verified on Niri** (`7.3.1-1-cachyos`, Raspberry Pi 3B at `720p60`): `120` V4L2 frames with exact `1,843,200` B span, `0` desyncs, plus manual OBS format selection. No long-duration endurance test recorded. | `Confirmed` |
| `720 × 576` | `829,440` B | `1/60` s (`60 fps`) | **Short functional test verified on Niri** (`7.3.1-1-cachyos`, Raspberry Pi 3B at `576p50`): `100` V4L2 frames with exact `829,440` B span, `0` desyncs, plus manual OBS format selection (observed at ~50 fps while V4L2 enumerates 60 fps). No long-duration endurance test recorded. | `Confirmed` |
| `720 × 480` | `691,200` B | `1/60` s (`60 fps`) | **Short functional test verified on Niri** (`7.3.1-1-cachyos`, Raspberry Pi 3B at `480p59.94`): `100` V4L2 frames with exact `691,200` B span, `0` desyncs, plus manual OBS format selection. No long-duration endurance test recorded. | `Confirmed` |
| `640 × 480` | `614,400` B | `1/60` s (`60 fps`) | **Short functional test verified on Niri** (`7.3.1-1-cachyos`, Raspberry Pi 3B at `640×480p60`): `100` V4L2 frames with exact `614,400` B span, `0` desyncs, plus manual OBS format selection. No long-duration endurance test recorded. | `Confirmed` |

> [!NOTE]
> Kernel `6.18.55-2-cachyos-lts` (GCC) was **DKMS build-verified only**; all recorded bare-metal runtime and hot-unplug tests above were executed on the **Niri** test laptop running CachyOS kernel `7.3.1-1-cachyos` (Clang), plus initial `1080p60` verification in a QEMU/KVM CachyOS VM ([`README.md`](../README.md) lines 5, 25; [`CHANGELOG.md`](../CHANGELOG.md) lines 34, 76). No `1080p30` test or 24-hour endurance test has been performed.

### 5. FX3 `16 KiB` Dual-Socket DMA Block Ordering, Kernel Parser, and Python Model Comparison

#### Why Swapped `16 KiB` DMA Pairs Produce `18,432`-Byte `SAV(N) -> EAV(N-1)` Spans

As documented in [`PROTOCOL.md`](../PROTOCOL.md) (lines 841–882) and [`kernel/gc550-video.c`](../kernel/gc550-video.c) (lines 26–31, 134–151), the FX3 streams `EP 0x83 IN` in `16,384`-byte (`16 KiB`, `GC550_FX3_DMA_BUF_SIZE`) buffers (`254` buffers = `4,161,536` bytes per `1080p` frame period). When the ping-pong DMA socket phase flips (for example, on warm restarts or signal interruptions), every `32,768`-byte (`32 KiB`, `GC550_PAIR_BUF_SIZE`) pair `[first 16 KiB, second 16 KiB]` arrives in swapped order `[second 16 KiB, first 16 KiB]`:

- `SAV(N)` (8 bytes at the start of a `16 KiB` buffer) is followed by `16,376` bytes of frame `N`'s start, and then immediately by the swapped partner buffer containing the final `2,056` video bytes of frame `N-1` and `EAV(N-1)` (`16,376 + 2,056 = 18,432` bytes).
- Consequently, every `SAV -> EAV` span measures `18,432` bytes (`<= 32,768`) with `eav_c0 != sav_b0` (`Confirmed`).

#### Exact Comparison: `kernel/gc550-video.c` vs. Stage 4A-1 Offline Python `StreamParser`

The table below compares the current C implementation in [`kernel/gc550-video.c`](../kernel/gc550-video.c) and [`kernel/gc550.h`](../kernel/gc550.h) against the offline Python model (`StreamParser` in `scratch/gc550_stage4a_probe.py`). The two implementations are **not** identical; all differences are explicitly documented below (`Confirmed`):

| Feature / Mechanism | Kernel Implementation (`kernel/gc550-video.c`, `kernel/gc550.h`) | Stage 4A-1 Offline Python Model (`StreamParser` in `gc550_stage4a_probe.py`) | Equivalence / Difference |
|---|---|---|---|
| **Top-Level Entry & `32 KiB` Swap Reordering** | `gc550_parse_stream_chunk()`: when `!p->swap_mode || !p->pair_buf`, calls `gc550_parse_ordered_bytes(dev, data, len)` directly. When `p->swap_mode` is `true`, accumulates `32,768` bytes (`GC550_PAIR_BUF_SIZE`) in `p->pair_buf` (`p->pair_pos`) and parses `[16384..32768)` then `[0..16384)`. | `parse_stream_chunk()`: when `not self.swap_mode`, calls `parse_ordered_bytes(chunk, 0, len(chunk))` directly. When `self.swap_mode` is `True`, accumulates `32,768` bytes (`PAIR_BUF_SIZE`) in `self.pair_buf` (`self.pair_pos`) and parses `[16384..32768)` then `[0..16384)`. | **Matches** in control flow and `32 KiB` pair reordering. |
| **Swap Detection (`swap_votes` & `swap_mode`)** | In `gc550_complete_video_frame()` (lines 139–153): if `p->video_pos <= GC550_SWAP_SPAN_LIMIT` (`32,768`) and `eav_c0 != p->sav_b0`, increments `p->swap_votes`. At `p->swap_votes >= 2`, flips `p->swap_mode = !p->swap_mode`, sets `p->swap_votes = 0` and `p->pair_pos = 0`, logs via `dev_info_ratelimited()`, sets `p->video_pos = 0`, and returns. Otherwise sets `p->swap_votes = 0`. | In `complete_video_frame()` (lines 294–303): if `self.video_pos <= SWAP_SPAN_LIMIT` (`32,768`) and `eav_c0 != self.sav_b0`, increments `self.swap_votes`. At `self.swap_votes >= 2`, flips `self.swap_mode = not self.swap_mode`, sets `self.swap_votes = 0` and `self.pair_pos = 0`, sets `self.video_pos = 0`, and returns. Otherwise sets `self.swap_votes = 0`. | **Matches** in threshold (`<= 32,768` B), `eav_c0 != sav_b0` check, 2-vote toggle, and `pair_pos = 0` reset (Note: neither resets `marker_stage` or `state` on toggle because `gc550_complete_video_frame()` runs at `EAV` completion before `p->state = GC550_STATE_POST_EAV`). |
| **Frame-Span Acceptance Limits** | In `gc550_complete_video_frame()` (lines 159–163): always rejects frame if `p->video_pos < frame_size - GC550_FRAME_SPAN_TOLERANCE` or `p->video_pos > frame_size + GC550_FRAME_SPAN_TOLERANCE`, where `frame_size = dev->pix_fmt.sizeimage` and `GC550_FRAME_SPAN_TOLERANCE = 0x4000` (`16,384` B). | In `complete_video_frame()` (lines 305–311): checks `self.video_pos < self.frame_size - FRAME_SPAN_TOLERANCE` or `self.video_pos > self.frame_size + FRAME_SPAN_TOLERANCE` (`0x4000` = `16,384` B) **only when `self.enforce_frame_span` is `True`** (default `True`), and records rejected spans in `self.rejected_video_spans`. | **Differs**: Python adds an optional `enforce_frame_span` boolean and a `rejected_video_spans` `Counter`; kernel always enforces `frame_size ± 0x4000`. |
| **Span Counter Saturation (`GC550_MAX_FRAME_SPAN`)** | In `gc550_append_video_bytes()` (lines 120–122): `GC550_MAX_FRAME_SPAN = GC550_FRAME_SIZE + 0x4000` (`4,163,584` B). If `p->video_pos <= GC550_MAX_FRAME_SPAN`, clamps `p->video_pos` to `min(p->video_pos + len, GC550_MAX_FRAME_SPAN + 1)` (`4,163,585` B). | In `append_video_bytes()` (lines 289–290): `MAX_FRAME_SPAN = DEFAULT_FRAME_SIZE + 0x4000` (`4,163,584` B). If `self.video_pos <= MAX_FRAME_SPAN`, sets `self.video_pos = min(self.video_pos + length, MAX_FRAME_SPAN + 1)` (`4,163,585` B). | **Matches** saturation arithmetic (`4,163,585` B); however, the kernel function also copies up to `frame_size - p->video_pos` bytes into the `vb2` plane buffer (`memcpy`), whereas Python only updates `self.video_pos` and discards video bytes. |
| **Short-Frame Neutral YUYV Black Tail Padding** | In `gc550_complete_video_frame()` (lines 173–179): when `done_buf` is present and `p->video_pos < frame_size` (within `[frame_size - 0x4000, frame_size - 1]`), fills `vbuf[p->video_pos .. frame_size - 1]` with `gc550_yuyv_black[i & 3]` (`{16, 128, 16, 128}`). | **Not implemented** in `StreamParser.complete_video_frame()` because `StreamParser` does not maintain a frame pixel buffer. | **Differs**: Kernel performs `{16, 128, 16, 128}` tail fill on `vb2` memory; Python model has no pixel buffer or tail-fill logic. |
| **False-Prefix Flush in `VIDEO_DATA`** | In `gc550_parse_ordered_bytes()` (lines 329–331): calls `gc550_append_video_bytes(dev, prefix_bytes, p->marker_stage)` to copy the held-back `{0x00, 0xFF, 0xFF}` prefix bytes into `cur_buf` and advance `p->video_pos`. | In `parse_ordered_bytes()` (lines 436–437): calls `self.append_video_bytes(self.marker_stage)`, advancing `self.video_pos` by `1..3` without copying `prefix_bytes`. | **Differs**: Kernel writes actual `prefix_bytes` into `vb2` plane memory; Python only increments `video_pos`. |
| **`frame_sequence` and Diagnostic Counters** | `struct gc550_stream_parser` tracks `frame_sequence` and `current_frame_sequence` (`u64`). At every valid SAV boundary, `p->current_frame_sequence = p->frame_sequence++;` executes unconditionally in `gc550_reserve_video_frame()`, accurately advancing the sequence for dropped frames as required by V4L2. Completed and aborted buffers receive `p->current_frame_sequence` via `gc550_finish_buffer()`. | `StreamParser` increments `self.frame_sequence += 1` unconditionally on every accepted frame (line 323) and adds 14 offline diagnostic counters/timestamps (`video_spans`, `rejected_video_spans`, `audio_pkt_sizes`, `audio_declared_sizes`, `invalid_audio_hdr_count`, `incomplete_audio_pkts`, `incomplete_audio_bytes`, `audio_bytes_total`, EAV window counters) plus `incomplete_audio_pkts` tracking in `reset()`. | **Matches** frame interval counting across dropped frames. Python tracks additional offline metrics, while kernel attaches sequence numbers to `vb2` capture buffers. |
| **Audio Payload Delivery** | `gc550_parse_ordered_bytes()` (line 281) calls `gc550_audio_push_samples(dev, &data[i], take)` to copy received audio payload bytes into the ALSA ring buffer (when an ALSA capture substream is active) and trigger `snd_pcm_period_elapsed()`. | `parse_ordered_bytes()` (lines 399–414) only increments `cur_audio_pkt_received`, `audio_bytes_total`, and `audio_bytes_since_first_eav` by `take`. | **Differs**: Kernel pushes audio payload bytes into ALSA `runtime->dma_area` when an ALSA capture substream is active; Python only counts bytes. |

#### Scope of the Stage 4A-1 Offline Unit Tests (`TestStage4AProbeOffline`)

All seven unit tests in `scratch/gc550_stage4a_probe.py` (`test_01_init_table_validation_and_step_count`, `test_02_markers_split_across_input_chunks`, `test_03_markers_split_across_16kib_dma_boundaries_and_swap_mode`, `test_04_partial_audio_payloads_and_interrupted_input`, `test_05_invalid_headers_and_recovery`, `test_06_accurate_windowed_audio_byte_rate_accounting`, and `test_07_video_span_saturation_at_max_frame_span`) **execute and validate only the offline Python functions (`validate_init_table()` and `StreamParser`) on synthetic in-memory data** (`Confirmed`):

- They do **not** compile, call, or test the C functions in [`kernel/gc550-video.c`](../kernel/gc550-video.c), V4L2 `vb2` buffer copying, `gc550_yuyv_black` short-frame tail padding, or ALSA ring-buffer delivery in [`kernel/gc550-audio.c`](../kernel/gc550-audio.c).
- They do **not** test real GC550 hardware.

---

## E. Hardware Initialization

### 1. Structure and Origin of `gc550_init_blob`

In [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h), the initialization sequence captured from the Windows driver initializing a GC550 (`07ca:4710`, with all CryptoMemory authentication commands stripped) is stored as `gc550_init_blob[]` and replayed by `gc550_run_init_sequence()` in [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) (lines 275–312):

- **Packed Entry Format (`Confirmed` in [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) lines 22–25 and [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) lines 282–285)**:
  ```
  Offset 0:        u8  tx_len       (1..36 bytes in gc550_init_blob; sent on EP 0x01 OUT)
  Offset 1..2:     u16 rx_len       (little-endian u16; 0 if write-only, 1..512 if EP 0x81 IN reply expected)
  Offset 3..3+N-1: u8  tx_bytes[N]  (raw command bytes sent on EP 0x01 OUT, where tx_bytes[0] is the opcode)
  ```
- **Authoritative Table Metrics (`Confirmed`)**:
  - `GC550_INIT_BLOB_SIZE` = **`53,146` bytes**
  - `GC550_INIT_NUM_STEPS` = **`6,576` steps**
  - Sum of `3 + tx_len` across all `6,576` entries = **`53,146` bytes** (`0` remainder bytes)
  - Forbidden CryptoMemory opcodes (`0x05`, `0x06`, `0x07`, `0x09`) = **`0` steps**

> [!NOTE]
> **Resolution of the Historical `6,535` vs. `6,576` Step-Count Discrepancy**: An earlier Phase 3 ad-hoc regex script reported `6,535` steps (`53,148` bytes) because it sliced [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) at the first occurrence of `"gc550_init_blob"` inside the comment on line 22 (`* Entry format in gc550_init_blob:`) instead of the C array definition on line 36 (`static const u8 gc550_init_blob[GC550_INIT_BLOB_SIZE] = {`). Lines 24–25 of that comment contained two hex tokens (`0x81` and `0x01`), prepending 2 extra bytes (`0x81, 0x01`) that misaligned the first `377` bytes (`47` real steps) into `6` bogus steps before resynchronizing at byte offset `377` (`6576 - 47 + 6 = 6535`). Parsing strictly between `{` and `};` of `gc550_init_blob[]` confirms the exact `6,576` steps and `53,146` bytes.

### 2. Verified Opcode and I2C Target Distribution in `gc550_init_blob`

#### Top-Level Opcode Breakdown (`Confirmed`)

| Opcode | Constant in `kernel/gc550.h` | Step Count | Exact `tx_len` Values in `gc550_init_blob` | Exact `rx_len` Values in `gc550_init_blob` | Wire Format & Role |
|---|---|---|---|---|---|
| `0x01` | `GC550_OP_I2C_WRITE` | `1,853` | `{5, 6, 7, 8, 9, 14, 17, 28, 35, 36}` (`1..32` data bytes) | `{0}` | `[0x01, slave, 0x01, reg, data...]` — Configures ADV7604, ADV7511, PCA9539A, TLV320AIC3x, ECP3, `0x4A`, `0x65`. |
| `0x02` | `GC550_OP_I2C_READ` | `4,721` | `{5}` | `{1, 2, 3, 5, 10, 13, 28, 32}` | `[0x02, slave, 0x01, read_len, reg]` — Init readbacks and ~4,800 periodic status-polling reads. |
| `0x08` | `GC550_OP_TRIG_08` | `1` | `{1}` (`08`) | `{0}` | `[0x08]` at Step `1` (immediately after Step `0` EEPROM read). |
| `0x0E` | `GC550_OP_EEPROM_READ` | `1` | `{6}` (`0e 02 07 fe 02 00`) | `{512}` | `[0x0E, 0x02, 0x07, 0xFE, 0x02, 0x00]` at Step `0` (reads `0x0200 = 512` bytes). |
| **Total** | | **`6,576`** | | | **`53,146` total packed bytes** |

#### Per-Slave I2C Transaction Breakdown (`0x01` Writes and `0x02` Reads) (`Confirmed`)

| 7-Bit I2C Address | Target Component / Sub-Map | `0x01` Write Steps | `0x02` Read Steps | Verified Register Operations in `gc550_init_blob` | Evidence Level |
|---|---|---|---|---|---|
| `0x10` | Lattice ECP3 FPGA (`GC550_I2C_FPGA`) | `18` | `6` | Reads regs `0x00`, `0x05`, `0x06`, `0x22`; writes `0x01`, `0x02`, `0x05`, `0x06`, `0x07`, `0x80`, `0x81`, `0x86`, `0x87`, `0x89`. | `Strongly supported` |
| `0x18` | TI TLV320AIC3x (`GC550_I2C_TLV320AIC3X`) | `16` | `0` | Steps `52..67`: writes PLL (`0x03=0x91, 0x04=0x10, 0x05=0x00, 0x06=0x00, 0x0B=0x01`), serial audio interface (`0x08=0xC0, 0x09=0x30, 0x0C=0x00`), ADC/line-in gain and routing (`0x0F, 0x10, 0x13, 0x15, 0x16, 0x18`). | `Strongly supported` |
| `0x20` | ADV7604 IO Map (`GC550_I2C_ADV7604_IO`) | `1,004` | `1,393` | Programs sub-map addresses `IO[0xF3..0xFE]` (`steps 79..101`, `351..373`), power/mode/format (`0x00..0x0C`, `0x14..0x16`, `0x30`), interrupt masks/clears (`0x40..0x82`), and polls raw status (`0x60` ×`180`, `0x65` ×`144`, `0x6A` ×`116`, `0x6F` ×`49`). | `Confirmed` |
| `0x22` | ADV7604 CP Map (`GC550_I2C_ADV7604_CP`) | `47` | `394` | Configures CP CSC/adjustments (`0x3A..0x3E`, `0x41`, `0x68`, `0x69`, `0xCF`), timing resets (`0x46..0x47`, `0x7B..0x91`, `0xA2..0xBF`), and polls STDI/sync lock (`0xB1` 1-byte ×`46` + 2-byte ×`198`, `0xB5` ×`46`). | `Confirmed` |
| `0x26` | ADV7604 AFE Map (`GC550_I2C_ADV7604_AFE`) | `7` | `3` | Writes `0x01=0x2E, 0x02=0x00, 0x0C=0x1F, 0x12=0x7B, 0x15, 0xC6=0x7F, 0xC8=0x40`; reads `0x02, 0x15, 0xC6`. | `Confirmed` |
| `0x32` | ADV7604 Repeater / KSV Map (`GC550_I2C_ADV7604_REP`) | `14` | `14` | Writes BCAPS/HDCP (`0x40=0x80`), SPA location (`0x76=0xAC`), SPA port B/C/D (`0x70, 0x72, 0x74`), and EDID enable (`0x77`: `0x00` at `steps 281, 285, 297, 301`, `0x01 -> 0x03 -> 0x07 -> 0x0F` at `steps 309..315`, and final write `0x00` at `step 317`). | `Confirmed` |
| `0x34` | ADV7604 HDMI RX Map (`GC550_I2C_ADV7604_HDMI`) | `50` | `612` | Configures termination (`0x01`), ADI recommended HDMI settings (`0x0D, 0x3D, 0x3E, 0x4E, 0x57, 0x58, 0x8D, 0x8E, 0x93, 0x94, 0x96, 0x9D`), and polls `0x04` (×`55`), `0x05` (×`53`), `0x36` 5-byte CS_DATA (×`46`), `0x5B` (1-byte ×`45`, 3-byte ×`18`), `0x5D` 3-byte (×`51`). | `Confirmed` |
| `0x36` | ADV7604 EDID RAM Map (`GC550_I2C_ADV7604_EDID`) | `10` | `0` | Steps `286..293`: writes `256`-byte EDID in `8 × 32`-byte blocks (`0x00..0xE0`); steps `294..295`: writes `0xAC=0x10`, `0xAD=0x00`. | `Confirmed` |
| `0x38` | ADV7511 Packet Memory (or ADV7604 ESDP) | `1` | `0` | Step `2,464`: writes `31` zero bytes to `reg 0xC0` (`01 38 01 c0 00...`, `tx_len = 35`) during `0x3D` setup. | `Confirmed` (write) / `Strongly supported` (ADV7511 packet memory) |
| `0x3D` | ADV7511 Main Map (`GC550_I2C_ADV7511_TX`) | `493` | `685` | Configures HDMI passthrough TX (`0x41`, fixed regs `0x98..0xE0`, audio `N` `0x01..0x03`, CSC `0x18..0x3B`), polls HPD/interrupts (`0x42, 0x96, 0x97, 0x9E, 0xAF`). | `Strongly supported` |
| `0x3F` | Unknown Slave `0x3F` | `0` | `8` | Steps `2,512..2,519`: `8` reads of `32` bytes each (`rx_len = 32`) at offsets `0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0` (`256` B total). | `Unknown` |
| `0x46` | ADV7604 InfoFrame Map (`GC550_I2C_ADV7604_INFO`) | `0` | `1,432` | Polls AVI InfoFrame (`0x00..0x1B`), Audio InfoFrame (`0x1D`, 10 B), `0x54` (28 B), and InfoFrame headers/checksums (`0xE0..0xE3, 0xEC`). | `Confirmed` |
| `0x4A` | Unknown Slave `0x4A` | `125` | `106` | Writes `regs 0x06..0x5F` (many set to `0x3F`); reads `regs 0x00..0x03, 0x05..0x61, 0x7E..0x7F`. | `Inferred` (audio mixer) / `Unknown` |
| `0x65` | Unknown Slave `0x65` | `4` | `0` | Steps `18..19` and `1,427..1,428`: writes `0x94 = 0x6D` (`1` B) and `0x18..0x1C = 04 01 20 23 8C` (`5` B). | `Unknown` |
| `0x74` | NXP PCA9539A (`GC550_I2C_PCA9539A`) | `64` | `68` | Configures port direction (`0x06=0x00, 0x07=0x09`) and toggles chip reset/enable lines on output ports `0x02`/`0x03`. | `Confirmed` |

### 3. Cold vs. Warm Board-State Detection and Post-Bootstrap Fixups

1. **Warm vs. Cold Detection (`gc550_detect_board_state()` in [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) lines 212–240) (`Confirmed`)**:
   - Called from `gc550_bootstrap_hw()` (lines 317–354 of [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c)) when `dev->is_bootstrapped` is `false`. (Note: `gc550.ko` has no `force_reinit` module parameter; `dev->is_bootstrapped` is per-device-instance state initialized to `false` on USB probe.)
   - Drains up to 4 stale replies on `EP 0x81 IN` (`gc550_cmd_drain_responses()`) under `dev->cmd_lock`, then performs two `0x02` I2C read transactions:
     - `gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0xfa, &edid_map)` -> checks `edid_map == 0x6c`. On a cold power-cycled board, `IO[0xFA]` reads `0x00` ([`PROTOCOL.md`](../PROTOCOL.md) line 811).
     - `gc550_i2c_read(dev, GC550_I2C_PCA9539A, 0x06, gpio_cfg, 2)` -> checks `gpio_cfg[0] == 0x00 && gpio_cfg[1] == 0x09`. On a freshly plugged card, `PCA9539A[0x06..0x07]` was observed to read `00 89` ([`PROTOCOL.md`](../PROTOCOL.md) line 813).
   - If `edid_map == 0x6c && gpio_cfg[0] == 0x00 && gpio_cfg[1] == 0x09`, `gc550_bootstrap_hw()` skips `gc550_run_init_sequence()`, runs `gc550_set_auto_input_colorspace(dev)`, and sets `dev->is_bootstrapped = true`.
   - **Unresolved Driver Reliability Issue — Partial-Bootstrap Detection Blind Spot (`Inferred` / Unresolved)**:
     - While `gc550_detect_board_state()` reads the **correct** ADV7604 EDID map register (`IO[0xFA]`) and **correct** target value (`0x6C`), matching `IO[0xFA] == 0x6C` and `PCA9539A[0x06..0x07] == {0x00, 0x09}` only proves that a prior initialization progressed at least past `step 91` (first pass) or `step 363` (second pass). If an earlier cold bootstrap was interrupted mid-sequence after `step 363` (before `step 6,575`), or if downstream peripherals (`ADV7511` at `0x3D`, `Lattice ECP3` at `0x10`) lost state without `IO[0xFA]` resetting, `gc550_detect_board_state()` would still classify the partially initialized board as warm (`already_configured = true`) and skip `gc550_run_init_sequence()`, leaving the hardware in an incomplete state. This partial-initialization blind spot remains an unresolved driver reliability issue. (Additionally, on the warm reuse path at `lines 332–338` of [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c), `gc550_bootstrap_hw()` returns early before calling `gc550_log_signal_status(dev)`, though `gc550_start_streaming()` in [`kernel/gc550-video.c`](../kernel/gc550-video.c) at line 629 calls `gc550_log_signal_status(dev)` immediately afterwards.)

2. **Post-Bootstrap Colorspace Fixup (`gc550_set_auto_input_colorspace()` in [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) lines 249–270) (`Confirmed`)**:
   - In `gc550_init_blob`, the initial setup writes `ADV7604 IO[0x02] = 0xFC` (`steps 1,368..1,453`, auto input color space `0xF`) and `0xF8` (`steps 1,455..1,458`), and sets `CP[0x3A..0x3E]` to neutral (`steps 1,405..1,415`: `0x3A=0x80, 0x3B=0x80, 0x3C=0x00, 0x3D=0x00, 0x3E=0x80`).
   - However, the recorded polling loop later reacts to the HDMI source connected when the Windows trace was captured ([`PROTOCOL.md`](../PROTOCOL.md) lines 583–589):
     - `steps 2,635..2,649` write `IO[0x02] = 0x08` -> `0x0C`, and **`step 5,989`** writes **`IO[0x02] = 0x1C`** (`bits 7:4 = 0x1`, forcing full-range RGB input).
     - `steps 1,734` and `2,638` write `CP[0x3C] = 0xF0` before `step 5,992` writes `CP[0x3C] = 0x00`.
   - Therefore, on both cold and warm paths, `gc550_bootstrap_hw()` calls `gc550_set_auto_input_colorspace(dev)`:
     - Reads `ADV7604 IO[0x02]` and writes `(io_02 & 0x0f) | 0xf0` (restoring `bits 7:4 = 0xF`, `ADV76XX_INP_COLOR_SPACE_AUTO`, e.g. `0x1C -> 0xFC`).
     - Writes `neutral_cp[4] = {0x80, 0x80, 0x00, 0x00}` to `ADV7604 CP[0x3A..0x3D]`.

3. **Important Limitation of Offline Table Validation (`Confirmed`)**:
   - Offline validation of `gc550_init_blob` proves only structural consistency of the C array in [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) against the expected entry format and opcode rules. It does **not** prove that hardware initialization has been executed on a given machine.

---

## F. Audio Architecture

### 1. Hardware Audio Signal Path

```
                    +-------------------------------------------------+
                    | ADV7604 HDMI Receiver (0x20 / 0x34 / 0x36 / 0x46)|
                    | - EDID in gc550_init_blob (0x36: "ADI HDMI TV") |
                    |   Audio Data Block 0x91..0xA3 (LPCM: 0f 7f 07)  |
                    | - Audio Packet Extractor (N / CTS / CS_DATA)    |
                    +-------------------------------------------------+
                                  |                         |
         I2S / SPDIF Digital      |                         | Passthrough Audio
         Audio Bus                v                         v
                    +--------------------+       +--------------------+
                    | Lattice ECP3 FPGA  |       | ADV7511 HDMI TX    |
                    | (0x10)             |       | (0x3D / 0x38)      |
                    | - Packs 00 FF FF 58|       +--------------------+
                    +--------------------+
                              |
                              | Parallel GPIF II Bus
                              v
+-------------------+   +--------------------+
| TLV320AIC3x (0x18)|   | Cypress FX3        |===> EP 0x83 IN (00 FF FF 58 + BE32 len + Audio payload bytes
| Analog Audio      |   +--------------------+     [sample format and sample semantics under verification])
+-------------------+
```

### 2. Extracted 256-Byte EDID and CEA Audio Data Block in `kernel/gc550-regtables.h` (`Confirmed`)

In [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) (`steps 286..293`, lines 180–199), `gc550_init_blob` writes a 256-byte EDID image in eight 32-byte blocks (`0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0`) to `GC550_I2C_ADV7604_EDID` (`0x36`), followed by `2` single-byte writes setting the CEC Source Physical Address (`0xAC = 0x10`, `0xAD = 0x00` at `steps 294..295`).

#### Complete 256-Byte EDID Extracted from `gc550_init_blob` (`steps 286..293`)

```text
Offset  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
0x00:   00 ff ff ff ff ff ff 00 06 96 78 44 01 00 00 00
0x10:   17 14 01 03 80 0c 09 78 0a 1e ac 98 59 56 85 28
0x20:   29 52 57 af ff 00 01 01 01 01 01 01 01 01 01 01
0x30:   01 01 01 01 01 01 8c 0a d0 8a 20 e0 2d 10 10 3e
0x40:   96 00 81 60 00 00 00 18 01 1d 80 18 71 1c 16 20
0x50:   58 2c 25 00 81 49 00 00 00 9e 00 00 00 fc 00 41
0x60:   44 49 20 48 44 4d 49 20 54 56 0a 20 00 00 00 fd
0x70:   00 17 3d 0d 2e 11 00 0a 20 20 20 20 20 20 01 1d
0x80:   02 03 30 71 4c 90 05 04 01 02 14 13 1f 06 15 03
0x90:   12 32 0f 7f 07 17 07 40 3d 1f 00 57 06 00 5f 7e
0xA0:   01 67 7e 00 83 4f 00 00 67 03 0c 00 10 00 b8 2d
0xB0:   01 1d 00 72 51 d0 1e 20 6e 28 55 00 81 49 00 00
0xC0:   00 1e d6 09 80 a0 20 e0 2d 10 10 60 a2 00 81 60
0xD0:   00 08 08 18 8c 0a d0 90 20 40 31 20 0c 40 55 00
0xE0:   81 60 00 00 00 18 00 00 00 00 00 00 00 00 00 00
0xF0:   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 c4
```

#### Resolution of the `0f 7f 07` vs. `23 09 07 07` EDID Description Conflict

1. **Identity of the EDID Written to `0x36` in `gc550_init_blob` (`Confirmed`)**:
   - The 256-byte image at `steps 286..293` has manufacturer ID `ATV` (`0x06, 0x96`), product code `0x4478`, year `2010` (week `23`), and monitor name `"ADI HDMI TV\n "` (`0x5F..0x6B`)—which [`PROTOCOL.md`](../PROTOCOL.md) (line 686) identifies as **"EDID #2"** (the Analog Devices reference default EDID).
   - Earlier notes that mentioned a 4-byte Audio Data Block `23 09 07 07` (`header 0x23` = tag `1`, length `3` bytes; single 2-channel LPCM SAD `09 07 07` for `32/44.1/48 kHz`) were referring to the sibling **GC551** `"AVER GC551"` EDID #1 (`PROTOCOL.md` line 685, *"audio LPCM 2ch"*). That `23 09 07 07` sequence does **not** exist in `gc550_init_blob` (`kernel/gc550-regtables.h`).
2. **Exact CEA-861 Extension Block (`0x80..0xFF`) and Audio Data Block (`0x91..0xA3`) in `gc550_init_blob` (`Confirmed`)**:
   - `0x80..0x83`: `02 03 30 71` (CEA-861 tag `0x02`, revision `3`, DTD start `0x30` -> `0xB0`, flags `0x71`).
   - `0x84..0x90`: `4c 90 05 04 01 02 14 13 1f 06 15 03 12` — **Video Data Block** (header `0x4C`: tag `2` = Video, length `12` bytes; `0x90` is VIC 16 `1080p60` native, ending with `0x12` = VIC 18 `576p50` at offset `0x90`).
   - **`0x91..0xA3`: CEA Audio Data Block (`19` bytes total: `1` header byte + `18` payload bytes = `6` Short Audio Descriptors)**:
     - **Offset `0x91`**: **`0x32`** — Data Block Header (`bits 7:5 = 001b` = Tag `1` Audio Data Block; `bits 4:0 = 10010b` = **`18` bytes** = `6 × 3`-byte SADs).

| SAD # | EDID Offset | Raw Bytes | Audio Format Code (`bits 6:3` of Byte 1) | Max Channels (`(Byte 1 & 0x7) + 1`) | Advertised Sample Rates (Byte 2 bitmask) | Byte 3 (LPCM Bit Depths / Max Bitrate) |
|---|---|---|---|---|---|---|
| **SAD #1** | **`0x92..0x94`** | **`0f 7f 07`** | **`1` — Linear PCM (LPCM)** (`0x0F >> 3 = 1`) | **`8` channels** (`(0x0F & 0x7) + 1 = 8`) | **`32`, `44.1`, `48`, `88.2`, `96`, `176.4`, `192 kHz`** (`0x7F` = bits `6:0` all set) | **`16-bit`, `20-bit`, `24-bit`** (`0x07` = bits `2:0` all set) |
| SAD #2 | `0x95..0x97` | `17 07 40` | `2` — AC-3 | `8` channels | `32`, `44.1`, `48 kHz` (`0x07`) | Max bitrate `512 kbps` (`0x40 × 8`) |
| SAD #3 | `0x98..0x9A` | `3d 1f 00` | `7` — DTS | `6` channels | `32`, `44.1`, `48`, `88.2`, `96 kHz` (`0x1F`) | `0x00` (`0 kbps` specified) |
| SAD #4 | `0x9B..0x9D` | `57 06 00` | `10` — Enhanced AC-3 (E-AC-3 / DD+) | `8` channels | `44.1`, `48 kHz` (`0x06`) | `0x00` (format-specific) |
| SAD #5 | `0x9E..0xA0` | `5f 7e 01` | `11` — DTS-HD | `8` channels | `44.1`, `48`, `88.2`, `96`, `176.4`, `192 kHz` (`0x7E`) | `0x01` (format-specific) |
| SAD #6 | `0xA1..0xA3` | `67 7e 00` | `12` — MAT (MLP / Dolby TrueHD) | `8` channels | `44.1`, `48`, `88.2`, `96`, `176.4`, `192 kHz` (`0x7E`) | `0x00` (format-specific) |

   - `0xA4..0xA7`: `83 4f 00 00` — **Speaker Allocation Data Block** (header `0x83`: tag `4`, length `3` bytes).
   - `0xA8..0xAF`: `67 03 0c 00 10 00 b8 2d` — **HDMI LLC Vendor-Specific Data Block** (header `0x67`: tag `3`, length `7` bytes, OUI `00-0C-03`, CEC physical address `1.0.0.0` at `0xAC..0xAD`, modified in place by `steps 294..295`).

> [!WARNING]
> **Unresolved Finding in `gc550_init_blob` — `REP[0x77] = 0x00` Disables Internal EDID Ports and Second Reset Pulse Follows (`Confirmed` register writes / `Unknown` runtime DDC source)**:
> 1. **Final `REP[0x77]` Write Disables All Four Internal ADV7604 EDID Ports (`Confirmed` in `gc550_init_blob`)**:
>    - In the ADV7604 Repeater Map (`0x32`), register `REP[0x77]` (`EDID_ENABLE`) defines bits `3:0` as the internal EDID enable controls for HDMI ports D, C, B, and A (`bit 0 = EDID_A_ENABLE`, `bit 1 = EDID_B_ENABLE`, `bit 2 = EDID_C_ENABLE`, `bit 3 = EDID_D_ENABLE`).
>    - After writing the 256-byte `"ADI HDMI TV"` EDID (`0f 7f 07`) to `0x36` at `steps 286..295`, `gc550_init_blob` sequences `REP[0x77]` through `0x01` (`step 309`), `0x03` (`step 311`), `0x07` (`step 313`), and `0x0F` (`step 315`), and then performs its **final `REP[0x77]` write at `step 317` with value `0x00`** (`01 32 01 77 00`).
>    - A final value of `REP[0x77] = 0x00` disables all four internal ADV7604 EDID ports (`A..D`), meaning the ADV7604's internal EDID RAM (`0x36`) is left disabled on the HDMI DDC bus at the end of that sequence.
> 2. **Second `PCA9539A` Reset Pulse Without Re-Writing `0x36` or `REP[0x77]` (`Confirmed` in `gc550_init_blob` / `Unknown` runtime EDID)**:
>    - Immediately after `step 317` (`steps 326..349`), `gc550_init_blob` issues the second `PCA9539A` (`0x74`) port `0x02` reset sequence (`71 -> 31 -> 71 -> 71 -> 51 -> 71`) and rewrites `IO[0xF3..0xFE]` (`steps 351..373`), but **never writes to `0x36` or `REP[0x77]` again** (unlike the GC551 sequence in [`PROTOCOL.md`](../PROTOCOL.md) lines 460–467, where EDID #1 `"AVER GC551"` is written byte-by-byte at `50..307` and again after the second reset pulse at `405..660` with internal EDID re-enabled).
>    - **Implication & Unresolved Question**: Because `step 317` sets `REP[0x77] = 0x00` (disabling all four internal ADV7604 EDID ports) and the subsequent reset at `steps 326..349` also leaves `REP[0x77]` at its reset default (`0x00`), the ADV7604 does not appear to serve the `"ADI HDMI TV"` image from `0x36` after initialization completes. What exact EDID an upstream HDMI source actually receives on the GC550 (for example, whether DDC is routed to an external on-board EDID EEPROM, bridged from the ADV7511 passthrough sink, or related to the `8 × 32`-byte reads from slave `0x3F` at `steps 2,512..2,519`) remains **unresolved** and has not been verified on hardware.

### 3. ADV7604 HDMI Audio Registers, `NEW_SAMP_RT_RAW`, and `CS_DATA_VALID_RAW`

Based on the Analog Devices ADV7604 Hardware Manual (Rev. F) and verified register accesses in `gc550_init_blob` (`Confirmed` for register addresses accessed in `gc550_init_blob`; see manual discrepancy notes below):

| Map (`7-bit Addr`) | Register Offset & Bits | Register / Bit Name | Role & Observed Activity in `gc550_init_blob` | Evidence Level |
|---|---|---|---|---|
| `IO` (`0x20`) | `0x65` bit 7 (`0x80`) | `CS_DATA_VALID_RAW` | Unmasked raw status bit indicating whether IEC 60958 channel-status data in `HDMI[0x36..0x3A]` is valid (`1` = valid, `0` = invalid/unavailable). Read `144` times in `gc550_init_blob` (`IO[0x66]` interrupt status read `45` times; `IO[0x67]` clear written `45` times). | `Confirmed` (polled in `gc550_init_blob`) / `Strongly supported` (ADV7604 manual) |
| `IO` (`0x20`) | `0x7E` bit 4 (`0x10`) **or** bit 3 (`0x08`) *(manual conflict)* | `NEW_SAMP_RT_RAW` | Raw status bit indicating that the IEC 60958 channel-status sampling-frequency bits (`HDMI[0x39]` bits `3:0`) have changed. **ADV7604 Hardware Manual Rev. F inconsistency**: Section 7.18.8 identifies `NEW_SAMP_RT_RAW` as `IO[0x7E]` **bit 4**, whereas Table 38 identifies it as `IO[0x7E]` **bit 3**. `IO[0x7E]` is read `0` times in `gc550_init_blob`. Definitive bit position in `IO[0x7E]` is **unresolved** pending authoritative clarification. | `Unknown` (unresolved bit 4 vs. bit 3 discrepancy in Rev. F manual) |
| `IO` (`0x20`) | `0x7F` bit 3 (`0x08`) *(unverified)* | `NEW_SAMP_RT_ST` *(unverified)* | `IO[0x7F]` is read `45` times in `gc550_init_blob` alongside `IO[0x80]` writes (`0x08`/`0x00`) and `IO[0x82]` writes (`0x08`/`0x00`, `23` times), which suggests a latched interrupt status register, but the exact bit assignment of `NEW_SAMP_RT_ST` at `IO[0x7F][3]` is **unverified** pending authoritative register-table confirmation. | `Inferred` / Unverified |
| `IO` (`0x20`) | `0x80` bit 3 (`0x08`) | `NEW_SAMP_RT_CLR` | Write-only clear bit documented at `IO[0x80]` bit 3 (`0x08`) in the ADV7604 Hardware Manual Rev. F. In `gc550_init_blob`, `IO[0x80]` is written `45` times with `0x08` / `0x00`. | `Confirmed` (written in `gc550_init_blob`) / `Strongly supported` (ADV7604 manual) |
| `HDMI` (`0x34`) | `0x04` bit 0 (`0x01`), bit 6 (`0x40`) | `AUDIO_PLL_LOCKED`, `AV_MUTE` | Bit 0 = `1` when the ADV7604 audio clock regeneration PLL is locked; Bit 6 = `1` when `AV_MUTE` is active. Read `55` times in `gc550_init_blob`. | `Strongly supported` |
| `HDMI` (`0x34`) | `0x05` bit 7 (`0x80`) | `HDMI_MODE` | Bit 7 = `1` in HDMI mode (audio & InfoFrames active), `0` in DVI mode. Read `53` times via 1-byte reads in `gc550_init_blob` and read by `gc550_log_signal_status()`. | `Confirmed` |
| `HDMI` (`0x34`) | `0x36..0x3A` (5 bytes) | `CS_DATA[39:0]` | First 5 bytes (`bits 0..39`) of decoded IEC 60958-3 Channel Status. `HDMI[0x39]` is Byte 3 (`bits 24..31`), where bits `3:0` (`CS[27:24]`) encode the sampling frequency. Read `46` times as a 5-byte block (`02 34 01 05 36`) in `gc550_init_blob`. | `Confirmed` (5-byte reads in `gc550_init_blob`) / `Strongly supported` (ADV7604 manual) |
| `HDMI` (`0x34`) | `0x5B..0x5F` (5 bytes) | `CTS[19:0]` & `N[19:0]` | HDMI Audio Clock Regeneration packet values (`128 * fs = f_TMDS * N / CTS`). In `gc550_init_blob`, read as `0x5B` (1-byte ×`45`, 3-byte ×`18`) and `0x5D` (3-byte ×`51`). | `Strongly supported` (register map) / `Unknown` (latch coherence unverified) |

#### Discrepancy and Caveats on `NEW_SAMP_RT_RAW` (`IO[0x7E]`) and Limitations of `CS_DATA_VALID_RAW` (`IO[0x65][7]`)

1. **ADV7604 Hardware Manual Rev. F Bit-Position Discrepancy for `NEW_SAMP_RT_RAW` (`Unresolved`)**:
   - The Analog Devices ADV7604 Hardware Manual (Rev. F) contains an internal contradiction regarding the bit position of `NEW_SAMP_RT_RAW` in `IO[0x7E]`:
     - **Section 7.18.8** identifies `NEW_SAMP_RT_RAW` at **`IO[0x7E]` bit 4** (`0x10`).
     - **Table 38** identifies `NEW_SAMP_RT_RAW` at **`IO[0x7E]` bit 3** (`0x08`).
     - The corresponding clear bit (`NEW_SAMP_RT_CLR`) is documented at **`IO[0x80]` bit 3** (`0x08`, matching the `0x08` writes to `IO[0x80]` in `gc550_init_blob`).
   - Neither bit 4 nor bit 3 of `IO[0x7E]` should be assumed without authoritative hardware/manual verification; the definitive `IO[0x7E]` bit position is marked **unresolved**. Similarly, `NEW_SAMP_RT_ST` at `IO[0x7F][3]` is marked **unverified** unless confirmed by an authoritative register specification.
2. **ADV7604 Manual Functional Caveat on `NEW_SAMP_RT_RAW`**:
   - According to the ADV7604 Hardware Manual Rev. F, `NEW_SAMP_RT_RAW` triggers when the channel-status sampling-rate bits change, **with the explicit caveat that `NEW_SAMP_RT_RAW` does NOT trigger when `CS_DATA_VALID_RAW` (`IO[0x65][7]`) is `0`**.
   - Consequently, if an HDMI source drops channel-status validity (`CS_DATA_VALID_RAW == 0`) during a mode/rate switch or audio dropout, relying on `NEW_SAMP_RT_RAW` edge detection can miss the transition.
3. **Limitations of `CS_DATA_VALID_RAW` (`IO[0x65]` bit 7)**:
   - `CS_DATA_VALID_RAW == 1` only indicates that the ADV7604 HDMI receiver has framed a valid IEC 60958 channel-status block in `HDMI[0x36..0x3A]`.
   - It does **not** guarantee that the HDMI source transmits an explicit or accurate sampling-frequency code in `HDMI[0x39]` bits `3:0` (for example, IEC 60958-3 defines `0x1` as *"sampling frequency not indicated"*, and non-compliant sources may send channel-status bits that disagree with the actual `N`/`CTS` audio clock).
   - Most importantly, `CS_DATA_VALID_RAW` and `HDMI[0x39]` only reflect the **ADV7604 HDMI receiver front-end**; they do **not** establish whether the GC550's downstream FPGA (`0x10`), codec/mixer (`0x18` / `0x4A`), and FX3 pass that sample rate unchanged into `EP 0x83 IN` (`00 FF FF 58`) or resample/repacketize to `48 kHz`.

#### Standard IEC 60958-3 Sampling Frequency Codes (`HDMI[0x39] & 0x0F`)

| `HDMI[0x39] & 0x0F` | Binary (`b3 b2 b1 b0`) | Nominal Sample Rate (`fs`) | Advertised in `gc550_init_blob` EDID SAD #1 (`0f 7f 07`)? |
|---|---|---|---|
| `0x3` | `0011` | `32,000 Hz` (`32 kHz`) | Yes (`bit 0`) |
| `0x0` | `0000` | `44,100 Hz` (`44.1 kHz`) | Yes (`bit 1`) |
| `0x2` | `0010` | `48,000 Hz` (`48 kHz`) | Yes (`bit 2`) |
| `0x8` | `1000` | `88,200 Hz` (`88.2 kHz`) | Yes (`bit 3`) |
| `0xA` | `1010` | `96,000 Hz` (`96 kHz`) | Yes (`bit 4`) |
| `0xC` | `1100` | `176,400 Hz` (`176.4 kHz`) | Yes (`bit 5`) |
| `0xE` | `1110` | `192,000 Hz` (`192 kHz`) | Yes (`bit 6`) |
| `0x1` | `0001` | Not indicated (`CS` unspecified) | N/A |

> [!IMPORTANT]
> **Hardware Caveats Regarding GC550 Audio Sample-Rate Detection**:
> 1. **`N`/`CTS` Multi-Byte Latching Is Unverified (`Unknown`)**: Do **not** assume that reading `HDMI[0x5B]` latches a coherent multi-byte `N`/`CTS` snapshot across `HDMI[0x5B..0x5F]` without primary datasheet verification. (Notably, `gc550_init_blob` itself reads `0x5B` and `0x5D` as separate 3-byte transactions rather than a single 5-byte read.)
> 2. **Host USB Byte-Rate Measurement Is Diagnostic Only (`Confirmed`)**: As confirmed on Titan in Phases B2–C3 below, `parser.audio_bytes` measures the declared USB packet payload size (`~3,200` bytes/frame at `1080p60` = `~192,000` bytes/s) regardless of whether the HDMI source is idle (`cs_valid=0`), `48 kHz`, or `44.1 kHz`. Computing the `00 FF FF 58` payload byte rate per second on the host therefore **cannot** distinguish incoming HDMI sample rates or independently establish the actual PCM sample rate.

### 4. Audio Payload Accounting (Phase D1 Analysis) and Verified Titan Hardware Findings (Phases B2, C1, C2, and C3)

> **Phase Provenance**: All hardware measurements in this section originate from **Phases B2, C1, C2, and C3** on Titan (`07ca:4710`). **Phase D1** was a read-only code and protocol investigation that analyzed those hardware observations against [`kernel/gc550-video.c`](../kernel/gc550-video.c), [`kernel/gc550-audio.c`](../kernel/gc550-audio.c), and [`PROTOCOL.md`](../PROTOCOL.md).

#### Audio Packet Structure and Parser Accounting (`Confirmed` in [`kernel/gc550-video.c`](../kernel/gc550-video.c) & [`kernel/gc550-audio.c`](../kernel/gc550-audio.c))

1. **Wire Structure on `EP 0x83 IN`**:
   - **Audio marker (`4` bytes)**: `00 FF FF 58` (`GC550_MARKER_AUDIO`), emitted once per video frame after the closing `EAV` header.
   - **Payload length header (`4` bytes)**: Big-endian `u32` (`p->audio_rem = get_unaligned_be32(p->hdr_buf)`).
   - **Audio payload (`p->audio_rem` bytes)**: Audio payload bytes declared by the length header (sample format and sample semantics under verification).
   - **Post-audio DMA filler**: Repeating `AA 55 55 AA` pattern padding the remainder of the FX3 `16 KiB` DMA buffer window until the next frame's `00 FF FF C0` (`SAV`) marker.
2. **How `gc550_parse_ordered_bytes()` and `gc550_audio_push_samples()` Process the Payload (`Confirmed`)**:
   - In [`kernel/gc550-video.c`](../kernel/gc550-video.c) (lines 214–217, 266–288, 357–365), `gc550_handle_marker()` consumes the 4-byte `00 FF FF 58` marker and transitions to `GC550_STATE_AUDIO_HEADER`.
   - `GC550_STATE_AUDIO_HEADER` consumes the 4-byte big-endian length header into `p->hdr_buf[0..3]`, decodes `p->audio_rem`, and transitions to `GC550_STATE_AUDIO_DATA` when `0 < p->audio_rem <= 65536`.
   - In `GC550_STATE_AUDIO_DATA`, the parser bypasses marker scanning and consumes `take = min_t(size_t, len - i, p->audio_rem)` bytes per chunk, increments `p->audio_bytes += take`, and passes the identical byte span (`&data[i]`, `take`) directly to `gc550_audio_push_samples(dev, &data[i], take)`. When `p->audio_rem == 0`, it returns to `GC550_STATE_IDLE`, skipping all trailing `AA 55 55 AA` DMA filler bytes until the next `0x00` marker prefix.
   - The parser therefore **excludes** the 4-byte `00 FF FF 58` marker, the 4-byte big-endian length field, and the trailing `AA 55 55 AA` filler from `parser.audio_bytes`.
3. **What `parser.audio_bytes` Measures vs. What It Does Not Establish (`Confirmed`)**:
   - `parser.audio_bytes` measures **exclusively the payload byte count declared by the FPGA's 4-byte big-endian length field** (`p->audio_rem`) and passed to `gc550_audio_push_samples()`.
   - Because the parser does not inspect the contents of those payload bytes, `parser.audio_bytes` **does not independently establish**:
     - The number of unique PCM samples.
     - Whether samples are duplicated.
     - Whether padding or silence is embedded inside the declared payload.
     - The actual audio sample rate.

#### Fixed Payload Observations: Measured Titan GC550 Results vs. Upstream `PROTOCOL.md` (`Confirmed`)

Controlled V4L2 capture tests (`1920×1080p60`, `swap_mode=0`, `desyncs=0`, with a Raspberry Pi 3B HDMI source and the GC550 ALSA PCM device closed) were conducted on **Titan** (`7.3.1-1-cachyos`, `07ca:4710`) in Phases B2, C1, C2, and C3:

| Phase | HDMI audio condition | Audio bytes | Video frames |
|---|---|---:|---:|
| B2 | No active HDMI audio | 959,984 | 300 |
| C1 | 48 kHz, valid channel status | 11,523,052 | 3,600 |
| C2 | 44.1 kHz source, incorrect channel-status metadata | 11,523,044 | 3,600 |
| C3 | 44.1 kHz, valid channel status | 11,523,036 | 3,600 |

- **Measured on Titan (`GC550`, `1080p60`) (`Confirmed` observation)**:
  - Across all four phases, the observed audio payload is approximately **`3,200` bytes per video frame** or **`192,000` bytes/s** (`300 × 3,200 = 960,000` B in Phase B2; `3,600 × 3,200 = 11,520,000` B plus one boundary packet of `~3,036..3,052` B in Phases C1–C3).
- **Reported by Upstream Reverse Engineering ([`PROTOCOL.md`](../PROTOCOL.md) lines 178, 848, 957 on `GC551`) (`Confirmed` upstream reference)**:
  - Upstream `PROTOCOL.md` documents a `3,200`-byte audio payload per video frame at `1080p60` (`48 kHz × 4 B ÷ 60`) and a **`3,840`-byte audio payload per video frame at `1080p50`** (`48 kHz × 4 B ÷ 50`, observed on a Sony A7 III `1080p50` capture on GC551), both corresponding to the same nominal **`192,000` bytes/s**. (Note: the `1080p50` `3,840`-byte figure is from upstream `PROTOCOL.md`, not a Titan measurement.)

#### HDMI Sample-Rate Verification on Titan (`Confirmed`)

- **Phase C1 (`48 kHz` reference)**: With the Raspberry Pi 3B outputting a `48,000 Hz` stereo `S16_LE` (`IEC958_SUBFRAME_LE`, `AES3=0x02`) 1 kHz sine wave, `gc550_log_signal_status()` read `ADV7604 IO[0x65] = 0x82 -> 0x82` (`bit 7 / cs_valid = 1 -> 1`) and `HDMI[0x39] = 0x02` (`cs_fs = 0x2`), confirming valid **`48,000 Hz`** IEC 60958 channel status at the ADV7604 receiver (`11,523,052` audio bytes over `3,600` frames).
- **Phase C3 (`44.1 kHz` verified)**: With the Raspberry Pi 3B hardware parameters verified at `44,100 Hz` (`IEC958_SUBFRAME_LE`) and the ALSA IEC958 channel-status byte explicitly set to `AES3=0x00` (`numid=4`: `AES0=0x04 AES1=0x82 AES2=0x00 AES3=0x00`), `gc550_log_signal_status()` read `ADV7604 IO[0x65] = 0x82 -> 0x82` (`bit 7 / cs_valid = 1 -> 1`) and `HDMI[0x39] = 0x00` (`cs_fs = 0x0`), confirming valid **`44,100 Hz`** IEC 60958 channel status at the ADV7604 receiver (`11,523,036` audio bytes over `3,600` frames).
- **Comparison of C1 (`48 kHz`) and C3 (`44.1 kHz`)**: Both verified conditions produced practically identical USB audio payload totals over `3,600` video frames, differing by only **`16` bytes** (`11,523,052` vs. `11,523,036` bytes).
- **Note on Phase C2 (Incorrect Channel-Status Metadata)**: In Phase C2, the Raspberry Pi hardware ran at `44,100 Hz`, but the default ALSA `hdmi` plugin configuration transmitted `AES3=0x02` (`48 kHz`) in the IEC 60958 channel status (`HDMI[0x39] = 0x02`). Phase C2 is recorded only as a test with incorrect source channel-status metadata, **not** as a valid `44.1 kHz` receiver verification.

#### Architectural Implications and Competing Hypotheses (`Confirmed` Fixed Rate / `Unknown` Conversion Location)

Because Phase B2 demonstrated that the GC550 continues generating `~3,200` audio payload bytes per video frame (`959,984` bytes over `300` frames) even when no valid HDMI audio is present (`IO[0x65] = 0x40`, `cs_valid = 0`), and Phases C1 and C3 delivered virtually identical byte totals (`~192,000` bytes/s) at both `48 kHz` and `44.1 kHz`, the USB audio packet byte rate is fixed relative to the video frame period rather than scaling with the incoming HDMI sample rate.

As empirically demonstrated in Phase D2 below, the GC550 hardware delivers a continuous `48 kHz` stereo `S16_LE` audio payload to the host ALSA interface under both `48 kHz` and `44.1 kHz` HDMI input. The driver performs no host-side resampling and passes demultiplexed payload bytes directly to ALSA ring buffers. Rate conversion upstream of the host ALSA interface is empirically supported by the preserved recordings. However, the exact internal silicon location and implementation mechanism (ADV7604 internal processing, an on-board DSP, or Cypress FX3 firmware) remain **unknown**.

### 5. Hardware Audio Capture and Offline Signal Analysis (Phases D2.1 & D2.2.7) (`Confirmed`)

> **Phase Provenance**: Physical ALSA audio captures were executed on **Titan** (`7.3.1-1-cachyos`, `07ca:4710`) during **Phase D2.1**. Rigorous offline spectral, continuity, and statistical reproducibility audits were completed in **Phases D2.2 through D2.2.7** using [`scratch/verify_d2_audio.py`](../scratch/verify_d2_audio.py) and verified against machine-readable JSON metrics. Note: These 30-second ALSA recordings are designated **D2 Condition 1** and **D2 Condition 2**, distinct from the earlier Phases B2 and C1–C3 (which were V4L2-only payload-byte tests with the ALSA device closed).

#### Capture Execution and Filesystem Verification (`Confirmed`)
Two controlled 30-second ALSA captures (`arecord -D hw:3,0 -f S16_LE -c 2 -r 48000`) were performed with concurrent V4L2 streaming (`1080p60`, 3,600 frames) using a 1,000 Hz dual-mono reference sine wave from a Raspberry Pi 3B:
1. **D2 Condition 1 (48 kHz HDMI Input)**: Captured to [`/tmp/gc550_d2_48k_raw_retry.pcm`](/tmp/gc550_d2_48k_raw_retry.pcm) (`5,760,000` bytes, `1,440,000` frames @ `48,000 Hz`). ADV7604 decoded rate: `48,000 Hz` (`HDMI[0x39] = 0x02`, `AES3=0x02`). Exit code 0, 0 parser desyncs.
2. **D2 Condition 2 (44.1 kHz HDMI Input)**: Captured to [`/tmp/gc550_d2_44k1_raw.pcm`](/tmp/gc550_d2_44k1_raw.pcm) (`5,760,000` bytes, `1,440,000` frames @ `48,000 Hz`). ADV7604 decoded rate: `44,100 Hz` (`HDMI[0x39] = 0x00`, `AES3=0x00`). Exit code 0, 0 parser desyncs.

#### Key Audio Findings (`Confirmed`)
- **Fixed 48 kHz ALSA Capture Success**: ALSA capture succeeded at `48,000 Hz` stereo `S16_LE` under both `48 kHz` and `44.1 kHz` HDMI input conditions. The driver performs no software resampling.
- **Audio Transport Continuity**: No audio discontinuities or transport repetitions were detected in the analyzed 30-second captures (`0` cycle length anomalies `> 0.5 samples` across all `30,000` cycles in both conditions).
- **Linear Scaling and Duplicate Samples**: Peak amplitudes were scaled identically from reference `±24,000 LSB` down to `±22,981 LSB` (`-0.376 dBFS`). Duplicate samples occurred exclusively at turning-point apexes (`|s| >= 22,900 LSB`), matching the integer quantization of the scaled continuous sine model (`180/180` matches in D2 Condition 1, `165/165` in D2 Condition 2). Zero slope duplicates and zero zero-crossing repetitions were observed.
- **Quantization Line Reproduction**: Spectral lines at `2.9 kHz`, `4.9 kHz`, `6.8 kHz`, and `8.8 kHz` originate from 16-bit integer quantization on the Raspberry Pi `44.1 kHz` grid (`gcd(1000, 44100) = 100 Hz`) and are faithfully reproduced through the GC550 capture pipeline.
- **Residual Measurements**:
  - **D2 Condition 1**: Continuous sine-model residual ($r_{\text{cont}}[n] = s[n] - y_{\text{fit}}[n]$) is `5.460 LSB RMS` (`18.6 LSB max`), with integer-quantized residual ($r_{\text{int}}[n] = s[n] - \text{round}(y_{\text{fit}}[n])$) at `5.468 LSB RMS` (`19.0 LSB max`). Residual spectral peaks occur at `996.00 Hz` (`2.69 LSB`, `-78.64 dBc`), `998.40 Hz` (`2.83 LSB`, `-78.20 dBc`), and `1003.20 Hz` (`2.95 LSB`, `-77.82 dBc`).
  - **D2 Condition 2**: Continuous residual is `0.511 LSB RMS` (`2.0 LSB max`), integer residual is `0.584 LSB RMS` (`2.0 LSB max`), with residual spectral peaks suppressed to `0.08 LSB` (`-109.18 dBc`).
  - **Spectral Metrics (IEEE-1241-inspired measurement methodology)**: D2 Condition 1 achieved `THD = -95.37 dB`, `SNR = 92.50 dB`, `SINAD = 90.69 dB` (`ENOB = 14.77 bits`). D2 Condition 2 achieved `THD = -97.01 dB`, `SNR = 91.68 dB`, `SINAD = 90.57 dB` (`ENOB = 14.75 bits`).

#### Boundary Between Established Facts and Unproven Hypotheses
- **Confirmed Observation**: The GC550 capture pipeline delivers a continuous `48 kHz` stereo `S16_LE` stream when fed a `44.1 kHz` HDMI source, with zero driver-level resampling. Rate conversion upstream of the host ALSA interface is empirically supported by the preserved recordings.
- **Unproven Hypotheses**:
  1. Attributing the residual spectral peaks in D2 Condition 1 to ADV7604 HDMI Audio Clock Regeneration (ACR) PLL tracking jitter or loop-filter modulation is **unproven**.
  2. Attributing the lower residual in D2 Condition 2 to rate-converter FIFO decoupling from the input clock is **unproven**.
  3. The exact internal silicon architecture and physical location of the rate-conversion block (ADV7604 internal processing, an on-board DSP, or Cypress FX3 firmware) remain **unknown**.
  4. The observed `+14.2 ppm` relative clock offset is a relative measurement between two unsynchronized oscillators (Pi transmitter vs GC550 capture clock) and does not independently establish the absolute accuracy or drift of either crystal.

### 6. Confirmed `gc550.ko` ALSA Implementation (`Confirmed` Driver Code & Hardware Operation)

In [`kernel/gc550-audio.c`](../kernel/gc550-audio.c) (lines 21–37, 164–220), `gc550.ko` advertises a fixed `48 kHz` stereo 16-bit little-endian ALSA capture subdevice (`gc550_pcm_hw`):

- **Driver Format**: `SNDRV_PCM_FMTBIT_S16_LE` (`4` bytes per stereo frame).
- **Driver Rate**: Fixed `SNDRV_PCM_RATE_48000` (`rate_min = 48000`, `rate_max = 48000`).
- **Driver Channels**: `2` (`channels_min = 2`, `channels_max = 2`).
- **Hardware Alignment**: Because the GC550 hardware capture endpoint consistently delivers `48 kHz` stereo `S16_LE` samples regardless of incoming HDMI sample rate (`48 kHz` or `44.1 kHz`), dynamic ALSA sample-rate switching is unnecessary and unsupported by the hardware capture stream. The fixed `48 kHz` ALSA configuration is the empirically verified, permanent driver design.

---

## H. V4L2 Buffer Lifecycle, Streaming Concurrency, and Compliance Verification (Phases E1.4–E1.5.2)

### 1. Architectural Defects Identified & Corrected

Hardware compliance testing on Titan revealed several subtle Videobuf2 lifecycle and parser synchronization defects that were addressed across Phases E1.4 through E1.5.2.2:

1. **Frame-Boundary Buffer Reservation (`gc550_reserve_video_frame`)**:
   - *Original Defect*: In early revisions, `gc550_append_video_bytes()` acquired queued buffers lazily if `dev->cur_buf` was NULL, even after `video_pos` had already advanced past 0. This allowed buffers queued mid-frame to attach to partial frames without receiving the start of the frame.
   - *Invariant Implemented*: A video buffer may **only** be assigned to a frame at a valid `SAV` boundary when `video_pos == 0`. At each valid SAV marker, one buffer is acquired from `dev->buf_list` under `dev->queue_lock`. If none is available, the entire frame is dropped; no buffer is ever attached mid-frame.

2. **Centralized Buffer Completion Helper (`gc550_finish_buffer`)**:
   - *Original Defect*: Early revisions returned buffers with `vb2_buffer_done(..., VB2_BUF_STATE_ERROR)` on aborts without initializing `timestamp`, `sequence`, or `field`. When `v4l2-compliance -s 1` ran `testMmap()`, it dequeued an error buffer with timestamp zero, triggering an assertion failure (`v4l2-test-buffers.cpp:405: !g_timestamp().tv_sec && !g_timestamp().tv_usec`) and aborting without `STREAMOFF`, causing subsequent tests to cascade-fail with `EBUSY`.
   - *Correction*: The centralized helper `gc550_finish_buffer(dev, buf, state)` consistently initializes:
     - Monotonic nanosecond timestamp (`buf->vb.vb2_buf.timestamp = ktime_get_ns()`).
     - Field format (`buf->vb.field = V4L2_FIELD_NONE`).
     - Frame sequence (`buf->vb.sequence = p->current_frame_sequence`).
     - Plane payload (`vb2_set_plane_payload()`).
     - Videobuf2 completion (`vb2_buffer_done(&buf->vb.vb2_buf, state)`).

3. **Frame-Interval Sequence Counting**:
   - *Semantics*: V4L2 sequence numbers track received frame intervals, including frames dropped because userspace had no queued buffers.
   - *Implementation*: `p->current_frame_sequence = p->frame_sequence++;` executes at every valid SAV boundary. If no buffer is available, `p->frame_sequence` still increments. Aborted frames retain the sequence assigned at their SAV boundary. `gc550_start_streaming()` resets sequences to 0, while mid-stream parser resyncs (`gc550_parser_reset()`) preserve sequence monotonicity.

4. **Zero-Byte ERROR Payload Policy & VB2 Virtual Address Validation**:
   - *Payload Policy*: If an incomplete frame aborts with `video_pos == 0`, reporting 0 bytes payload or leaving stale memory can fail compliance assertions. When `video_pos == 0`, `gc550_finish_buffer()` writes one 4-byte neutral black YUYV macro-pixel (`gc550_yuyv_black` = `{ 16, 128, 16, 128 }`) to plane 0 and sets payload to 4 bytes if `vbuf != NULL` (and 0 if NULL), satisfying compliance without full-frame memset overhead.
   - *Address Validation*: In [`gc550_buf_prepare()`](../kernel/gc550-video.c), `vb2_plane_vaddr(vb, 0)` is validated at queue time; buffers without an accessible kernel virtual mapping are rejected immediately with `-EFAULT`.

### 2. Titan Bare-Metal Hardware Verification (Phase E1.5.2)

- **Test Platform**: Titan (AMD Ryzen 9 5900X, Radeon RX 9070 XT, Matisse xHCI controller `0000:06:00.1`, CachyOS Linux `7.3.3-1-cachyos`, x86_64).
- **Module Build**: DKMS package `avermedia-gc550-dkms` (version `0.1.3`, PKCS#7 signed, compiled with Clang/LLVM and `W=1` with zero warnings/errors). Active module `srcversion: 27D8CAC60C62239C91B8BFC`.
- **Non-Streaming V4L2 Compliance (`v4l2-compliance -d /dev/video2`)**:
  - **48/48 passed, 0 failures, 0 warnings**.
  - Verified: `VIDIOC_QUERYCAP`, priority, format enumeration, parameter queries, `VIDIOC_REQBUFS`, `CREATE_BUFS`, `VIDIOC_REMOVE_BUFS`, and `EXPBUF`.
- **Full Streaming V4L2 Compliance (`v4l2-compliance -d /dev/video2 -s 1`)**:
  - **57/57 passed, 0 failures, 0 warnings**.
  - `test read/write`: OK
  - `test MMAP (no poll, REQBUFS)`: OK
  - `test MMAP (select, REQBUFS)`: OK
  - `test MMAP (epoll, REQBUFS)`: OK
  - `test MMAP (no poll, CREATE_BUFS)`: OK
  - `test MMAP (select, CREATE_BUFS)`: OK
  - `test MMAP (epoll, CREATE_BUFS)`: OK
  - `test USERPTR (no poll)`: OK
  - `test USERPTR (select)`: OK
  - `test DMABUF`: Cannot test, specify `--expbuf-device` (pending).

### 3. Verification Boundaries & Pending Work

To maintain strict scientific and engineering rigor, verified findings are distinguished from unverified items:

- **Verified on Hardware**:
  - `CREATE_BUFS` dynamic buffer allocation and format compliance.
  - Strict SAV-boundary frame reservation (no mid-frame buffer attachment).
  - Monotonic nanosecond timestamping (`ktime_get_ns()`) for both DONE and ERROR completions.
  - Correct sequence number advancement on dropped frames and preservation on aborted frames.
  - Full streaming compliance across MMAP and USERPTR under poll, select, and epoll.
  - Long-duration 30-minute endurance capture (Phase E1.6 TE-01 on Titan: 108,000 frames dequeued, 108,001 driver-observed, 0 dropped frames, 0 mid-stream desyncs over 1,799.985 s at 60.001 fps, 345,598,308 audio bytes parsed).
  - Multi-resolution functional smoke tests (Phase E1.6 TR-01 to TR-05 on Titan: 1080p60 [300 frames], 720p60 [62 frames], 576p50 [51 frames], 480p60 [60 frames], 640x480p60 [62 frames] verified with dynamically matched Raspberry Pi HDMI source modes; all five modes demonstrated successful short captures without mid-stream errors, though four did not meet the originally planned 300-frame test duration).
  - Simultaneous video streaming and ALSA audio capture stability (Phase E1.6 TS-01 on Titan: 30-second continuous capture, 1,800 video frames at 60.00 fps, 5,750,784 PCM bytes at 48 kHz stereo [~29.952 s], demonstrating 99.84% audio/video duration correspondence during simultaneous capture without independent lip-sync measurement).
- **Pending Follow-up Work (Not Claimed as Verified)**:
  - Hot unplug/replug during active V4L2/ALSA streaming on hardware (Phase E1.6 TH-01/TH-02 marked NOT TESTED for physical cable disconnect/reconnect; software interface unbind/bind was evaluated instead).
  - DMABUF streaming verification with an external exporting device (`--expbuf-device`).

---

## I. Stream Parser Swap Lifecycle & Phase E1.7 Investigation

### 1. Context and Problem Statement
During multi-resolution testing and warm stream restarts, the driver parser exhibited a state where every incoming frame triggered parser desynchronization (`desync_count++`). In one transition, `2,074` frames resulted in `2,077` desynchronization events, preventing valid buffer delivery to userspace until the driver interface was rebound.

Source code audit identified that `dev->parser.swap_mode` and `dev->parser.swap_votes` are initialized only once during driver probe (`kzalloc()`). When a stream stops with `swap_mode == 1`, that state is retained across STREAMOFF. If a subsequent stream starts while the incoming hardware block order is in normal order, the driver un-swaps normal 16 KiB blocks, corrupting SAV/EAV boundaries. Because full frames are assembled (`video_pos > GC550_SWAP_SPAN_LIMIT`), the two-vote detector unconditionally resets `swap_votes = 0`, creating an asymmetric one-way condition where `swap_mode` cannot toggle back to 0.

### 2. Phase E1.7 Implementation (Tested & Rejected)
To test whether resetting swap state at stream startup resolves this issue, an experimental change was implemented in [`gc550_start_streaming()`](../kernel/gc550-video.c):

```c
dev->parser.swap_mode = false;
dev->parser.swap_votes = 0;
```

- **Experimental Module**: Compiled with Clang/LLVM `W=1` on Titan (`7.3.3-1-cachyos`, x86_64), producing active experimental module `srcversion: A68A3199829648981D6FDB3`.
- **Test RG-01 (Cold STREAMON)**: **Passed**. 120 frames captured at 60.02 fps; dmesg logged initial `swap_mode=0`, detected `FX3 16 KiB DMA buffer order switched to SWAPPED` at frame 2, and finished with exactly 2 initial detection desyncs and 0 subsequent desyncs (`frames=124, audio_bytes=396792, desyncs=2, swap_mode=1`).
- **Test RG-02 (Consecutive STREAMOFF/STREAMON Cycles)**: **Failed on Cycle 2**.
  - Cycle 1 completed cleanly (60 frames captured, `frames=64, audio_bytes=204800, desyncs=4, swap_mode=1`).
  - Cycle 2 (initiated 550 ms after Cycle 1 completed) started at `[26802.100070]`. At `[26802.124571]` (24 ms later), the driver logged `FX3 16 KiB DMA buffer order switched to SWAPPED`.
  - However, userspace `v4l2-ctl` blocked in `VIDIOC_DQBUF` and dequeued **zero frames**.
  - When interrupted after ~98 seconds, the kernel reported:
    `Stream stopped: frames=64, audio_bytes=204796, desyncs=5433, swap_mode=1`.
  - In ~98 seconds, only 64 SAV markers were recognized (`frames=64`), approximately 204,796 audio bytes were parsed, and 5,433 desynchronization events were logged.
- **Halted Tests**: Tests RG-03, RG-04, and RG-05 were not run due to early termination on RG-02 failure.
- **Rollback**: Restored verified E1.5.2.2 module from backup (`/home/gengartv/gc550_backup_e1.5.2/gc550.ko.zst`, SHA256 `b795b207d573eaadbe23f553d25dca40a29ba5d03898b148d38c689fffa7bb97`, `srcversion: 27D8CAC60C62239C91B8BFC`, git commit `8744e86`). Verified device availability on `/dev/video2` and ALSA card 2.
- **Conclusion**: The unconditional STREAMON reset approach is **tested and rejected**.

### 3. Established Facts vs. Unproven Hypotheses

To maintain documentation integrity, observed measurements are strictly separated from hypotheses:

- **Confirmed Observations**:
  1. Resetting `swap_mode = false` at STREAMON caused rapid consecutive streaming restarts (Cycle 2, 550 ms after Cycle 1) to fail completely with 0 dequeued frames, 64 SAV frames parsed, ~204,796 audio bytes, and 5,433 desynchronization events.
  2. The two-vote detector logged `FX3 16 KiB DMA buffer order switched to SWAPPED` 24 ms after Cycle 2 started, yet frame assembly failed.
  3. Under the baseline E1.5.2.2 module (`27D8CAC60C62239C91B8BFC`), long-duration streaming (108,000 frames / 30 minutes) runs with zero desyncs and zero frame drops once synchronized.
- **Unproven Hypotheses**:
  1. *FX3 DMA socket state persistence*: The hypothesis that Cypress FX3 DMA socket phase or FIFO alignment persists across STREAMOFF/STREAMON on warm restarts is a plausible hypothesis, but is **not a confirmed explanation**.
  2. *Hardware delivered frame count*: The estimate of ~5,880 hardware frames delivered during the 98-second failure was a theoretical calculation ($98 \text{ s} \times 60 \text{ fps}$), **not a measured fact**. The only measured values are `frames=64` (SAV markers recognized), `audio_bytes=204796`, and `desyncs=5433`.

### 4. Phase E1.8 Technical Investigation Findings

Following the failure of Phase E1.7, a strictly read-only technical investigation was performed across [`kernel/gc550-video.c`](../kernel/gc550-video.c), [`kernel/gc550.h`](../kernel/gc550.h), [`kernel/gc550-core.c`](../kernel/gc550-core.c), and the upstream reference implementation [`ChrisAJS/lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace) (`src/liblgx/src/BufferReorderer.{h,cpp}`, `FrameBuilder.{h,cpp}` at commit `908a1ce3`).

#### A. Stream Lifecycle and State Persistence
1. **STREAMON Reset Scope**: [`gc550_parser_reset()`](../kernel/gc550-video.c) resets `state = IDLE`, `sav_b0 = 0`, `hdr_pos = 0`, `audio_rem = 0`, `video_pos = 0`, `marker_stage = 0`, and `pair_pos = 0`. Stream counters (`frame_sequence`, `current_frame_sequence`, `audio_bytes`) are zeroed in `gc550_start_streaming()`.
2. **Persistent State Across Streams**: In the verified E1.5.2.2 baseline, `swap_mode`, `swap_votes`, and `desync_count` are **never reset at STREAMON or STREAMOFF** (`Confirmed`). They survive across streaming sessions and persist until module unload.
3. **Buffer Allocation Lifecycle**: `pair_buf` (32 KiB) is dynamically allocated at STREAMON (`kmalloc`) and freed at STREAMOFF (`kfree`). All four 512 KiB streaming URBs are allocated and submitted at STREAMON, and killed (`usb_kill_urb`) and freed at STREAMOFF.
4. **Driver-Side STREAMOFF Inaction**: [`gc550_stop_stream_resources()`](../kernel/gc550-video.c) issues **no explicit FX3 stop, DMA reset, or endpoint FIFO flush command** (`Confirmed`). Whether the FX3 continues acquiring data, retains committed DMA buffers, or preserves socket phase across the pause remains `Unknown` without bus-level capture.

#### B. 16 KiB Pairing Grid and Mid-URB Mode Transitions
1. **Unanchored Pairing Grid**: In `gc550_parse_stream_chunk()`, when `swap_mode == true`, incoming bytes are accumulated into `pair_buf` starting at index `pair_pos = 0`. The driver makes an implicit assumption that byte 0 of the first URB received after STREAMON is byte 0 of an even 16 KiB DMA block (`Confirmed`). The kernel driver possesses **no alignment mechanism** to synchronize `pair_pos` with physical 16 KiB DMA blocks or SAV frame markers.
2. **Mid-URB State Transition**: When the two-vote detector confirms an order switch (`swap_votes >= 2`) inside `gc550_complete_video_frame()`, line 230 toggles `p->swap_mode = !p->swap_mode;` and zeroes `p->pair_pos = 0;`. This occurs deep inside the byte-by-byte loop of `gc550_parse_ordered_bytes()`.
   - **Crucial Finding**: `gc550_parse_ordered_bytes()` continues executing for the **remainder of that active 512 KiB URB** (`Confirmed`). The remaining ~476 KiB of that URB are parsed directly without 32 KiB pair accumulation or swapping.
   - Only upon arrival of the *subsequent* URB does `gc550_parse_stream_chunk()` accumulate into `pair_buf`.
3. **Absence of Downstream Reset**: The kernel driver issues no downstream framing reset or gap notification when `swap_mode` toggles (`Confirmed`). In-flight marker prefixes (`marker_stage`) and active frame states continue uninterrupted.

#### C. Upstream Reference Implementation Comparison (`ChrisAJS/lgx2userspace`)
A detailed source audit of `ChrisAJS/lgx2userspace` (`BufferReorderer` and `FrameBuilder`) revealed fundamental architectural differences:

| Architectural Feature | Upstream Reference (`ChrisAJS/lgx2userspace`) | Kernel Driver (`lgx2KernelLevel`) | Evidence Level |
|---|---|---|---|
| **Pipeline Separation** | Decoupled 2-stage pipeline: `BufferReorderer` (stream reordering) $\to$ `FrameBuilder` (frame parsing). | Monolithic byte-stream parser with inline pair accumulator (`gc550_parse_stream_chunk`). | `Confirmed` |
| **Grid Anchoring** | **Explicitly anchored to SAV offset**: `_pairOrigin = observation.savOffset` (`BufferReorderer.cpp:120`). | **Unanchored**: Assumes byte 0 of host URB is Block 0 offset 0 (`pair_pos = 0`). | `Confirmed` |
| **Startup Leading Bytes** | **Dropped**: All bytes prior to the first decided SAV are discarded (`_outOffset = end` in `Order::Unknown`). | **Parsed**: All bytes from URB 0 byte 0 are parsed directly into the state machine. | `Confirmed` |
| **Normal-Order Detection** | **Proactive timer/window**: If 32 KiB elapses after SAV without an EAV, `Order::Normal` is confirmed. | **None**: Assumes normal mode until an error/swap condition triggers. | `Confirmed` |
| **Bidirectional Recovery** | **Full**: Transitions `Normal -> Swapped` and `Swapped -> Normal` using 2-SAV agreeing candidate logic. | **One-way trap**: Cannot transition `Swapped -> Normal` because normal spans ($> 32$ KiB) zero votes. | `Confirmed` |
| **Order Transition Boundary** | **Strictly at SAV boundary**: Applied only when emit offset equals deciding SAV offset. | **Mid-URB**: Applied immediately inside `complete_video_frame`; remainder of URB parsed unswapped. | `Confirmed` |
| **Downstream Resync on Order Change** | **Explicit gap notification**: Downstream `FrameBuilder` resets state, clears carry, and disarms (`resetForGap`). | **None**: Parser state machine continues running inline without carry/straddle reset. | `Confirmed` |
| **FrameBuilder Re-Arming Discrepancy** | Header comment claims clean SAV/EAV with matching `c0 == b0` required; code actually arms (`_armed = true`) on valid checksum alone and treats `c[0] != _savB0` as soft mismatch (`FrameBuilder.cpp:152`). | Driver immediately aborts vb2 buffer with `VB2_BUF_STATE_ERROR` on `c0 != b0` when span $\le 32$ KiB. | `Confirmed` |

#### D. Multi-Resolution Compatibility and Phase Drift Risks
1. **Upstream Invariant**: `BufferReorderer.h` explicitly assumes an even number of 16 KiB DMA blocks per frame period (1080p: 254 blocks). In `BufferReorderer::agrees()`, upstream enforces `(observation.savOffset - pairOrigin) % PAIR_BYTES == 0`.
2. **Analysis for Supported Resolutions**:
   - **1080p** ($1920 \times 1080$): Padded period is $4,161,536\text{ B} = 254 \times 16,384\text{ B} = 127 \text{ pairs}$ (EVEN). Phase drift is zero.
   - **720p, 576p, 480p, 640x480**: Video payloads are not integer multiples of 32 KiB ($112.5$, $50.625$, $42.1875$, and $37.5$ blocks). Whether the FX3 pads these modes to an **even** or **odd** number of 16 KiB blocks is unmeasured on hardware (`Unknown`).
3. **Phase Drift Consequence**: If a resolution has an odd number of 16 KiB blocks per frame period ($2k+1$), the SAV marker will alternate between Block 0 and Block 1 on every single frame. A naive port of upstream's SAV-grid agreement check would conclude that phase moved on every frame, causing continuous pipeline resets. The upstream algorithm cannot be ported unchanged without qualification.

#### E. Analysis of the E1.7 Failure on Cycle 2
1. **Minimum State Transitions Inferred**: In Cycle 2, dmesg logged `FX3 16 KiB DMA buffer order switched to SWAPPED` at `[26802.124571]` and again at `[26899.272523]`. Because line 230 toggles `p->swap_mode = !p->swap_mode;`, a second print of `SWAPPED` proves that a **minimum of two state transitions** (`SWAPPED -> NORMAL -> SWAPPED`) occurred, requiring at least 4 qualifying observation votes (`Confirmed`). The exact total number of transitions was masked by printk ratelimiting (`Unknown`).
2. **URB Completion Lengths in E1.7**: The runtime values of `urb->actual_length` for Cycle 1 and Cycle 2 were not captured because `dev_dbg` was not enabled (`Unknown`).
3. **Audio Sample Count Correction**: The 4-byte difference between Cycle 1 (`audio_bytes = 204800`) and Cycle 2 (`audio_bytes = 204796`) represents exactly one 16-bit stereo PCM sample pushed to ALSA, not raw USB transfer bytes or FIFO occupancy. It cannot be used as evidence for physical FIFO residue.

---

### 5. Phase E1.9 Passive Diagnostic Investigation Plan

To resolve the remaining unknowns without altering driver behavior or introducing timing jitter, a passive, non-intrusive diagnostic instrumentation specification is established for future implementation.

#### A. Diagnostic Capture Targets
The diagnostic collector will record:
1. **Stream Cycle Counter**: Incremented on each STREAMON invocation.
2. **URB Monotonic Completion Sequence & Slot Index**: Distinguishes the completion order ($0, 1, 2, \dots$) from the fixed hardware URB slot ($0..3$).
3. **URB Status & Runtime `actual_length`**: Captures short transfers or USB errors.
4. **Cumulative Raw USB Stream Byte Offset**: $\Sigma \text{urb->actual\_length}$ from stream start.
5. **Initial SAV Observations**: Absolute raw stream offset, relative URB offset, and `b0` sequence byte for at least the first 8 checksum-valid SAV markers per stream.
6. **Qualifying Observation Events**: Exact byte offsets, span lengths (`video_pos`), and sequence comparison (`b0` vs `c0`).
7. **Swap Transitions**: `swap_mode` before/after, `swap_votes`, `pair_pos` before/after, and a flag indicating whether the transition occurred while inside `gc550_parse_ordered_bytes()`.
8. **Offset Domain Separation**: Explicit separation of raw physical USB offsets from logical post-swap parser offsets.

#### B. Concurrency, Ring-Buffer Design, and Overhead Elimination
- **Pre-Allocated Memory**: Diagnostic ring buffer (`struct gc550_diag_event[64]`) allocated in `gc550_dev` during device probe; zero memory allocation in the streaming path.
- **Lockless Capture**: Event index advanced via `atomic_inc_return()`. Write operations access distinct pre-allocated slots; no mutexes or spinlocks acquired in the hot path.
- **Bounded Retention & Overflow**: The ring buffer records the first 64 significant events per stream. Once full, an atomic overflow counter increments; further events are dropped to prevent buffer overwrites.
- **Zero Console I/O During Streaming**: No `printk` or `dev_dbg` calls are made during active video processing. At STREAMOFF, [`gc550_stop_streaming()`](../kernel/gc550-video.c) formats and dumps the diagnostic log in a single deferred summary block.
- **Primary Discriminating Measurement**: The single highest-value metric is the **exact byte offset of the very first checksum-valid SAV marker relative to `urb->transfer_buffer[0]` on Cycle 2, combined with `urb->actual_length` of that initial transfer**. This distinguishes USB FIFO residue (short packet or unaligned offset) from hardware socket phase persistence (SAV aligned to offset 0 vs 16,384).

---

## G. Current Knowledge and Unresolved Questions

The table below summarizes the established reverse-engineering findings, their evidence sources, their confidence classifications, and the remaining open questions for future kernel driver work.

| Finding | Evidence / Source | Confidence | Remaining Uncertainty |
|---|---|---|---|
| **USB Device IDs & Endpoints**: `07ca:4710` (primary) and `07ca:0550` (secondary) expose Interface `0` (`Alt 0`, class `0xFF`) with `EP 0x01 OUT` (commands), `EP 0x81 IN` (command replies), and `EP 0x83 IN` (capture stream). | USB descriptors, [`PROTOCOL.md`](../PROTOCOL.md) line 912, [`kernel/gc550.h`](../kernel/gc550.h), [`kernel/gc550-core.c`](../kernel/gc550-core.c), runtime testing on `07ca:4710`. | `Confirmed` | Exact roles of unused endpoints `EP 0x02 OUT`, `EP 0x82 IN`, and `EP 0x03 OUT` on GC550 hardware. |
| **Destructive Nature of Concurrent Userspace USB Access**: Reading `EP 0x81 IN` or `EP 0x83 IN` (or claiming Interface `0`) from userspace while `gc550.ko` is active steals FIFO replies/stream chunks and corrupts driver state. | USB 3.0 Bulk endpoint semantics, [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c), [`kernel/gc550-video.c`](../kernel/gc550-video.c). | `Confirmed` | Whether future hardware diagnostics should be exposed via kernel debugfs/V4L2 controls serialized under `dev->cmd_lock` rather than userspace `libusb`. |
| **FX3 Command Protocol (`0x01` I2C Write, `0x02` I2C Read)**: `0x01` is `[0x01, slave, 0x01, reg, data[0..len-1]]` (`tx_len = 5..36` B, `rx_len = 0`); `0x02` is `[0x02, slave, 0x01, read_len, reg]` (`tx_len = 5` B, `rx_len = read_len`). | `avmu3_x64.sys` (`FUN_1400199cc`), [`PROTOCOL.md`](../PROTOCOL.md) lines 415–418, [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) lines 145–197, all `6,574` I2C steps in `gc550_init_blob`. | `Confirmed` | Exact FX3 internal firmware timeout and I2C bus recovery behaviour if an internal I2C slave stretches clocks or NACKs while `EP 0x83 IN` is streaming. |
| **ADV7604 Sub-Map Registers (`IO[0xF3..0xFE]`) & Target `0x46`**: `IO[0xF5]=0x8C` (`0x46` InfoFrame), `IO[0xF8]=0x4C` (`0x26` AFE), `IO[0xF9]=0x64` (`0x32` REP), `IO[0xFA]=0x6C` (`0x36` EDID), `IO[0xFB]=0x68` (`0x34` HDMI), `IO[0xFD]=0x44` (`0x22` CP). | [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) (`steps 79..101`, `351..373`), `<media/i2c/adv7604.h>`, [`PROTOCOL.md`](../PROTOCOL.md) lines 439–440, 576. | `Confirmed` | Whether `IO[0xF6] = 0x70` (7-bit `0x38` ESDP map, powered down via `IO[0x0B]=0x44`) has any bus conflict with the `0x38` write at `step 2,464` (ADV7511 packet memory). |
| **CryptoMemory Opcodes (`0x05`, `0x06`, `0x07`, `0x09`)**: Used by `CryptoAT88::*` in `avmu3_x64.sys` with command bytes `0xB2..0xBA`; absent from `gc550_init_blob` and blocked in `gc550_cmd_send()`. | `avmu3_x64.sys` decompilation ([`PROTOCOL.md`](../PROTOCOL.md) lines 502–555), [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) lines 77–85, offline scan of `gc550_init_blob`. | `Confirmed` (for `0x05`, `0x06`, `0x07`, `0x09`) | Because the FX3 firmware binary is not disassembled, blocking top-level opcodes `0x05`/`0x06`/`0x07`/`0x09` does **not** prove that no other FX3 opcode or internal firmware path could ever interact with the CryptoMemory bus. |
| **FX3 Opcodes `0x08` and `0x0E` in `gc550_init_blob`**: Step `0` executes `0e 02 07 fe 02 00` (`512` B read); Step `1` executes `08` (`1` B write). | [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) line 37, [`PROTOCOL.md`](../PROTOCOL.md) lines 793–803. | `Confirmed` (wire format) / `Inferred` (`0x0E` reads EEPROM) | Exact internal action performed by FX3 firmware upon receiving `0x08` (and `0x14` on GC551) is `Unknown` without FX3 firmware disassembly. |
| **Initialization Table (`gc550_init_blob`) Structure**: `53,146` bytes containing `6,576` steps (`1,853` × `0x01`, `4,721` × `0x02`, `1` × `0x08`, `1` × `0x0E`). | [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h), [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c), offline structural validator. | `Confirmed` | Approximately `4,800` of the `6,576` steps are periodic status-polling reads recorded in the original Windows USB capture; minimal cold-init subset reduction has not been tested on GC550 hardware. |
| **Warm Board-State Detection & Auto Colorspace Fixup**: `gc550_detect_board_state()` checks `ADV7604 IO[0xFA] == 0x6C` (the correct EDID map register and value) and `PCA9539A[0x06..0x07] == {0x00, 0x09}`; `gc550_set_auto_input_colorspace()` sets `IO[0x02]` bits `7:4 = 0xF` (`0x1C -> 0xFC`) and resets `CP[0x3A..0x3D] = {0x80, 0x80, 0x00, 0x00}`. | [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c) lines 212–270, `gc550_init_blob` (`steps 91, 363, 5989`), runtime testing on GC550. | `Confirmed` | **Unresolved driver reliability issue**: checking `IO[0xFA] == 0x6C` and `PCA9539A[0x06..0x07] == {0x00, 0x09}` cannot distinguish a fully completed `6,576`-step bootstrap from a previously interrupted bootstrap that failed after `step 363`. |
| **Capture Stream Framing & Runtime Validation**: `SAV` (`00 FF FF C0` + 4B), `EAV` (`00 FF FF C1` + 5B), `AUDIO` (`00 FF FF 58` + BE32 len), `AA 55 55 AA` filler. Five sizes in `gc550_frame_sizes[]`: `1920×1080`, `1280×720`, `720×576`, `720×480`, `640×480`. | [`PROTOCOL.md`](../PROTOCOL.md), [`kernel/gc550-video.c`](../kernel/gc550-video.c), [`CHANGELOG.md`](../CHANGELOG.md), [`README.md`](../README.md), [`TODO.md`](../TODO.md). | `Confirmed` | Multi-resolution short smoke tests (50–300 frames) verified on hardware; long-duration endurance testing was performed only at 1080p60 on Titan (108,000 frames) and Niri (776,295 frames). V4L2 interval enumeration currently hardcodes `60 fps` for all sizes. |
| **Kernel Stream Parser vs. Offline Python `StreamParser`**: Both implement `marker_stage` `0..3`, 2-vote `swap_mode` (`video_pos <= 32768 && eav_c0 != sav_b0`), `32 KiB` `pair_buf` reordering, `4,163,585` B span saturation, and `±0x4000` span tolerance, but differ in `vb2` buffer copying, `{16, 128, 16, 128}` tail padding, `frame_sequence` gating (`if (done_buf)`), and ALSA delivery. | [`kernel/gc550-video.c`](../kernel/gc550-video.c), `scratch/gc550_stage4a_probe.py`. | `Confirmed` | All 7 unit tests in `TestStage4AProbeOffline` cover **only** the offline Python model on synthetic byte arrays; the kernel's `gc550_yuyv_black` short-frame tail fill has not yet been triggered by a natural short span on hardware ([`TODO.md`](../TODO.md) line 18). |
| **Programmed EDID (`0x36`) & `REP[0x77]` State in `gc550_init_blob`**: `steps 286..293` write the 256-byte `"ADI HDMI TV"` EDID (`0x91..0xA3` Audio Data Block, header `0x32`, LPCM SAD #1 `0f 7f 07` = 8-ch, `32..192 kHz`, `16/20/24-bit`, plus AC-3, DTS, E-AC-3, DTS-HD, TrueHD), then `steps 309..317` sequence `REP[0x77]` `01 -> 03 -> 07 -> 0F -> 00`. | [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h) lines 180–200, [`PROTOCOL.md`](../PROTOCOL.md) line 686. | `Confirmed` (bytes & `REP[0x77]=0x00` in `gc550_init_blob`) / `Unknown` (runtime DDC source) | The final `REP[0x77] = 0x00` write at `step 317` disables all four internal ADV7604 EDID ports (`bits 3:0`), and `steps 326..349` pulse the `PCA9539A` reset line a second time and rewrite `IO[0xF3..0xFE]` (`steps 351..373`) **without** rewriting `0x36` or `REP[0x77]`. What EDID the HDMI source actually receives on GC550 hardware is unresolved. |
| **Audio Transport & Sample-Rate Handling (Phases B2, C1–C3, D1, D2.1, D2.2.7)**: Fixed `48 kHz` stereo `S16_LE` ALSA capture confirmed on Titan under both `48,000 Hz` (D2 Condition 1) and `44,100 Hz` (D2 Condition 2) HDMI input. ADV7604 correctly reports HDMI input rate (`IO[0x65]`, `HDMI[0x39]`). Audio continuity verified across 30-second captures (`0` anomalies `> 0.5 samples`). Signal converts from `44.1 kHz` to `48 kHz` upstream of host USB capture. Duplicate samples occur exclusively at turning-point apexes (`|s| >= 22,900 LSB`). | [`kernel/gc550-video.c`](../kernel/gc550-video.c), [`kernel/gc550-audio.c`](../kernel/gc550-audio.c), [`kernel/gc550-cmd.c`](../kernel/gc550-cmd.c), Titan hardware captures C1/C2, offline analysis script `verify_d2_audio.py`. | `Confirmed` (Fixed `48 kHz` `S16_LE` capture, ADV7604 rate decoding, transport continuity) / `Unknown` (Internal silicon conversion block) | Fixed `48 kHz` ALSA capture configuration confirmed permanent; dynamic rate switching is unnecessary. Exact internal silicon component and conversion location (`44.1 kHz -> 48 kHz`) remain `Unknown`. |
| **Uncharacterized I2C Slaves (`0x4A`, `0x65`, `0x3F`) & FPGA (`0x10`) Registers**: Addressed in `gc550_init_blob` (`0x4A`: `125`W/`106`R; `0x65`: `4`W; `0x3F`: `8`×32B R; `0x10`: `18`W/`6`R). | [`kernel/gc550-regtables.h`](../kernel/gc550-regtables.h), [`PROTOCOL.md`](../PROTOCOL.md) lines 579–581. | `Confirmed` (I2C transactions) / `Unknown` (IC identities & register maps) | Exact IC part numbers and register definitions of `0x4A`, `0x65`, and `0x3F`, as well as the internal register map of the Lattice ECP3 FPGA (`0x10`), remain `Unknown`. |
| **V4L2 Streaming Compliance, Multi-Resolution & Endurance (Phases E1.4–E1.6)**: 100% compliance pass (57/57 streaming, 48/48 non-streaming, 0 failures, 0 warnings) on Titan (`7.3.3-1-cachyos`, `srcversion: 27D8CAC60C62239C91B8BFC`). Buffers reserve strictly at SAV boundary; centralized `gc550_finish_buffer()` provides monotonic timestamps and sequence intervals; `gc550_buf_prepare()` validates virtual addresses. Phase E1.6 verified 30-minute endurance (108,000 frames dequeued, 0 dropped, 0 mid-stream desyncs), short multi-resolution functional checks across all 5 sizes (1080p [300 frames], 720p [62], 576p [51], 480p [60], 640x480 [62]), and 30s simultaneous AV capture demonstrating 99.84% audio/video duration correspondence without independent lip-sync measurement. | `v4l2-compliance 1.32.0`, `v4l2-ctl`, `arecord` on Titan bare-metal hardware. | `Confirmed` (for 1080p MMAP/USERPTR, 30-min endurance, 5-resolution functional checks, AV duration correspondence) | Hot unplug/replug during active capture remains unverified on hardware (software unbind/bind evaluated). DMABUF export (`--expbuf-device`) remains pending. |
| **Swap State Lifecycle & Phase E1.7 Testing**: Unconditional `swap_mode = false` reset at STREAMON was tested (`A68A3199829648981D6FDB3`) and failed on rapid stream restart (RG-02 Cycle 2: 0 dequeued frames, 5,433 desyncs over ~98s, `frames=64, audio_bytes=204796`). Restored to verified baseline E1.5.2.2 (`27D8CAC60C62239C91B8BFC`, commit `8744e86`). | Hardware regression on Titan bare-metal, kernel logs, task logs. | `Confirmed` (for RG-01 pass, RG-02 failure, rollback) / `Hypothesis` (FX3 socket state persistence) | Unconditional reset at STREAMON rejected. Read-only investigation of FX3 DMA socket behavior planned for Phase E1.8. |
| **Unanchored Pairing Grid & Mid-URB Mode Transition (Phase E1.8)**: Kernel driver pair accumulation starts at host URB byte 0 without SAV anchoring; mode transitions occur mid-URB while `gc550_parse_ordered_bytes()` continues processing remainder unswapped; no downstream framing reset occurs on order change. | Source code audit of [`kernel/gc550-video.c`](../kernel/gc550-video.c) lines 220–258, 453–485. | `Confirmed` | Exact byte alignment of initial URB on warm stream restart remains `Unknown` until passive diagnostic capture is executed. |
| **Upstream `BufferReorderer` SAV-Anchored Architecture (Phase E1.8)**: Upstream reference explicitly anchors 32 KiB pair grid to `observation.savOffset`, drops leading bytes prior to decided SAV, detects normal order via 32 KiB lookahead window, transitions bidirectionally, and passes `gapped = true` to downstream `FrameBuilder`. | Source code audit of `ChrisAJS/lgx2userspace` (`BufferReorderer.{h,cpp}`, commit `908a1ce3`). | `Confirmed` | Upstream FrameBuilder arming discrepancy confirmed: header comment requires `c0 == b0`, but code arms on checksum alone and treats `c[0] != _savB0` as soft mismatch. |
| **Multi-Resolution DMA Block Parity & Phase Drift Risk (Phase E1.8)**: Upstream algorithm assumes an even number of 16 KiB blocks per frame period (1080p: 254 blocks). If lower resolutions have odd block periods, SAV alternates between Block 0 and Block 1 on alternating frames, breaking upstream's `(savOffset - pairOrigin) % 32768 == 0` check. | Mathematical analysis of frame periods, [`PROTOCOL.md`](../PROTOCOL.md), `BufferReorderer.cpp`. | `Confirmed` (for 1080p 254 even blocks) / `Inferred` (phase drift risk on odd blocks) | Exact total frame period (with padding) in 16 KiB blocks for 720p, 576p, 480p, and 640x480 is `Unknown`. |
| **STREAMOFF Hardware State & FIFO Behavior (Phase E1.8)**: Kernel driver issues no explicit FX3 stop, DMA channel reset, or FIFO flush command at STREAMOFF. Actual FX3 hardware state and FIFO occupancy during pause remain unmeasured. | [`kernel/gc550-video.c`](../kernel/gc550-video.c) lines 676–690. | `Confirmed` (driver inaction) / `Unknown` (FX3 hardware behavior) | Whether `usb_clear_halt()` on EP 0x83 safely flushes the FX3 Bulk IN FIFO without wedging endpoint state. |

