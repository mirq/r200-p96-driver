# Radeon9200.chip for Prometheus.card

Pure-C Picasso96 chip driver for desktop RV280 Radeon 9200 PCI boards behind
`Prometheus.card`.

## Status

The current library is `Radeon9200.chip 3.0` and exposes Radeon3D interface 13,
adding client-written VRAM streaming segments and batched semantic commits to
the hardware TCL path.
It has been validated on a physical 50 MHz 68060 Amiga with a
Prometheus/FireBird bridge, an RV280 `1002:5964` 128 MiB COMBIOS board, and a
64 MiB linear aperture. Supported device IDs are `1002:5960`, `1002:5961`, and
`1002:5964`.

Implemented display and 2D features include:

- CRTC0 output through the primary VGA DAC and the validated internal-TMDS DVI
  route. The DVI path has driven a stable 1920x1080 Workbench at up to the
  165 MHz single-link limit.
- CLUT8, little-endian RGB565 (`RGBFB_R5G6B5PC`), and four-byte BGRA/XRGB8888
  (`RGBFB_B8G8R8A8`) scanout, with panning, palette, blanking, vertical sync,
  and DPMS callbacks.
- Bounded ROM selection, legacy COMBIOS parsing, cold-card initialization, and
  endian-safe mapped MMIO.
- Hardware `FillRect`, `InvertRect`, `BlitRect`, `DrawLine`, JAM1/JAM2
  `BlitTemplate`, constrained JAM2 `BlitPattern`, and
  `BlitRectNoMaskComplete` for all 16 P96 minterms. Copies validate complete
  source and destination extents and select a safe direction for overlapping
  on-board ranges.
- A 64x64 ARGB hardware cursor, enabled by default.
- A page-aligned shared Prometheus DMA arena at the high end of VRAM, separate
  from Radeon-private CP and cursor storage.
- Bounded command-processor submission, recovery, fences, imported P96
  surfaces, and semantic 3D execution. The public contract is documented in
  [`RADEON3D_SUBMISSION.md`](RADEON3D_SUBMISSION.md); physical validation is
  recorded chronologically in [`R200_3D_PROGRESS.md`](R200_3D_PROGRESS.md).

Hardware acceleration falls back to the P96 defaults when an operation or
surface cannot be represented safely. Timeout recovery invalidates current 3D
sessions, resets the engine, reloads and self-tests the CP, and re-arms safe
acceleration.

## Build

```sh
make clean && make
make tools
make abi-check
make r3d-tools
```

The default cross-compiler prefix is `/opt/amiga/bin/m68k-amigaos-`. A normal
build produces the matched `Radeon9200.chip` and `Prometheus.card` files in the
repository root. Warnings are treated as errors for the chip and tools.

Other configurations use independent object trees and output names:

```sh
make DEBUG=1       # serial logging and Radeon9200.Debug statistics
make FASTWAIT=1    # controlled cache-flush experiment
make DEBUG=1 FASTWAIT=1
```

`FASTWAIT=1` omits the destination-cache flush after FIFO-empty and engine-idle
completion. It is an experiment, not the default release configuration.

## Installation

Install `Radeon9200.chip` and `Prometheus.card` as a matched pair. The active
monitor icon is `DEVS:Monitors/Radeon.info`; an icon beside the chip library is
not used. Preserve its binary `DiskObject` by editing it with Workbench
**Information**.

The validated active ToolTypes are:

```text
BOARDTYPE=Prometheus
SETTINGSFILE=SYS:Devs/Picasso96Settings.9200
OUTPUT=VGA
DMASIZE=2M
CP=YES
HWSPRITE=YES
```

`BOARDTYPE` and `SETTINGSFILE` must be switched together. Use `OUTPUT=VGA` for
the first boot on an unvalidated board. `OUTPUT=DVI` is restricted to a
supported internal-TMDS COMBIOS profile. See [`tooltypes.md`](tooltypes.md) for
the complete option semantics and DVI validation record.

`DMASIZE` is required for Radeon initialization. It accepts positive decimal
bytes with an optional `K` or `M` suffix, rounds up to 4096 bytes, and must
leave at least 4 MiB for Picasso96. The reserved tail is published through the
Prometheus DMA vectors for peer PCI bus masters. Invalid, zero, unavailable,
or oversized requests fail initialization.

## Tests and performance

`build/p96screen` opens a `640x480` screen, draws and reads back test content,
and optionally runs format-specific fill, pattern, template, text, copy, mask,
and overlap checks:

```text
p96screen [8|16|32] [seconds] [test]
```

Run the complete `8/16/32/8` sequence before accepting a build. Additional
procedures are in [`p96speed.md`](p96speed.md),
[`p96overlap.md`](p96overlap.md), and
[`p96windowmove.md`](p96windowmove.md).

The repository does not claim an up-to-date 2D baseline for version 3.0: the
recorded P96Speed figures predate the latest state-shadowing, pipelining,
backpressure, DVI, and complete-copy changes. [`performance.md`](performance.md)
defines the metadata and reruns required before publishing new comparisons.
The latest recorded 3D checkpoint is a dated, artifact-identified 4.967 FPS
mean for fullscreen textured gears; it is not a general 2D score.

## Limits

- Legacy COMBIOS only; ATOM BIOS cold initialization is rejected.
- CRTC0 only. External TMDS, CRTC1, TV output, interrupts, overlays, and
  border/overscan programming are not implemented.
- Direct color is limited to RGB565PC and B8G8R8A8; no 15-bit, packed 24-bit,
  native-endian alias, or alternate channel ordering is advertised.
- Pattern acceleration is limited to supported JAM2 patterns. Planar conversion
  and unsupported templates or surfaces use P96 software.
- Linear scanout is limited to the lower 64 MiB aperture.
- Horizontal panning follows the CRTC's eight-byte granularity.
- One Radeon board instance is supported per chip-library load.
- Shutdown restores PCI command ownership and blanks output but does not restore
  every pre-existing Radeon register.

## Licensing

R200 command-processor microcode is distributed under
[`R200_MICROCODE_LICENSE.txt`](R200_MICROCODE_LICENSE.txt). Additional
attribution and binary redistribution terms are in
[`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).
