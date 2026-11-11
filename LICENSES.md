# Licensing and Provenance

## Project Licensing Policy

The `lgx2KernelLevel` project provides a native Linux kernel module (`gc550.ko`) for the AVerMedia Live Gamer Extreme (GC550).

### 1. Original Project Code (`GPL-2.0-only OR MIT`)

All original Linux kernel driver source files, headers, build system scripts, and packaging files authored for this project:

- **Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>**

are dual-licensed under recipients' choice of:

1. **GNU General Public License version 2 only (`GPL-2.0-only`)**, as published by the Free Software Foundation; see [`LICENSE-GPL-2.0`](LICENSE-GPL-2.0).
2. **The MIT License (`MIT`)**; see [`LICENCE.md`](LICENCE.md).

This dual-licensing arrangement (`GPL-2.0-only OR MIT`) preserves the ability to use the code permissively under MIT while ensuring full compatibility with the Linux kernel's GPL-2.0 licensing model.

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
- Licensed under the **MIT License**; the original license and copyright notice are preserved in full in [`LICENCE.md`](LICENCE.md).

All upstream copyright notices, permissions, and conditions remain intact and respected in this repository. Upstream authors have not granted additional licensing rights beyond the MIT License.

### 3. Upstream Protocol Specification (`PROTOCOL.md`)

The file [`PROTOCOL.md`](PROTOCOL.md) is preserved verbatim from `ChrisAJS/lgx2userspace` as an authoritative historical reverse-engineering and hardware specification. It is authored by Chris Sawczuk and contributors under the MIT License and is not modified by this project.

---

## File Classification & Applicable Licenses

| File / Component | Primary Author(s) / Origin | Applicable License(s) |
|---|---|---|
| `kernel/gc550.h` | Øystein Lie (incorporating upstream protocol definitions) | `GPL-2.0-only OR MIT` |
| `kernel/gc550-core.c` | Øystein Lie | `GPL-2.0-only OR MIT` |
| `kernel/gc550-cmd.c` | Øystein Lie (protocol researched by Chris Sawczuk) | `GPL-2.0-only OR MIT` |
| `kernel/gc550-video.c` | Øystein Lie (framing formulas researched by Chris Sawczuk) | `GPL-2.0-only OR MIT` |
| `kernel/gc550-audio.c` | Øystein Lie (packet format researched by Chris Sawczuk) | `GPL-2.0-only OR MIT` |
| `kernel/gc550-regtables.h` | Øystein Lie (initialization sequences reverse-engineered by Chris Sawczuk) | `GPL-2.0-only OR MIT` |
| `Makefile` | Øystein Lie | `GPL-2.0-only OR MIT` |
| `Kbuild` | Øystein Lie | `GPL-2.0-only OR MIT` |
| `install.sh` | Øystein Lie | `GPL-2.0-only OR MIT` |
| `PKGBUILD` | Øystein Lie | `GPL-2.0-only OR MIT` |
| `LICENCE.md` | Chris Sawczuk | `MIT` (upstream license) |
| `LICENSE-GPL-2.0` | Free Software Foundation | `GPL-2.0-only` |
| `PROTOCOL.md` | Chris Sawczuk and contributors | `MIT` (preserved upstream) |
