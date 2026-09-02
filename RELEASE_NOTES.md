# Radeon9200 Prometheus integration 0.31

Release 0.31 ships the matched `Radeon9200.chip` and `Prometheus.card` pair.
The chip reports `$VER: Radeon9200.chip 3.0 (2.9.2026)` and exposes Radeon3D
interface 17.

## Highlights

- Adds interface-17 auxiliary render-surface allocation from driver-owned VRAM,
  preventing depth buffers from overlapping Picasso96 screen buffers.
- Adds release-safe per-submission EClock telemetry and capture/plot tooling.
- Adds the pre-serialized Radeon3D replay benchmark for isolating driver and
  command-processor throughput.
- Preserves immutable record-chain emission, ordered commits, state batching,
  and client-written VRAM streaming segments.
- Retains bounded surface validation, session generation checks, submission
  fences, recovery, and failure attribution for rejected semantic commits.
- Keeps Picasso96 display, cursor, 2D acceleration, VGA, and validated
  internal-TMDS DVI support from 0.21.

## Configuration

Install `Radeon9200.chip` and `Prometheus.card` as a matched pair. Monitor icons
use `BOARDTYPE=Prometheus` with
`SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`. A valid positive `DMASIZE` is
required. `CP=YES` enables Radeon3D; `OUTPUT=VGA` remains the conservative
default, while `OUTPUT=DVI` requires a supported internal-TMDS COMBIOS profile.

See `tooltypes.md` for complete configuration and recovery precautions.

## Validation

Validated on a physical 50 MHz 68060 Amiga with a Prometheus/FireBird bridge
and RV280 Radeon 9200. The MiniGL 800x600x32 fullscreen suite passes 24/24;
dynamic lightmap updates pass without commit failures. Release and debug builds
use the pinned GCC 6.5.0b toolchain in CI.

This remains an experimental driver. Use a recoverable setup for first boot on
new hardware.
