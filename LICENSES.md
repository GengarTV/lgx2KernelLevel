# Licensing and Provenance

## Project Licensing Policy

The `lgx2KernelLevel` project provides a native Linux kernel module (`gc550.ko`) for the AVerMedia Live Gamer Extreme (GC550).

### 1. Original Project Code (`GPL-2.0-only OR MIT`)

All original Linux kernel driver source files, headers, build system scripts, and packaging files authored for this project:

- **Copyright (c) 2026 Øystein Lie <oeystein@gengartv.com>**

are dual-licensed under recipients' choice of:

1. **GNU General Public License version 2 only (`GPL-2.0-only`)**, as published by the Free Software Foundation; see [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0).
2. **The MIT License (`MIT`)**; see [`LICENSE-MIT`](LICENSE-MIT).

This dual-licensing arrangement (`GPL-2.0-only OR MIT`) preserves the ability to use the project's original code permissively under MIT while ensuring full compatibility with the Linux kernel's GPL-2.0 licensing requirements.

The kernel module binary declares:
```c
MODULE_LICENSE("Dual MIT/GPL");
```
in accordance with standard Linux kernel module licensing conventions (`include/linux/module.h`), signifying a dual GNU General Public License v2 / MIT license.

---

## Upstream Provenance and Third-Party Notices

### 2. Upstream Reverse-Engineering & Research (`ChrisAJS/lgx2userspace`)

Hardware reverse-engineering, USB 3.0 protocol decoding, BT.1120 SAV/EAV checksum validation formulas, 16 KiB FX3 DMA swap discovery, and hardware initialization sequences originated from the upstream open-source project [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace):

- **Copyright 2021 Chris Sawczuk <chris@sawcz.uk> and contributors**
- Licensed under the **MIT License**; the original upstream license and copyright notice are preserved in full in [`LICENCE.md`](LICENCE.md).

All upstream copyright notices, permissions, and conditions remain intact and respected in this repository. Upstream authors have not granted additional licensing rights beyond the MIT License.

### 3. Upstream Protocol Specification (`PROTOCOL.md`)

The file [`PROTOCOL.md`](PROTOCOL.md) is preserved verbatim from `ChrisAJS/lgx2userspace` as an authoritative historical reverse-engineering and hardware specification. It is authored by Chris Sawczuk and contributors under the MIT License and is not modified by this project.

---

## Component Provenance & File Classification

The repository distinguishes original implementation code from upstream research and protocol definitions:

### Detailed Provenance Analysis:

- **`kernel/gc550.h`**:
  - *Original Implementation*: Linux kernel driver data structures (`struct gc550_dev`, `struct gc550_stream_parser`), V4L2/ALSA context, locking, prototypes written by Øystein Lie (`GPL-2.0-only OR MIT`).
  - *Upstream-Derived Material*: Register constants, endpoint definitions, marker constants, and ADV7604 sub-map offsets reverse-engineered by Chris Sawczuk under MIT in `ChrisAJS/lgx2userspace`.

- **`kernel/gc550-regtables.h`**:
  - *Original Implementation*: Reformatting, packaging into a flattened C array (`gc550_init_blob`), and runtime structure designed for the kernel driver by Øystein Lie (`GPL-2.0-only OR MIT`).
  - *Upstream-Derived Material*: The 6,576 USB-to-I2C bridge transactions, chip init values, and AT88SC anti-clone stripping reverse-engineered by Chris Sawczuk under MIT (`src/liblgx/src/devices/lgx/Commands.cpp`). Upstream MIT copyright and attribution are preserved.

- **`kernel/gc550-video.c`**:
  - *Original Implementation*: Linux V4L2 `videobuf2-vmalloc` capture pipeline, buffer reservation at SAV boundary, centralized timestamping (`ktime_get_ns()`), sequence intervals, compliance fixes, and kernel stream parser implementation by Øystein Lie (`GPL-2.0-only OR MIT`).
  - *Upstream-Derived Material*: BT.1120 SAV/EAV framing checksum formulas and 16 KiB FX3 DMA buffer reordering concept reverse-engineered by Chris Sawczuk under MIT.

- **`kernel/gc550-cmd.c`**:
  - *Original Implementation*: Synchronous USB command execution, URB allocation, mutex serialization (`dev->cmd_lock`), runtime opcode filtering, and board-state detection functions by Øystein Lie (`GPL-2.0-only OR MIT`).
  - *Upstream-Derived Material*: Cypress FX3 command protocol wire format (`0x01` write, `0x02` read), ADV7604/PCA9539A board-state register check, and CryptoMemory opcode identification (`0x05..0x07`, `0x09`) reverse-engineered by Chris Sawczuk under MIT.

- **`kernel/gc550-core.c` & `kernel/gc550-audio.c`**:
  - *Original Implementation*: USB driver lifecycle (`usb_driver`, probe, disconnect, kref), ALSA PCM capture device (`snd_card`, `snd_pcm_ops`), ring buffer pushing by Øystein Lie (`GPL-2.0-only OR MIT`).
  - *Upstream-Derived Material*: Audio packet marker (`00 FF FF 58`) and 48 kHz stereo payload format reverse-engineered by Chris Sawczuk under MIT.

---

## File Classification & Applicable Licenses

| File / Component | Primary Author(s) / Origin | Applicable License(s) | License Text File |
|---|---|---|---|
| `kernel/gc550.h` | Øystein Lie (incorporating upstream protocol definitions) | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `kernel/gc550-core.c` | Øystein Lie | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `kernel/gc550-cmd.c` | Øystein Lie (protocol researched by Chris Sawczuk) | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `kernel/gc550-video.c` | Øystein Lie (framing formulas researched by Chris Sawczuk) | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `kernel/gc550-audio.c` | Øystein Lie (packet format researched by Chris Sawczuk) | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `kernel/gc550-regtables.h` | Øystein Lie (sequences reverse-engineered by Chris Sawczuk) | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `Makefile` | Øystein Lie | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `Kbuild` | Øystein Lie | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `install.sh` | Øystein Lie | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `PKGBUILD` | Øystein Lie | `GPL-2.0-only OR MIT` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0), [`LICENSE-MIT`](LICENSE-MIT) |
| `LICENSE-MIT` | Øystein Lie | `MIT` (project license) | [`LICENSE-MIT`](LICENSE-MIT) |
| `LICENSE-GPL-2.0` | Free Software Foundation | `GPL-2.0-only` | [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0) |
| `LICENCE.md` | Chris Sawczuk | `MIT` (upstream license) | [`LICENCE.md`](LICENCE.md) |
| `PROTOCOL.md` | Chris Sawczuk and contributors | `MIT` (preserved upstream) | [`PROTOCOL.md`](PROTOCOL.md) |
