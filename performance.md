# Performance and validation

This file describes the current performance status and the evidence required
for a publishable result. It replaces the former cumulative optimization diary;
superseded experiments and plans remain available in Git history.

## Current status

There is no version-3.0 2D baseline yet. Existing P96Speed, overlap, and moving
window results were collected from older artifacts before the latest 2D state
shadowing, pipelining, hardware FIFO backpressure, fallback-drain, all-minterm,
and DVI changes. They remain useful historical evidence in the individual tool
documents, but must not be described as current driver performance.

The newest artifact-identified 3D checkpoint was measured on the physical
68060/RV280 system on 17 August 2026. Fullscreen textured gears produced 4.919,
4.925, and 4.946 FPS (mean **4.930 FPS**) with three submissions per frame and
an 8192-dword batch limit. A later interface-8 quad-list checkpoint measured
4.971 and 4.963 FPS (mean **4.967 FPS**) on its separately identified binaries.
Both checkpoints passed their recorded Phase 1 and Phase 4-6 acceptance suites.
See [`R200_3D_PROGRESS.md`](R200_3D_PROGRESS.md) for artifacts and scope.

These figures describe those exact binaries. They are not estimates for an
uncommitted working tree or a later release.

## Required metadata

Every accepted run must record:

- Git commit and whether the working tree was clean.
- Size, SHA-256, and CRC32 of `Radeon9200.chip` and `Prometheus.card`.
- Release, DEBUG, FASTWAIT, or other compile-time configuration.
- CPU and clock, bridge model, Radeon PCI ID/revision, VRAM, ROM type, and
  `rtg.library`/benchmark versions.
- Complete active monitor ToolTypes and Workbench/benchmark modes.
- EClock frequency, cold/warm boot state, run order, raw output, and all
  individual samples.

Do not combine numbers from differently identified artifacts. Report median,
mean, range, and individual trials where repeated measurements are available.

## Version-3.0 baseline procedure

1. Build a clean release pair and record the metadata above.
2. Cold boot with the validated ToolTypes.
3. Run `p96screen` with `test` in the order 8/16/32/8. Reject the build on any
   readback, edge, mask, fallback, or recovery failure.
4. Run P96Speed 1.2 at `640x480x16` for 13 seconds per test, at least three
   times under the same conditions. Preserve each 1,889-byte raw report. Follow
   [`p96speed.md`](p96speed.md).
5. Cold reboot before focused tests; the long P96Speed session has historically
   changed otherwise stable timings after its window closes.
6. Run current `p96overlap` twice and retain every trial and `ClipRect` count.
   Follow [`p96overlap.md`](p96overlap.md).
7. Run current `p96windowmove` twice and retain `TOTAL`, `STAGES`, `TRIALS`, and
   `RESULT` lines. Follow [`p96windowmove.md`](p96windowmove.md).
8. For a 3D release, run `radeon3dinfo`, `radeon3dphase1`,
   `radeon3dsessions`, `radeon3dformats`, and the current MiniGL acceptance
   executables. Record exact artifact hashes and summary lines.

## Focused attribution

`p96screen [8|16|32] <seconds> test` reports focused complete-copy, fill,
scatter-fill, template, text, and copy measurements. A DEBUG build publishes a
versioned `Radeon9200.Debug` block; decode it with
`tools/decode_debug_stats.py` and take deltas around exactly one workload.

Useful DEBUG evidence includes callback calls/hardware/software/ticks, FIFO and
idle polls, timeout/recovery counts, complete-copy opcode distribution,
validation and submission timing, MMIO/aperture costs, and fallback drains.
DEBUG timings are diagnostic and must not be mixed with release performance.

## Historical findings still relevant to design

- Direct mapped MMIO and checked 32-bit surface validation produced the large
  early `FillRect` improvement over software rendering.
- Cross-surface acceleration removed expensive backing-store defaults;
  supporting source-XOR-destination reduced a measured mover-open stage from
  roughly 9.5 seconds to 11.7 ms on its historical test artifacts.
- Repeated `HOST_DATA0` template submission beat per-word FIFO polling on the
  reference machine and became the normal host-data implementation.
- The experimental `TEXTSTAGE=YES` VRAM expansion path wedged the 2D engine and
  remains off by default.
- Removing required CP/aperture ordering barriers or treating a successful
  build as hardware proof is not an acceptable optimization.

Historical numbers above explain retained implementation choices; they are not
the version-3.0 baseline.
