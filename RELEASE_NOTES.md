# Radeon9200 Prometheus integration 3.0

Release 3.0 ships the matched `Radeon9200.chip` and `Prometheus.card` pair.
The chip reports `$VER: Radeon9200.chip 3.0 (2.9.2026)` and exposes Radeon3D
interface 19. The reference documentation is in [`docs/`](docs/README.md).

## Highlights

- Radeon3D interface 19 adds the parked fence-coalescing experiment
  (`RADEON3D_INDIRECT_NO_FENCE` + `Radeon3DSubmitFence`). It is default-off and
  inert on the shipped stack; see
  [`docs/07-history.md`](docs/07-history.md#31-interface-19-fence-coalescing-parked).
- Interface 18 trusted indirect dispatch with render transitions, descriptor
  snapshotting, post-prepare revalidation and ranged fence validation
  (`RADEON3D_CAP_MULTI_FENCE`).
- Interface 17 auxiliary render-surface allocation from driver-owned VRAM,
  preventing depth buffers from overlapping Picasso96 screen buffers.
- Release-safe per-submission EClock telemetry and capture/plot tooling.
- Pre-serialized Radeon3D replay benchmark for isolating driver and
  command-processor throughput.
- Immutable record-chain emission, ordered commits, state batching, and
  client-written VRAM streaming segments.
- Bounded surface validation, session generation checks, submission fences,
  recovery, and failure attribution for rejected semantic commits.
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
