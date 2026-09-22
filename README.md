# Radeon9200.chip for Prometheus.card

Pure-C Picasso96 chip driver for desktop RV280 Radeon 9200 PCI boards behind
`Prometheus.card`, plus the **Radeon3D** service that 68k and PPC/WarpOS
clients use for hardware 2D and 3D.

## What it is and how it works

The driver is a 68k AmigaOS shared library (`Radeon9200.chip`) loaded by
Picasso96 as `LIBS:Picasso96/Radeon9200.chip`. It owns the card:

- `Prometheus.card` enumerates PCI, claims the Radeon, sets up address
  translation and the shared DMA arena, and passes a validated handoff in
  `BoardInfo.CardData`.
- `Radeon9200.chip` initializes the RV280 from the legacy COMBIOS, programs
  CRTC0 output (VGA or validated internal TMDS/DVI), drives the hardware
  cursor, and accelerates Picasso96 2D directly through memory-mapped MMIO.
- For 3D it initializes the R200 command processor (CP), keeps a 1 MiB ring in
  private VRAM, and exposes the **Radeon3D service** through library vectors.

The service is bounded and semantic. A client submits either host-endian
*semantic records* (clear, draw, state) or *streaming commits* whose vertices
live in a service-owned VRAM segment. The driver validates the input, the
dual-target emitter (`src/radeon3d_emit.c`) turns it into an R200 command
stream, and the CP executes it. Every submission is retired by a *fence*; a
failed submit runs bounded recovery (invalidate sessions, reset the engine,
reload and self-test the CP).

**PPC/WarpOS clients never touch PCI, MMIO, the CP ring or VRAM allocation.**
They open `Radeon9200.chip` by name, open a Radeon3D session, import or
allocate surfaces, write vertices into leased segments (byte-swapped and
cache-flushed), and submit through `Radeon3DExecute` /
`Radeon3DCommitBatch` / `Radeon3DCommitStateBatch` (or the trusted
`Radeon3DDispatchIndirect` packet path). The MiniGL WarpOS frontend reaches
the 68k service through an Exec message-port host; native 68k clients call the
vectors directly.

Start with the documentation:

| Document | For |
|---|---|
| [`docs/01-ppc-client-guide.md`](docs/01-ppc-client-guide.md) | PPC/WarpOS integration: cross-CPU model, memory/cache/endianness, sessions, submission paths, fences |
| [`docs/02-service-abi-reference.md`](docs/02-service-abi-reference.md) | Full ABI: LVOs, capabilities, info block, structures, record formats, limits |
| [`docs/03-driver-architecture.md`](docs/03-driver-architecture.md) | 68k internals: init, locks, CP, 2D engine, display, BIOS, cursor, VRAM |
| [`docs/04-performance.md`](docs/04-performance.md) | Measured costs in microseconds/cycles, bandwidth, FPS ceilings, methodology |
| [`docs/05-build-deploy-run.md`](docs/05-build-deploy-run.md) | Build matrix, ToolTypes, installation, recovery layers |
| [`docs/06-testing.md`](docs/06-testing.md) | Probes, acceptance procedures, baseline rules |
| [`docs/07-history.md`](docs/07-history.md) | Interface evolution, decisions, rejected/parked work, open candidates |
| [`docs/08-troubleshooting.md`](docs/08-troubleshooting.md) | Failure stages, artifacts, recovery |

## Status

The current library is `Radeon9200.chip 3.0` and exposes **Radeon3D interface
19**. It has been validated on a physical 50 MHz 68060 Amiga with a
Prometheus/FireBird bridge, an RV280 `1002:5964` 128 MiB COMBIOS board, and a
64 MiB linear aperture. Supported device IDs are `1002:5960`, `1002:5961`, and
`1002:5964`.

Interface 19 adds the parked fence-coalescing experiment (default-off and not
deployable; see [`docs/07-history.md`](docs/07-history.md#31-interface-19-fence-coalescing-parked)).
The vendored MiniGL consumer header is at interface 17 and remains compatible:
it simply does not receive the 18/19 capabilities.

Implemented display and 2D features:

- CRTC0 through the primary VGA DAC and the validated internal-TMDS DVI route
  (stable 1920x1080 up to the 165 MHz single-link limit).
- CLUT8, little-endian RGB565 (`RGBFB_R5G6B5PC`), and four-byte BGRA/XRGB8888
  (`RGBFB_B8G8R8A8`) scanout, with panning, palette, blanking, vertical sync
  and DPMS callbacks.
- Bounded ROM selection, legacy COMBIOS parsing, cold-card initialization and
  endian-safe mapped MMIO.
- Hardware `FillRect`, `InvertRect`, `BlitRect`, `DrawLine`, JAM1/JAM2
  `BlitTemplate`, constrained JAM2 `BlitPattern`, and
  `BlitRectNoMaskComplete` for all 16 P96 minterms, with safe overlap
  direction selection.
- A 64x64 ARGB hardware cursor, enabled by default.
- A page-aligned shared Prometheus DMA arena at the high end of VRAM,
  separate from Radeon-private CP, segment, aux and cursor storage.

Implemented Radeon3D features (interface-gated, see
[`docs/02-service-abi-reference.md`](docs/02-service-abi-reference.md)):

- Bounded semantic records: clears, triangles, strips, fans, quads, points,
  lines; RGB565/BGRA/CLUT8 targets; textures with mipmaps, wrap, filters,
  alpha test and blending; fog and two texture units.
- Hardware transform/clip, texgen (object-linear and sphere-map), per-vertex
  normals and fixed-function lighting.
- Streaming segments and header-only vertex-fetch commits; homogeneous state
  batches and reuse records.
- Trusted indirect CP packet dispatch for validated producers.
- Session generations, ordered commits, ranged fences, per-submission timing
  telemetry and recovery.

Hardware acceleration falls back to the P96 defaults when an operation or
surface cannot be represented safely.

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
make DEBUG=1          # Radeon9200-debug.chip + Radeon9200.Debug stats port
make FASTWAIT=1       # controlled cache-flush experiment
make DEBUG=1 PROBES=1 # DEBUG plus boot-time engine probes (see docs; not for deployment)
```

## Installation

Install `Radeon9200.chip` and `Prometheus.card` as a matched pair. The active
monitor icon is `DEVS:Monitors/Radeon.info`; preserve its binary `DiskObject`
by editing it with Workbench **Information**. The validated ToolTypes are:

```text
BOARDTYPE=Prometheus
SETTINGSFILE=SYS:Devs/Picasso96Settings.9200
OUTPUT=VGA
DMASIZE=2M
CP=YES
HWSPRITE=YES
```

`BOARDTYPE` and `SETTINGSFILE` must be switched together. `DMASIZE` is required
for Radeon initialization and must leave at least 4 MiB for Picasso96.
`CP=YES` is required for Radeon3D. Full option semantics, the DVI validation
record, and the automatic/manual recovery layers are in
[`docs/05-build-deploy-run.md`](docs/05-build-deploy-run.md).

## Tests and performance

`build/p96screen` opens a 640x480 screen, draws and reads back test content,
and optionally runs format-specific fill, pattern, template, text, copy, mask
and overlap checks:

```text
p96screen [8|16|32] [seconds] [test]
```

Run the complete `8/16/32/8` sequence before accepting a build. 3D service
probes (`radeon3dinfo`, `radeon3dsessions`, `radeon3dphase1`,
`radeon3dformats`, `r3dreplay`, `r3dib`, `r3dtexupdate`) and the benchmark
procedures are listed in [`docs/06-testing.md`](docs/06-testing.md).

Measured performance - PCI aperture ~6.1-6.3 MB/s, ~30 cycles per aperture
dword, full GPU idle drains per fenced submission, and the recorded FPS
checkpoints - is in [`docs/04-performance.md`](docs/04-performance.md). There
is no current published 2D baseline for the interface-19 tree; older figures
are explicitly labelled historical.

## Limits

- Legacy COMBIOS only; ATOM BIOS cold initialization is rejected.
- CRTC0 only. External TMDS, CRTC1, TV output, interrupts, overlays and
  border/overscan programming are not implemented.
- Direct color is limited to RGB565PC and B8G8R8A8; no 15-bit, packed 24-bit,
  native-endian alias or alternate channel ordering is advertised.
- Pattern acceleration is limited to supported JAM2 patterns; planar
  conversion and unsupported templates/surfaces use P96 software.
- Linear scanout is limited to the lower 64 MiB aperture.
- Horizontal panning follows the CRTC's eight-byte granularity.
- One Radeon board instance per chip-library load.
- `TEXTSTAGE=YES` is a known-broken experiment; leave it off.
- Shutdown restores PCI command ownership and blanks output but does not
  restore every pre-existing Radeon register.

## Licensing

R200 command-processor microcode is distributed under
[`R200_MICROCODE_LICENSE.txt`](R200_MICROCODE_LICENSE.txt). Additional
attribution and binary redistribution terms are in
[`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).
