# Radeon9200 Prometheus integration 0.50

Release 0.50 ships the matched `Radeon9200.chip` and `Prometheus.card` pair.
The chip reports `$VER: Radeon9200.chip 3.2 (25.10.2026)` and keeps Radeon3D
interface 20 unchanged. The reference documentation is in
[`docs/`](docs/README.md).

## Highlights (0.50)

- Hardware planar blits: the chip installs `BlitPlanar2Chunky` and
  `BlitPlanar2Direct` (new `PLANAR` ToolType, default on, `PLANAR=NO`
  withholds them). Each selected plane is expanded by the 2D engine through
  `HOST_DATA0`; CLUT targets take one write-masked pass per plane, direct-colour
  targets an opaque pass plus transparent passes per plane subset. Four or more
  data planes on direct-colour targets stay with rtg.library's default, which
  is faster there. A 330x64 planar blit on the 24-bit screen drops from 24/26/27
  ms to 1.3/5.6/20 ms for one, two and three planes; on an 8-bit screen an
  8-plane blit drops from 19 ms to 8.8 ms. HippoPlayer's scope windows run at
  full rate.
- Readback fix: the host aperture keeps one read-ahead line, and a line the CPU
  read before an engine write to it was returned stale on the next readback
  (WaitBlit, delays and 68060 cache flushes did not clear it). The driver now
  drops that line after every engine drain, so `ReadPixel` and the software
  paths after a blit see VRAM. Pre-existing on every engine path.
- New `p2ctest` correctness suite (`make tools`): planar blits of depth 1-8 with
  every mask class, NULL and all-ones planes and unaligned offsets, pixel-exact
  against a graphics.library reference; 164 front-screen and 328 8/16-bit
  screen cases pass on the reference machine. Debug stats v25 add a Planar
  op slot.

Release 3.1 (kept below for context) shipped the matched `Radeon9200.chip` and `Prometheus.card` pair.
The chip reports `$VER: Radeon9200.chip 3.1 (19.10.2026)` and exposes
Radeon3D interface 20 with the semantic record path removed (the record
entry points are reserved LVO slots returning FALSE; the client is a
trusted cpemit producer on the indirect path). The reference documentation
is in [`docs/`](docs/README.md).

## Highlights (3.1)

- Semantic record path removal: `Radeon3DExecute`, `Radeon3DCommitDraw`,
  `Radeon3DCommitBatch`, `Radeon3DCommitStateBatch`, the record emitter,
  the `RADEON3D_EXEC_*` ABI and the record capability bits are gone; the
  surviving entry points keep every LVO offset.
- `r3dreplay` reworked onto the cpemit submit path as the standing
  back-pressure regression tool; semantic-era probes retired.
- Vendored MiniGL consumer header synced with the ABI change.

Release 3.0 (kept below for context) shipped interface 20.
The chip reported `$VER: Radeon9200.chip 3.0 (2.9.2026)` and exposed
Radeon3D interface 20.

## Highlights

- Radeon3D interface 20 adds the PPC engine lease
  (`RADEON3D_CAP_PPC_RING`): an exclusive ring lease under which the WarpOS
  PPC is the only ring writer, the 68k refuses submissions, and Picasso96
  2D falls back to software. `Radeon3DAcquireLease`/`Radeon3DReleaseLease`
  (LVOs -156/-162) plus `Radeon3DHeartbeatLease` (-168) carry the
  descriptors. Fallback flag: `make PPCRING=0` builds a fallback-only
  driver; see
  [`docs/09-ppc-direct-ring-design.md`](docs/09-ppc-direct-ring-design.md).
- The PPC direct-ring writer was hardware-validated on the physical
  machine: 32 fenced PACKET2 batches submitted locally and retired in
  ~125-200 us, with the 68k cross-checking the lease's last fence.
- Radeon3D interface 19 carries the parked fence-coalescing experiment
  (`RADEON3D_INDIRECT_NO_FENCE` + `Radeon3DSubmitFence`). It is default-off
  and inert on the shipped stack; see
  [`docs/07-history.md`](docs/07-history.md#31-interface-19-fence-coalescing-parked).
- Interface 18 trusted indirect dispatch with render transitions, descriptor
  snapshotting, post-prepare revalidation and ranged fence validation
  (`RADEON3D_CAP_MULTI_FENCE`).
- Interface 17 auxiliary render-surface allocation from driver-owned VRAM,
  preventing depth buffers from overlapping Picasso96 screen buffers.
- Release-safe per-submission EClock telemetry and capture/plot tooling.
- Pre-serialized Radeon3D replay benchmark for isolating driver and
  command-processor throughput.
- Bounded surface validation, session generation checks, submission fences,
  recovery, and failure attribution for rejected submissions.
- Picasso96 display, cursor, 2D acceleration, VGA, and validated internal-TMDS
  DVI support retained from 0.21.

## Configuration

Install `Radeon9200.chip` and `Prometheus.card` as a matched pair. Monitor icons
use `BOARDTYPE=Prometheus` with
`SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`. A valid positive `DMASIZE` is
required. `CP=YES` enables Radeon3D; `OUTPUT=VGA` remains the conservative
default, while `OUTPUT=DVI` requires a supported internal-TMDS COMBIOS profile.

See [`docs/05-build-deploy-run.md`](docs/05-build-deploy-run.md) for complete
configuration, installation and recovery precautions.

## Validation

Validated on a physical 50 MHz 68060 Amiga with a Prometheus/FireBird bridge
and RV280 Radeon 9200. The MiniGL 800x600x32 fullscreen suite passes 24/24;
dynamic lightmap updates pass without commit failures. Release and debug builds
use the pinned GCC 6.5.0b toolchain in CI.

Recorded performance figures and their artifact metadata are in
[`docs/04-performance.md`](docs/04-performance.md). There is no current
published 2D baseline for this tree.

This remains an experimental driver. Use a recoverable setup for first boot on
new hardware.
