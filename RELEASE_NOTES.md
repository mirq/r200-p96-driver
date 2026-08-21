# Radeon9200 Prometheus integration 0.21

Release 0.21 ships the matched `Radeon9200.chip` and `Prometheus.card` pair.
The chip reports `$VER: Radeon9200.chip 3.0 (15.8.2026)` and exposes Radeon3D
interface 8.

## Highlights

- Keeps Prometheus responsible for Radeon discovery, PCI ownership, bridge
  setup, and publication of the shared high-VRAM DMA arena.
- Supports CRTC0 VGA output and the validated RV280 internal-TMDS DVI route,
  including BIOS-derived transmitter clocks up to the 165 MHz single-link
  limit. A 1920x1080 Workbench was physically validated.
- Supports CLUT8, RGB565PC, and B8G8R8A8 scanout, palette, panning, DPMS, and a
  64x64 ARGB hardware cursor.
- Accelerates fills, inversion, same-surface copies, lines, supported patterns
  and templates, and complete copies for all 16 four-bit Picasso96 minterms.
  On-board surface extents and arithmetic are checked before submission;
  overlapping ranges select a safe copy direction.
- Uses cached 2D state, pipelined submissions, hardware FIFO backpressure, and
  skips unnecessary drains for safe off-board software fallbacks.
- Provides bounded Radeon3D sessions, trusted command staging, fences, P96
  surface import, recovery, and semantic execution. Interface 7 adds native
  triangle-strip and triangle-fan records; interface 8 adds native quad-list
  records and perspective-correct v5 draws. The batch cap remains 8192 dwords.
- Invalidates stale sessions on detach or recovery and restores a safe direct
  MMIO baseline before later Picasso96 work.
- Includes GCC/vbcc ABI fixtures and runtime probes for discovery, sessions,
  submissions, fences, imported formats, and recovery.

## Configuration

Monitor icons use `BOARDTYPE=Prometheus` together with
`SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`. A valid positive `DMASIZE` is
required. `CP=YES` enables the active Radeon3D service; Picasso96 2D remains on
the direct-MMIO path. `OUTPUT=VGA` is the conservative default and
`OUTPUT=DVI` requires a supported internal-TMDS COMBIOS profile.

See `tooltypes.md` for complete semantics and recovery precautions.

## Validation and performance

The current source has extensive physical 2D/3D correctness records, but no
complete version-3.0 2D benchmark baseline. Older P96Speed figures are not
promoted as release performance. `performance.md` defines the reproducible
baseline procedure and required artifact metadata.

The latest identified 3D checkpoint averaged 4.967 FPS in fullscreen textured
gears on the 50 MHz 68060/RV280 reference machine and passed the Phase 1 and
MiniGL Phase 4-6 suites. That number applies only to the hashes recorded in
`R200_3D_PROGRESS.md`.

This remains an experimental driver. Use a recoverable setup for first boot on
new hardware.
